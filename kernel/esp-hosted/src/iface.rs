use ::sdio::sdio::SdioCard;
use ::sdio::{MmcBus, MmcError};
use aligned::{A4, Aligned};
use core::{ptr, slice};
use embassy_time::{Delay, Timer};

const SDIO_FUNC_1: u8 = 1;
const FUNC1_MASK: u8 = 1 << 1;

/// Host-to-co-processor interrupt bit that opens the data path.
const ESP_OPEN_DATA_PATH: u8 = 1 << 0;

// Co-processor registers, masked with `ESP_ADDRESS_MASK` (0x3FF).
const REG_INT_RAW: u32 = 0x050;
const REG_TOKEN_RDATA: u32 = 0x044;
const REG_PACKET_LEN: u32 = 0x060;
const REG_INT_CLR: u32 = 0x0D4;
const REG_HOST_TO_SLAVE_INTR: u32 = 0x08C; // scratch register 7

/// CMD53 data window end; data is transferred to/from `ESP_DATA_END_ADDR - len`.
const ESP_DATA_END_ADDR: u32 = 0x1F800;
const ESP_BLOCK_SIZE: usize = 512;
const _: () = assert!(crate::MAX_BUFFER_SIZE.is_multiple_of(ESP_BLOCK_SIZE));
type Block = Aligned<A4, [u8; ESP_BLOCK_SIZE]>;

const ESP_RX_BYTE_MAX: u32 = 0x10_0000;
const ESP_LEN_MASK: u32 = 0x0F_FFFF;
const ESP_TX_BUFFER_MAX: u32 = 0x1000;
const ESP_TX_BUFFER_MASK: u32 = 0x0FFF;
/// Co-processor RX buffer size; one packet uses `ceil(len / this)` buffers.
const ESP_RX_BUFFER_SIZE: usize = 1536;
/// Times, 1 ms apart, to look for TX buffers before giving up on a packet.
const CREDIT_RETRIES: u8 = 50;

/// The window address that makes the co-processor's FIFO advance by `len`.
fn data_address(len: usize) -> u32 {
    ESP_DATA_END_ADDR.saturating_sub(word(len))
}

fn word(value: usize) -> u32 {
    u32::try_from(value).unwrap_or(u32::MAX)
}

fn block_count(available: usize, len: usize) -> Option<usize> {
    let count = len.div_ceil(ESP_BLOCK_SIZE);
    (count * ESP_BLOCK_SIZE <= available).then_some(count)
}

fn blocks(buffer: &Aligned<A4, [u8]>, len: usize) -> Option<&[Block]> {
    let count = block_count(buffer.len(), len)?;
    // SAFETY: `buffer` is A4-aligned and holds `count` whole blocks.
    Some(unsafe { slice::from_raw_parts(ptr::from_ref(buffer).cast::<Block>(), count) })
}

fn blocks_mut(buffer: &mut Aligned<A4, [u8]>, len: usize) -> Option<&mut [Block]> {
    let count = block_count(buffer.len(), len)?;
    // SAFETY: as in `blocks`, and `buffer` is borrowed mutably.
    Some(unsafe { slice::from_raw_parts_mut(ptr::from_mut(buffer).cast::<Block>(), count) })
}

pub(crate) struct SdioInterface<B: MmcBus> {
    card: SdioCard<B, Delay>,
    freq: u32,
    tx_buf_count: u32,
    rx_byte_count: u32,
    /// Last buffer-grant count read from `TOKEN_RDATA`. Cached so TX doesn't read
    /// the register on every packet; refreshed only when credits run out.
    cached_granted: u32,
}

impl<B: MmcBus> SdioInterface<B> {
    pub fn new(bus: B, freq_hz: u32) -> Self {
        Self {
            card: SdioCard::new_uninit(bus, Delay),
            freq: freq_hz,
            tx_buf_count: 0,
            rx_byte_count: 0,
            cached_granted: 0,
        }
    }

    async fn try_init(&mut self) -> Result<(), MmcError> {
        self.card.reacquire(self.freq).await?;

        self.tx_buf_count = 0;
        self.rx_byte_count = 0;
        self.cached_granted = 0;

        self.card.enable_functions(FUNC1_MASK).await?;
        let block_size = u16::try_from(ESP_BLOCK_SIZE).map_err(|_overflow| MmcError::BlockSize)?;
        self.card.set_block_size(0, block_size).await?;
        self.card.set_block_size(1, block_size).await?;
        self.card.enable_interrupts(FUNC1_MASK).await?;
        self.card
            .cmd52_write(SDIO_FUNC_1, REG_HOST_TO_SLAVE_INTR, ESP_OPEN_DATA_PATH)
            .await?;

        Ok(())
    }

    async fn read_reg32(&mut self, addr: u32) -> Result<u32, MmcError> {
        let mut buf: Aligned<A4, [u8; 4]> = Aligned([0; 4]);
        self.card
            .cmd53_read_bytes(SDIO_FUNC_1, true, addr, &mut buf)
            .await?;
        Ok(u32::from_le_bytes(*buf))
    }

    async fn write_reg32(&mut self, addr: u32, val: u32) -> Result<(), MmcError> {
        let buf: Aligned<A4, [u8; 4]> = Aligned(val.to_le_bytes());
        self.card
            .cmd53_write_bytes(SDIO_FUNC_1, true, addr, &buf)
            .await?;
        Ok(())
    }

    async fn read_status(&mut self) -> Result<(u32, u32), MmcError> {
        /// Length of the combined status read (`INT_RAW`..=`PACKET_LEN`), in bytes.
        const REG_STATUS_LEN: usize = (REG_PACKET_LEN - REG_INT_RAW + 4) as usize;

        let mut buf = Aligned([0; REG_STATUS_LEN]);
        self.card
            .cmd53_read_bytes(SDIO_FUNC_1, true, REG_INT_RAW, &mut buf)
            .await?;
        let [i0, i1, i2, i3, .., p0, p1, p2, p3] = *buf;
        let interrupts = u32::from_le_bytes([i0, i1, i2, i3]);
        let packet_len = u32::from_le_bytes([p0, p1, p2, p3]);
        Ok((interrupts, packet_len))
    }

    /// Reads `len` of the `left` bytes still queued, rounded up to whole blocks: the address
    /// encodes `left`, as in esp-hosted's host driver.
    async fn read_data(
        &mut self,
        buffer: &mut Aligned<A4, [u8]>,
        len: usize,
        left: usize,
    ) -> Result<(), MmcError> {
        let blocks = blocks_mut(buffer, len).ok_or(MmcError::BlockSize)?;
        self.card
            .cmd53_read_blocks::<ESP_BLOCK_SIZE>(SDIO_FUNC_1, true, data_address(left), blocks)
            .await
    }

    /// Writes `len` bytes, rounded up to whole blocks; the co-processor discards the tail.
    async fn write_data(&mut self, buffer: &Aligned<A4, [u8]>, len: usize) -> Result<(), MmcError> {
        let blocks = blocks(buffer, len).ok_or(MmcError::BlockSize)?;
        self.card
            .cmd53_write_blocks::<ESP_BLOCK_SIZE>(SDIO_FUNC_1, true, data_address(len), blocks)
            .await
    }

    fn has_credit(&self, buf_needed: u32) -> bool {
        self.cached_granted.wrapping_sub(self.tx_buf_count) & ESP_TX_BUFFER_MASK >= buf_needed
    }

    async fn write_packet(
        &mut self,
        buffer: &Aligned<A4, [u8]>,
        tx_len: usize,
    ) -> Result<(), MmcError> {
        let buf_needed = word(tx_len.div_ceil(ESP_RX_BUFFER_SIZE));

        for _ in 0..CREDIT_RETRIES {
            if !self.has_credit(buf_needed) {
                let token = self.read_reg32(REG_TOKEN_RDATA).await?;
                self.cached_granted = (token >> 16) & ESP_TX_BUFFER_MASK;
            }
            if self.has_credit(buf_needed) {
                self.write_data(buffer, tx_len).await?;
                self.tx_buf_count = (self.tx_buf_count + buf_needed) % ESP_TX_BUFFER_MAX;
                return Ok(());
            }
            Timer::after_millis(1).await;
        }
        Err(MmcError::Busy)
    }

    async fn read_packet(&mut self, buffer: &mut Aligned<A4, [u8]>) -> Result<usize, MmcError> {
        let (interrupts, raw) = self.read_status().await?;

        if interrupts != 0 {
            self.write_reg32(REG_INT_CLR, interrupts).await?;
        }
        let raw = raw & ESP_LEN_MASK;
        // All ones is a failed bus read, not a length. The count, not the interrupt, says whether
        // a packet came: the clear above can hide one that arrived after the status read.
        if raw == ESP_LEN_MASK {
            return Ok(0);
        }
        let len = (raw.wrapping_sub(self.rx_byte_count) & ESP_LEN_MASK) as usize;

        // Unlike esp-hosted's host, which grows its buffer to the whole read, a read beyond the
        // buffer is drained and dropped, or the count would never catch up.
        let mut left = len;
        while left > 0 {
            let chunk = left.min(buffer.len());
            self.read_data(buffer, chunk, left).await?;
            self.rx_byte_count = (self.rx_byte_count + word(chunk)) % ESP_RX_BYTE_MAX;
            left -= chunk;
        }
        Ok(if len <= buffer.len() { len } else { 0 })
    }

    pub async fn init(&mut self) {
        while self.try_init().await.is_err() {
            Timer::after_millis(100).await;
        }
    }

    pub async fn wait_for_ready(&mut self) {
        self.card.wait_for_event().await.ok();
    }

    pub async fn transfer(
        &mut self,
        buffer: &mut Aligned<A4, [u8]>,
        tx_len: usize,
    ) -> Result<usize, MmcError> {
        if tx_len > 0 {
            self.write_packet(buffer, tx_len).await?;
        }
        self.read_packet(buffer).await
    }
}
