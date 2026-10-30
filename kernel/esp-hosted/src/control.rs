use alloc::vec::Vec;

use embassy_net_driver_channel as ch;
use embassy_net_driver_channel::driver::{HardwareAddress, LinkState};
use embassy_sync::{blocking_mutex::raw::NoopRawMutex, signal::Signal};
use embassy_time::{Duration, Timer, with_timeout};

use crate::ioctl::Shared;
use crate::proto::{self, Value};

/// The coprocessor refused a request, or answered it wrongly or not at all.
#[derive(Debug)]
pub struct Error;

/// A lost frame would otherwise hang the caller forever.
const RPC_TIMEOUT: Duration = Duration::from_secs(5);
/// How often [`Control::watch`] asks the coprocessor something.
const HEARTBEAT: Duration = Duration::from_secs(10);

const WIFI_INTERFACE_AP: u64 = 1;
const WIFI_MODE_AP: u64 = 2;
/// `wifi_init_config` as `(field, value)` with the `CONFIG_ESP_WIFI_*` values of the kernel's
/// `sdkconfig.h`, which C's `WIFI_INIT_CONFIG_DEFAULT` expands; zero fields are left out.
const WIFI_INIT_CONFIG: [(u32, u64); 18] = [
    (1, 10),                       // static_rx_buf_num
    (2, 32),                       // dynamic_rx_buf_num
    (3, 1),                        // tx_buf_type: dynamic
    (4, 10),                       // static_tx_buf_num
    (5, 32),                       // dynamic_tx_buf_num
    (8, 1),                        // ampdu_rx_enable
    (9, 1),                        // ampdu_tx_enable
    (11, 1),                       // nvs_enable
    (13, 6),                       // rx_ba_win
    (15, 752),                     // beacon_max_len
    (16, 32),                      // mgmt_sbuf_num
    (17, 1 | (1 << 5) | (1 << 8)), // feature_caps: wpa3_sae, gmac, bss_max_idle
    (18, 1),                       // sta_disconnected_pm
    (19, 7),                       // espnow_max_encrypt_num
    (20, 0x1F2F_3F4F),             // magic
    (21, 1),                       // rx_mgmt_buf_type: dynamic
    (22, 32),                      // rx_mgmt_buf_num
    (23, 3),                       // tx_hetb_queue_num
];

pub struct Control<'a> {
    state_ch: ch::StateRunner<'a>,
    shared: &'a Shared,
    up: Signal<NoopRawMutex, ()>,
}

impl<'a> Control<'a> {
    pub(crate) fn new(state_ch: ch::StateRunner<'a>, shared: &'a Shared) -> Self {
        Self {
            state_ch,
            shared,
            up: Signal::new(),
        }
    }

    /// Waits for the coprocessor, then starts an open access point on `channel` and brings the
    /// link up.
    ///
    /// # Errors
    ///
    /// If the coprocessor refuses a request or answers it wrongly.
    pub async fn start_ap(
        &self,
        ssid: &[u8],
        channel: u8,
        max_connections: u8,
    ) -> Result<(), Error> {
        self.shared.init_wait().await;

        let mut config = Vec::new();
        for (field, value) in WIFI_INIT_CONFIG {
            proto::number(&mut config, field, value);
        }
        let mut message = Vec::new();
        proto::bytes(&mut message, 1, &config);
        self.call(proto::WIFI_INIT, &message, 1).await?;

        let mut message = Vec::new();
        proto::number(&mut message, 1, WIFI_MODE_AP);
        self.call(proto::SET_MODE, &message, 1).await?;

        let mut ap = Vec::new();
        proto::bytes(&mut ap, 1, ssid);
        proto::number(&mut ap, 3, ssid.len() as u64);
        proto::number(&mut ap, 4, channel.into());
        proto::number(&mut ap, 7, max_connections.into());
        let mut config = Vec::new();
        proto::bytes(&mut config, 1, &ap);
        let mut message = Vec::new();
        proto::number(&mut message, 1, WIFI_INTERFACE_AP);
        proto::bytes(&mut message, 2, &config);
        self.call(proto::WIFI_SET_CONFIG, &message, 1).await?;

        self.call(proto::WIFI_START, &[], 1).await?;

        let address = self.mac_address().await?;
        self.state_ch
            .set_hardware_address(HardwareAddress::Ethernet(address));
        self.state_ch.set_link_state(LinkState::Up);
        self.up.signal(());
        Ok(())
    }

    /// Once the access point is up, returns when the coprocessor stops answering, which a working
    /// link never notices.
    pub async fn watch(&self) {
        self.up.wait().await;
        loop {
            Timer::after(HEARTBEAT).await;
            if self.mac_address().await.is_err() {
                return;
            }
        }
    }

    async fn mac_address(&self) -> Result<[u8; 6], Error> {
        let mut message = Vec::new();
        proto::number(&mut message, 1, WIFI_INTERFACE_AP);
        let response = self.call(proto::GET_MAC_ADDRESS, &message, 2).await?;
        let Some(Value::Bytes(address)) = proto::find(&response, 1) else {
            return Err(Error);
        };
        address.first_chunk().copied().ok_or(Error)
    }

    /// Sends request `id` and returns the response's message, whose field `status` is the
    /// coprocessor's error code.
    async fn call(&self, id: u32, message: &[u8], status: u32) -> Result<Vec<u8>, Error> {
        let response = with_timeout(RPC_TIMEOUT, self.shared.ioctl(proto::request(id, message)))
            .await
            .map_err(|_timeout| Error)?;

        // A response's id is its request's plus 256.
        if !matches!(proto::find(&response, 2), Some(Value::Varint(n)) if n == u64::from(id) + 256)
        {
            return Err(Error);
        }
        let Some(Value::Bytes(message)) =
            proto::fields(&response).find_map(|(field, value)| (field > 3).then_some(value))
        else {
            return Err(Error);
        };
        match proto::find(message, status) {
            Some(Value::Varint(code)) if code != 0 => Err(Error),
            _ => Ok(message.to_vec()),
        }
    }
}
