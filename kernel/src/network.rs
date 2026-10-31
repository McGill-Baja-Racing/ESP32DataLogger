//! The master's network: the Wi-Fi coprocessor as an open access point through esp-hosted over
//! SDIO, with the interface and DHCP server in front of it, and the ESP-IDF calls that set them up.
//!
//! One task runs all of it and never blocks. The HTTP handlers run on a task of their own (`http`).

use alloc::boxed::Box;
use core::{
    cell::RefCell,
    ffi::c_void,
    mem::replace,
    net::{Ipv4Addr, SocketAddr},
};

use critical_section::Mutex;
use edge_dhcp::{
    io::{DEFAULT_SERVER_PORT, server::run as run_dhcp},
    server::{Server as DhcpServer, ServerOptions},
};
use edge_nal::{UdpReceive, UdpSend};
use edge_nal_embassy::TcpBuffers;
use embassy_executor::task;
use embassy_futures::{join::join3, select::select3};
use embassy_net::{
    Config, Ipv4Cidr, StackResources, StaticConfigV4,
    udp::{PacketMetadata, UdpSocket},
};
use embassy_net_esp_hosted::{Control, NetDriver, State};
use embassy_sync::{blocking_mutex::raw::CriticalSectionRawMutex, signal::Signal};
use embassy_time::Timer;
use embedded_io_async::{ErrorKind, ErrorType};
use esp_hal::{
    Async,
    gpio::{Level, Output, OutputConfig},
    peripherals::{GPIO14, GPIO15, GPIO16, GPIO17, GPIO18, GPIO19, GPIO54},
    rng::Rng,
    sdmmc::{Slot, SlotConfig},
};
use esp_radio_rtos_driver as rtos;
use esp_rtos::embassy::Executor;
use macros::export;
use sys::{
    ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED,
    ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, esp_err_t, esp_ip4_addr_t, esp_netif_ip_info_t,
    wifi_ap_config_t, wifi_auth_mode_t, wifi_config_t, wifi_init_config_t, wifi_interface_t,
    wifi_mode_t, wifi_osi_funcs_t, wpa_crypto_funcs_t,
};

use crate::{http, rtos::ISR_STACK, sdmmc};

/// Above every C task, so the network is never starved.
const PRIORITY: u32 = 11;
/// The deepest call chain measures about 5 KiB of frames, with the interrupt allowance.
const STACK: usize = 6 * 1024 + ISR_STACK;
/// The coprocessor's SDIO clock.
const SDIO_HZ: u32 = 40_000_000;
/// The HTTP server's sockets and the DHCP server's.
const SOCKETS: usize = http::CONNECTIONS + 1;
/// The DHCP server's socket buffers and message buffer; a larger datagram is dropped, not read.
const DHCP: usize = 576;
/// Leases are short: the table is keyed by client-supplied addresses, and a lease is held until it
/// expires.
const LEASES: usize = 16;
const LEASE_SECS: u32 = 60;
/// ESP-IDF's access point defaults.
const AP_IP: esp_ip4_addr_t = esp_ip4_addr_t {
    addr: u32::from_le_bytes([192, 168, 4, 1]),
};

/// What C has set up so far; `esp_wifi_start` hands it to the network task.
struct Settings {
    hosted: bool,
    started: bool,
    dhcps_stopped: bool,
    ip_info: esp_netif_ip_info_t,
    ap: Option<wifi_ap_config_t>,
}

static SETTINGS: Mutex<RefCell<Settings>> = Mutex::new(RefCell::new(Settings {
    hosted: false,
    started: false,
    dhcps_stopped: false,
    ip_info: esp_netif_ip_info_t {
        ip: AP_IP,
        netmask: esp_ip4_addr_t {
            addr: u32::from_le_bytes([255, 255, 255, 0]),
        },
        gw: AP_IP,
    },
    ap: None,
}));

fn settings<R>(f: impl FnOnce(&mut Settings) -> R) -> R {
    critical_section::with(|cs| f(&mut SETTINGS.borrow_ref_mut(cs)))
}

/// What `esp_wifi_start` hands the network task.
struct Start {
    ap: wifi_ap_config_t,
    cidr: Ipv4Cidr,
    gateway: Ipv4Addr,
    dhcp: bool,
}

static START: Signal<CriticalSectionRawMutex, Start> = Signal::new();

type Hosted = (Slot<'static, 1, Async>, Output<'static>);

#[export]
fn esp_hosted_init() -> Result<(), esp_err_t> {
    if settings(|settings| replace(&mut settings.hosted, true)) {
        return Err(ESP_ERR_INVALID_STATE);
    }
    let controller = sdmmc::controller()?;
    // SAFETY: slot 1, its pins and the coprocessor's reset pin are esp-hosted's alone, and
    // `hosted` lets this run once.
    let hosted: Hosted = unsafe {
        (
            controller
                .slot::<1>(SlotConfig::default())
                .map_err(|_slot| ESP_ERR_INVALID_STATE)?
                .with_clk(GPIO18::steal())
                .with_cmd(GPIO19::steal())
                .with_data0(GPIO14::steal())
                .with_data1(GPIO15::steal())
                .with_data2(GPIO16::steal())
                .with_data3(GPIO17::steal())
                .into_async(),
            Output::new(GPIO54::steal(), Level::High, OutputConfig::default()),
        )
    };
    let hosted = Box::into_raw(Box::new(hosted)).cast();
    // SAFETY: the task is the only user of `hosted`.
    unsafe { rtos::task_create("network", run, hosted, PRIORITY, None, STACK) };
    Ok(())
}

extern "C" fn run(hosted: *mut c_void) {
    // SAFETY: `esp_hosted_init` leaked this `Hosted` for this task alone.
    let (slot, reset) = *unsafe { Box::from_raw(hosted.cast::<Hosted>()) };
    Box::leak(Box::new(Executor::new())).run(|spawner| {
        if let Ok(token) = network(slot, reset) {
            spawner.spawn(token);
        }
    });
}

/// Runs the coprocessor and, on `esp_wifi_start`, the access point on it; a coprocessor that
/// resets or stops answering is reset and the access point started again.
#[task]
async fn network(mut slot: Slot<'static, 1, Async>, mut reset: Output<'static>) {
    let start = START.wait().await;
    loop {
        reset.set_low();
        Timer::after_millis(100).await;
        reset.set_high();
        Timer::after_millis(1000).await;

        // Built in place, in the task's static storage.
        let mut state = State::new();
        let (net_device, control, mut runner) =
            embassy_net_esp_hosted::new(&mut state, &mut slot, SDIO_HZ);
        #[expect(
            clippy::large_futures,
            reason = "the task's storage is static, which Box::pin would take from the heap"
        )]
        select3(
            runner.run(),
            serve(&control, net_device, &start),
            control.watch(),
        )
        .await;
    }
}

/// The DHCP server's socket, for the IPv4-only stack.
struct DhcpSocket<'a>(UdpSocket<'a>);

impl ErrorType for DhcpSocket<'_> {
    type Error = ErrorKind;
}

impl UdpReceive for DhcpSocket<'_> {
    async fn receive(&mut self, buffer: &mut [u8]) -> Result<(usize, SocketAddr), ErrorKind> {
        let (len, from) = self
            .0
            .recv_from(buffer)
            .await
            .map_err(|_error| ErrorKind::Other)?;
        Ok((
            len,
            SocketAddr::new(from.endpoint.addr.into(), from.endpoint.port),
        ))
    }
}

impl UdpSend for DhcpSocket<'_> {
    async fn send(&mut self, remote: SocketAddr, data: &[u8]) -> Result<(), ErrorKind> {
        let SocketAddr::V4(remote) = remote else {
            return Err(ErrorKind::InvalidInput);
        };
        self.0
            .send_to(data, (*remote.ip(), remote.port()))
            .await
            .map_err(|_error| ErrorKind::Other)
    }
}

/// Starts the access point, then serves DHCP and HTTP on it.
async fn serve(control: &Control<'_>, device: NetDriver<'_>, start: &Start) {
    let ap = &start.ap;
    let ssid = ap.ssid.get(..usize::from(ap.ssid_len)).unwrap_or_default();
    if control
        .start_ap(ssid, ap.channel, ap.max_connection)
        .await
        .is_err()
    {
        return;
    }
    let mut resources = StackResources::<SOCKETS>::new();
    let mut seed = [0; 8];
    Rng::new().read(&mut seed);
    let config = Config::ipv4_static(StaticConfigV4 {
        address: start.cidr,
        gateway: None,
        dns_servers: <_>::default(),
    });
    let (stack, mut runner) =
        embassy_net::new(device, config, &mut resources, u64::from_le_bytes(seed));

    let tcp_buffers = TcpBuffers::<{ http::CONNECTIONS }, { http::TX }, { http::RX }>::new();
    let dhcp = async {
        if !start.dhcp {
            return;
        }
        let mut rx_meta = [PacketMetadata::EMPTY; 1];
        let mut tx_meta = [PacketMetadata::EMPTY; 1];
        let mut rx = [0; DHCP];
        let mut tx = [0; DHCP];
        let mut socket = UdpSocket::new(stack, &mut rx_meta, &mut rx, &mut tx_meta, &mut tx);
        if socket.bind(DEFAULT_SERVER_PORT).is_err() {
            return;
        }
        let mut socket = DhcpSocket(socket);
        let ip = start.cidr.address();
        let gateways = [start.gateway];
        let mut options = ServerOptions::new(ip, None);
        options.gateways = &gateways;
        options.subnet = Some(start.cidr.netmask());
        options.lease_duration_secs = LEASE_SECS;
        let mut server = DhcpServer::<_, LEASES>::new_with_et(ip);
        let mut buffer = [0; DHCP];
        loop {
            run_dhcp(&mut server, &options, &mut socket, &mut buffer)
                .await
                .ok();
            Timer::after_millis(100).await;
        }
    };
    join3(
        runner.run(),
        dhcp,
        http::serve(edge_nal_embassy::Tcp::new(stack, &tcp_buffers)),
    )
    .await;
}

#[export]
fn nvs_flash_init() -> Result<(), esp_err_t> {
    Ok(())
}

#[export]
#[expect(
    clippy::panic,
    reason = "C erases only after an init error, which never happens"
)]
fn nvs_flash_erase() -> Result<(), esp_err_t> {
    panic!("nvs_flash_init never asks for an erase")
}

#[export]
fn esp_netif_init() -> Result<(), esp_err_t> {
    Ok(())
}

#[export]
fn esp_event_loop_create_default() -> Result<(), esp_err_t> {
    Ok(())
}

/// The access point's interface, ESP-IDF's `esp_netif_t`.
struct Netif;

sys::c_handle!(Netif, *mut sys::esp_netif_t);

static NETIF: Netif = Netif;

#[export]
fn esp_netif_create_default_wifi_ap() -> &'static Netif {
    &NETIF
}

/// Changing the interface after the access point is up isn't supported.
fn before_start() {
    assert!(
        !settings(|settings| settings.started),
        "the kernel configures the interface only before esp_wifi_start"
    );
}

#[export]
fn esp_netif_dhcps_stop(_esp_netif: &Netif) -> Result<(), esp_err_t> {
    before_start();
    if settings(|settings| replace(&mut settings.dhcps_stopped, true)) {
        Err(ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    } else {
        Ok(())
    }
}

/// With the interface down, the server starts when the access point does.
#[export]
fn esp_netif_dhcps_start(_esp_netif: &Netif) -> Result<(), esp_err_t> {
    before_start();
    settings(|settings| settings.dhcps_stopped = false);
    Ok(())
}

/// `esp_ip4_addr_t`'s address is in network order, so its bytes in memory are the dotted ones.
fn address(ip: esp_ip4_addr_t) -> Ipv4Addr {
    Ipv4Addr::from(ip.addr.to_le_bytes())
}

#[export]
fn esp_netif_set_ip_info(
    _esp_netif: &Netif,
    ip_info: &esp_netif_ip_info_t,
) -> Result<(), esp_err_t> {
    before_start();
    settings(|settings| {
        if !settings.dhcps_stopped {
            return Err(ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED);
        }
        settings.ip_info = *ip_info;
        Ok(())
    })
}

#[export]
fn esp_wifi_init(_config: &wifi_init_config_t) -> Result<(), esp_err_t> {
    Ok(())
}

#[export]
fn esp_wifi_set_mode(mode: wifi_mode_t) -> Result<(), esp_err_t> {
    assert!(
        mode == wifi_mode_t::WIFI_MODE_AP,
        "the kernel supports only the access point mode"
    );
    Ok(())
}

#[export]
fn esp_wifi_set_config(
    interface: wifi_interface_t,
    conf: &mut wifi_config_t,
) -> Result<(), esp_err_t> {
    assert!(
        interface == wifi_interface_t::WIFI_IF_AP,
        "the kernel supports only the access point's interface"
    );
    // SAFETY: C filled in the access point's member for `WIFI_IF_AP`.
    let ap = unsafe { conf.ap };
    assert!(
        ap.authmode == wifi_auth_mode_t::WIFI_AUTH_OPEN
            && (1..=ap.ssid.len()).contains(&usize::from(ap.ssid_len)),
        "the kernel supports only an open access point with an SSID length"
    );
    before_start();
    settings(|settings| settings.ap = Some(ap));
    Ok(())
}

/// Returns once the access point is queued, as ESP-IDF does: it comes up on its own time, and is
/// brought up again if the coprocessor fails.
#[export]
fn esp_wifi_start() -> Result<(), esp_err_t> {
    let (ap, ip_info, dhcp) = settings(|settings| {
        settings
            .ap
            .filter(|_ap| !replace(&mut settings.started, true))
            .map(|ap| (ap, settings.ip_info, !settings.dhcps_stopped))
    })
    .ok_or(ESP_ERR_INVALID_STATE)?;
    let cidr = Ipv4Cidr::from_netmask(address(ip_info.ip), address(ip_info.netmask))
        .map_err(|_error| ESP_ERR_INVALID_ARG)?;
    START.signal(Start {
        ap,
        cidr,
        gateway: address(ip_info.gw),
        dhcp,
    });
    Ok(())
}

/// `WIFI_INIT_CONFIG_DEFAULT` names these; their contents are unused.
// SAFETY: `wifi_osi_funcs_t` is an opaque zero-sized type, which C only takes the address of.
#[unsafe(no_mangle)]
static g_wifi_osi_funcs: wifi_osi_funcs_t = unsafe { core::mem::zeroed() };

// SAFETY: all-zero is a valid `wpa_crypto_funcs_t`: no callbacks.
#[unsafe(no_mangle)]
static g_wifi_default_wpa_crypto_funcs: wpa_crypto_funcs_t = unsafe { core::mem::zeroed() };
