use alloc::vec::Vec;

use embassy_net_driver_channel as ch;
use embassy_net_driver_channel::driver::{HardwareAddress, LinkState};

use crate::ioctl::Shared;
use crate::proto::{self, Value};

/// Errors reported by control.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Error {
    /// The coprocessor refused the request with the given error code.
    Failed(i32),
    /// The coprocessor's answer was malformed or missing.
    Internal,
}

const WIFI_INTERFACE_AP: u64 = 1;
const WIFI_MODE_AP: u64 = 2;
/// `wifi_init_config` as `(field, value)` with the `CONFIG_ESP_WIFI_*` values of the kernel's
/// `sdkconfig.h`, which C's `WIFI_INIT_CONFIG_DEFAULT` expands; zero fields are left out.
const WIFI_INIT_CONFIG: [(u32, u64); 18] = [
    (1, 10),                   // static_rx_buf_num
    (2, 32),                   // dynamic_rx_buf_num
    (3, 1),                    // tx_buf_type: dynamic
    (4, 10),                   // static_tx_buf_num
    (5, 32),                   // dynamic_tx_buf_num
    (8, 1),                    // ampdu_rx_enable
    (9, 1),                    // ampdu_tx_enable
    (11, 1),                   // nvs_enable
    (13, 6),                   // rx_ba_win
    (15, 752),                 // beacon_max_len
    (16, 32),                  // mgmt_sbuf_num
    (17, 1 | 1 << 5 | 1 << 8), // feature_caps: wpa3_sae, gmac, bss_max_idle
    (18, 1),                   // sta_disconnected_pm
    (19, 7),                   // espnow_max_encrypt_num
    (20, 0x1F2F_3F4F),         // magic
    (21, 1),                   // rx_mgmt_buf_type: dynamic
    (22, 32),                  // rx_mgmt_buf_num
    (23, 3),                   // tx_hetb_queue_num
];

/// Handle for starting the access point.
pub struct Control<'a> {
    state_ch: ch::StateRunner<'a>,
    shared: &'a Shared,
}

impl<'a> Control<'a> {
    pub(crate) fn new(state_ch: ch::StateRunner<'a>, shared: &'a Shared) -> Self {
        Self { state_ch, shared }
    }

    /// Waits for the coprocessor, then starts an open access point on `channel` and brings the
    /// link up.
    ///
    /// # Errors
    ///
    /// If the coprocessor refuses a request or answers it wrongly.
    pub async fn start_ap(
        &mut self,
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

        let mut message = Vec::new();
        proto::number(&mut message, 1, WIFI_INTERFACE_AP);
        let response = self.call(proto::GET_MAC_ADDRESS, &message, 2).await?;
        let Some(Value::Bytes(address)) = proto::find(&response, 1) else {
            return Err(Error::Internal);
        };
        let Some(&address) = address.first_chunk() else {
            return Err(Error::Internal);
        };
        self.state_ch
            .set_hardware_address(HardwareAddress::Ethernet(address));
        self.state_ch.set_link_state(LinkState::Up);
        Ok(())
    }

    /// Sends request `id` and returns the response's message, whose field `status` is the
    /// coprocessor's error code.
    async fn call(&mut self, id: u32, message: &[u8], status: u32) -> Result<Vec<u8>, Error> {
        struct CancelOnDrop<'a>(&'a Shared);

        impl Drop for CancelOnDrop<'_> {
            fn drop(&mut self) {
                self.0.ioctl_cancel();
            }
        }

        let cancel = CancelOnDrop(self.shared);
        let response = self.shared.ioctl(proto::request(id, message)).await;
        core::mem::forget(cancel);

        // A response's id is its request's plus 256.
        if !matches!(proto::find(&response, 2), Some(Value::Varint(n)) if n == u64::from(id) + 256)
        {
            return Err(Error::Internal);
        }
        let Some(Value::Bytes(message)) =
            proto::fields(&response).find_map(|(field, value)| (field > 3).then_some(value))
        else {
            return Err(Error::Internal);
        };
        match proto::find(message, status) {
            // A negative code is sign-extended to 64 bits.
            Some(Value::Varint(code)) => match i32::try_from(code.cast_signed()) {
                Ok(0) => Ok(message.to_vec()),
                Ok(code) => Err(Error::Failed(code)),
                Err(_code) => Err(Error::Internal),
            },
            _ => Ok(message.to_vec()),
        }
    }
}
