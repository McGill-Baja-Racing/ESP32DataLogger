//! embassy-net driver for esp-hosted-mcu over SDIO, as an open access point.
//!
//! This is `embassy-net-esp-hosted` at embassy commit b74e728, cut to what the access point needs
//! (no station mode, other transports, Bluetooth, OTA or esp-hosted-fg), with the access point
//! added after embassy-rs/embassy#3972. Every file is modified.
#![no_std]

extern crate alloc;

use ::sdio::MmcBus;
use aligned::Aligned;
use core::convert::Infallible;

use embassy_futures::select::{Either3, select, select3};
use embassy_net_driver_channel as ch;
use embassy_net_driver_channel::driver::HardwareAddress;
use embassy_time::Timer;

use crate::ioctl::Shared;

mod control;
mod iface;
mod ioctl;
mod proto;

pub use control::{Control, Error};
use iface::SdioInterface;

const MTU: usize = 1514;

/// A frame is this header, then its payload: interface type in the low 4 bits of byte 0, flags,
/// then little-endian length, offset (always `OFFSET`), checksum and sequence number, and 2 bytes
/// of padding. A fragmented frame (flags bit 0) isn't reassembled.
const OFFSET: u16 = 12;
/// `CONFIG_ESP_HOSTED_SDIO_CHECKSUM`; must match the coprocessor. Off, the field is sent as 0.
const CHECKSUM: bool = false;
const HEADER: usize = OFFSET as usize;

/// esp-hosted-mcu's interface types.
const IF_AP: u8 = 2;
const IF_SERIAL: u8 = 3;

/// A whole number of SDIO blocks, holding a header and a packet.
const MAX_BUFFER_SIZE: usize = 1536;

/// The RPC framing of esp-hosted-mcu: a type-length-value header before a request or response.
const RPC_RESPONSE: &[u8; 10] = b"\x01\x06\x00RPCRsp\x02";
const RPC_EVENT: &[u8; 10] = b"\x01\x06\x00RPCEvt\x02";

/// How often an idle link is polled, which also notices a coprocessor that stopped answering.
const POLL_SECS: u64 = 1;
/// Failed transfers in a row that count as a coprocessor reset.
const MAX_FAILURES: u8 = 3;

/// State for the esp-hosted driver.
pub struct State {
    shared: Shared,
    ch: ch::State<MTU, 2, 2>,
}

impl State {
    /// Create a new state.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            shared: Shared::new(),
            ch: ch::State::new(),
        }
    }
}

impl Default for State {
    fn default() -> Self {
        Self::new()
    }
}

/// Network device for use with embassy-net.
pub type NetDriver<'a> = ch::Device<'a, MTU>;

/// Handles returned by [`new`] for interacting with the esp-hosted driver.
pub struct HostedResources<'a, B: MmcBus> {
    /// Network device for use with embassy-net.
    pub net_device: NetDriver<'a>,

    /// Control handle for starting the access point.
    pub control: Control<'a>,

    /// Runner driving communication with the coprocessor. Must be polled.
    pub runner: Runner<'a, B>,
}

/// Create a new esp-hosted driver around `bus`, with the coprocessor already reset.
pub fn new<B: MmcBus>(state: &mut State, bus: B, freq_hz: u32) -> HostedResources<'_, B> {
    let (ch_runner, device) = ch::new(&mut state.ch, HardwareAddress::Ethernet([0; 6]));
    let state_ch = ch_runner.state_runner();

    let runner = Runner {
        ch: ch_runner,
        shared: &state.shared,
        next_seq: 1,
        failures: 0,
        iface: SdioInterface::new(bus, freq_hz),
    };

    HostedResources {
        net_device: device,
        control: Control::new(state_ch, &state.shared),
        runner,
    }
}

/// The coprocessor stopped answering or restarted, and has to be reset and started again.
#[derive(Debug)]
pub struct Reset;

/// Runner for communicating with the Wi-Fi coprocessor.
pub struct Runner<'a, B: MmcBus> {
    ch: ch::Runner<'a, MTU>,
    shared: &'a Shared,

    next_seq: u16,
    failures: u8,

    iface: SdioInterface<B>,
}

impl<B: MmcBus> Runner<'_, B> {
    /// Run the packet processing until the coprocessor needs a reset.
    ///
    /// # Errors
    ///
    /// When the coprocessor restarts or stops answering.
    pub async fn run(&mut self) -> Result<Infallible, Reset> {
        let mut buffer = Aligned([0u8; MAX_BUFFER_SIZE]);
        self.iface.init().await;

        loop {
            let ioctl = self.shared.ioctl_wait_pending();
            let tx = self.ch.tx_buf();
            let ev = select(self.iface.wait_for_ready(), Timer::after_secs(POLL_SECS));

            let seq = self.next_seq;
            let tx_len = match select3(ioctl, tx, ev).await {
                Either3::First(request) => {
                    let size = u16::try_from(request.len()).map(u16::to_le_bytes);
                    let sent = size.ok().and_then(|size| {
                        frame(
                            &mut *buffer,
                            IF_SERIAL,
                            seq,
                            &[RPC_RESPONSE, &size, &request],
                        )
                    });
                    if sent.is_none() {
                        self.shared.ioctl_done(&[]);
                    }
                    sent
                }
                Either3::Second(packet) => {
                    let sent = frame(&mut *buffer, IF_AP, seq, &[packet]);
                    self.ch.tx_done();
                    sent
                }
                Either3::Third(_event) => None,
            }
            .unwrap_or(0);
            if tx_len > 0 {
                self.next_seq = seq.wrapping_add(1);
            }

            match self.iface.transfer(&mut buffer, tx_len).await {
                Ok(received) => {
                    self.failures = 0;
                    if let Some(received) = buffer.get(..received) {
                        self.handle_rx(received)?;
                    }
                }
                Err(_error) => {
                    self.failures += 1;
                    if self.failures >= MAX_FAILURES {
                        return Err(Reset);
                    }
                }
            }
        }
    }

    /// Splits a read into its frames. A bad frame drops the rest, whose boundaries are lost.
    fn handle_rx(&mut self, mut stream: &[u8]) -> Result<(), Reset> {
        while let Some((header, rest)) = stream.split_first_chunk::<HEADER>() {
            let [if_type, _, len0, len1, offset0, offset1, sum0, sum1, ..] = *header;
            let len = usize::from(u16::from_le_bytes([len0, len1]));
            let Some((payload, next)) = rest.split_at_checked(len) else {
                return Ok(());
            };
            if len == 0 || u16::from_le_bytes([offset0, offset1]) != OFFSET {
                return Ok(());
            }
            if CHECKSUM {
                let mut zeroed = *header;
                zeroed[6] = 0;
                zeroed[7] = 0;
                if checksum(&zeroed).wrapping_add(checksum(payload))
                    != u16::from_le_bytes([sum0, sum1])
                {
                    return Ok(());
                }
            }
            stream = next;
            self.handle_frame(if_type, payload)?;
        }
        Ok(())
    }

    fn handle_frame(&mut self, if_type: u8, payload: &[u8]) -> Result<(), Reset> {
        match if_type & 0x0f {
            IF_AP => {
                if let Some(buf) = self.ch.try_rx_buf()
                    && let Some(buf) = buf.get_mut(..payload.len())
                {
                    buf.copy_from_slice(payload);
                    self.ch.rx_done(payload.len());
                }
            }
            IF_SERIAL => {
                let Some((header, rest)) = payload.split_first_chunk::<10>() else {
                    return Ok(());
                };
                let Some((len, data)) = rest.split_first_chunk::<2>() else {
                    return Ok(());
                };
                let Some(data) = data.get(..usize::from(u16::from_le_bytes(*len))) else {
                    return Ok(());
                };
                if header == RPC_RESPONSE {
                    self.shared.ioctl_done(data);
                } else if header == RPC_EVENT
                    && let Some(proto::Value::Varint(proto::EVENT_ESP_INIT)) = proto::find(data, 2)
                {
                    // A second init event means the coprocessor restarted.
                    if self.shared.ready() {
                        return Err(Reset);
                    }
                    self.shared.set_ready();
                }
            }
            _ => {}
        }
        Ok(())
    }
}

/// Writes a frame of `parts` to `buffer` and returns its length, or `None` if it doesn't fit.
fn frame(buffer: &mut [u8], if_type: u8, seq: u16, parts: &[&[u8]]) -> Option<usize> {
    let (header, payload) = buffer.split_at_mut_checked(HEADER)?;
    let mut len = 0;
    for part in parts {
        payload
            .get_mut(len..len + part.len())?
            .copy_from_slice(part);
        len += part.len();
    }
    let mut bytes = [0; HEADER];
    bytes[0] = if_type;
    bytes[2..4].copy_from_slice(&u16::try_from(len).ok()?.to_le_bytes());
    bytes[4..6].copy_from_slice(&OFFSET.to_le_bytes());
    bytes[8..10].copy_from_slice(&seq.to_le_bytes());
    if CHECKSUM {
        let sum = checksum(&bytes).wrapping_add(checksum(payload.get(..len)?));
        bytes[6..8].copy_from_slice(&sum.to_le_bytes());
    }
    header.copy_from_slice(&bytes);
    Some(HEADER + len)
}

fn checksum(bytes: &[u8]) -> u16 {
    bytes
        .iter()
        .fold(0, |sum, &byte| u16::wrapping_add(sum, byte.into()))
}
