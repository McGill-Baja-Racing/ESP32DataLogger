//! embassy-net driver for esp-hosted-mcu over SDIO, as an open access point.
//!
//! Differs from `embassy-net-esp-hosted`: it starts an access point instead of joining a network,
//! reads in SDIO streaming mode, and has no other transports, Bluetooth, OTA or esp-hosted-fg.
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
/// then little-endian length, offset (always `OFFSET`), checksum (unused, sent as 0) and sequence
/// number, and 2 bytes of padding. A fragmented frame (flags bit 0) isn't reassembled.
const OFFSET: u16 = 12;
const HEADER: usize = OFFSET as usize;

/// esp-hosted-mcu's interface types.
const IF_AP: u8 = 2;
const IF_SERIAL: u8 = 3;

/// A whole number of SDIO blocks, holding a frame to send or everything the coprocessor queued
/// since the last read. A longer read is dropped.
const MAX_BUFFER_SIZE: usize = 8 * 512;

/// The RPC framing of esp-hosted-mcu: a type-length-value header before a request or response.
const RPC_HEADER: usize = 10;
const RPC_RESPONSE: &[u8; RPC_HEADER] = b"\x01\x06\x00RPCRsp\x02";
const RPC_EVENT: &[u8; RPC_HEADER] = b"\x01\x06\x00RPCEvt\x02";

/// How often an idle link is polled, which also notices a coprocessor that stopped answering.
const POLL_SECS: u64 = 1;
/// Failed transfers in a row that count as a coprocessor reset.
const MAX_FAILURES: u8 = 3;

pub struct State {
    shared: Shared,
    ch: ch::State<MTU, 2, 2>,
}

impl State {
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

pub type NetDriver<'a> = ch::Device<'a, MTU>;

pub fn new<B: MmcBus>(
    state: &mut State,
    bus: B,
    freq_hz: u32,
) -> (NetDriver<'_>, Control<'_>, Runner<'_, B>) {
    let (ch_runner, device) = ch::new(&mut state.ch, HardwareAddress::Ethernet([0; 6]));
    let state_ch = ch_runner.state_runner();

    let runner = Runner {
        ch: ch_runner,
        shared: &state.shared,
        next_seq: 1,
        failures: 0,
        iface: SdioInterface::new(bus, freq_hz),
    };

    (device, Control::new(state_ch, &state.shared), runner)
}

/// The coprocessor stopped answering or restarted, and has to be reset and started again.
#[derive(Debug)]
pub struct Reset;

pub struct Runner<'a, B: MmcBus> {
    ch: ch::Runner<'a, MTU>,
    shared: &'a Shared,

    next_seq: u16,
    failures: u8,

    iface: SdioInterface<B>,
}

impl<B: MmcBus> Runner<'_, B> {
    /// Runs until the coprocessor needs a reset.
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
                        self.handle_rx(received).await?;
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
    async fn handle_rx(&mut self, mut stream: &[u8]) -> Result<(), Reset> {
        while let Some((header, rest)) = stream.split_first_chunk::<HEADER>() {
            let [if_type, _, len0, len1, offset0, offset1, ..] = *header;
            let len = usize::from(u16::from_le_bytes([len0, len1]));
            let Some((payload, next)) = rest.split_at_checked(len) else {
                return Ok(());
            };
            if len == 0 || u16::from_le_bytes([offset0, offset1]) != OFFSET {
                return Ok(());
            }
            stream = next;
            self.handle_frame(if_type, payload).await?;
        }
        Ok(())
    }

    async fn handle_frame(&mut self, if_type: u8, payload: &[u8]) -> Result<(), Reset> {
        match if_type & 0x0f {
            IF_AP => {
                // Waits for the stack to free a buffer rather than dropping the frame.
                if let Some(buf) = self.ch.rx_buf().await.get_mut(..payload.len()) {
                    buf.copy_from_slice(payload);
                    self.ch.rx_done(payload.len());
                }
            }
            IF_SERIAL => {
                let Some((header, rest)) = payload.split_first_chunk::<RPC_HEADER>() else {
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
                    && matches!(
                        proto::find(data, 2),
                        Some(proto::Value::Varint(proto::EVENT_ESP_INIT))
                    )
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
    let [len0, len1] = u16::try_from(len).ok()?.to_le_bytes();
    let [offset0, offset1] = OFFSET.to_le_bytes();
    let [seq0, seq1] = seq.to_le_bytes();
    header.copy_from_slice(&[
        if_type, 0, len0, len1, offset0, offset1, 0, 0, seq0, seq1, 0, 0,
    ]);
    Some(HEADER + len)
}
