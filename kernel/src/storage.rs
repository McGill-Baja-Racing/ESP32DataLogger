//! `esp_vfs_fat_sdmmc_mount`: brings up the SD card, mounts its FAT volume with `fatfs`, and
//! keeps the open files for the POSIX calls in `files`.
//!
//! One mutex guards the volume and every open file. FAT work runs on the calling C task (the
//! logger, the web server, the console), never on the network task.
//!
//! Unlike ESP-IDF's `FatFs`, a volume is never unmounted: the card keeps the dirty flag `fatfs` sets
//! on the first write, and the free-space hint is not written back.

use alloc::boxed::Box;
use core::{
    cell::{Cell, UnsafeCell},
    ffi::{c_int, c_void},
    fmt::Debug,
    mem::MaybeUninit,
    sync::atomic::{AtomicU32, Ordering},
};

use critical_section::Mutex;
use esp_hal::{
    Blocking,
    dma::aligned::InternalMemory,
    peripherals::{GPIO39, GPIO40, GPIO41, GPIO42, GPIO43, GPIO44},
    sdmmc::{BlockingError, BusWidth, CommandFlags, Error, ResponseLen, Slot, SlotConfig},
};
use esp_radio_rtos_driver::{
    self as rtos,
    semaphore::{SemaphoreHandle, SemaphoreKind},
};
use fatfs::{
    Date, DateTime, FileSystem, FsOptions, IoBase, LossyOemCpConverter, Read, Seek, SeekFrom, Time,
    TimeProvider, Write,
};
use macros::export;
use sys::{
    EEXIST, EIO, ENFILE, ENOENT, ENOSPC, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
    ESP_ERR_TIMEOUT, ESP_FAIL, SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING,
    SDMMC_SLOT_FLAG_INTERNAL_PULLUP, esp_err_t, esp_vfs_fat_mount_config_t, sdmmc_card_t,
    sdmmc_host_t, sdmmc_slot_config_t,
};

use crate::{libc::time, sdmmc};

const SECTOR: u32 = 512;
const SECTOR_U16: u16 = 512;
/// The voltage window of a 3.3 V host, and the bit that asks for a high-capacity card.
const OCR_VOLTAGE: u32 = 0xFF_8000;
const OCR_HIGH_CAPACITY: u32 = 1 << 30;
const OCR_READY: u32 = 1 << 31;
const STATUS_READY_FOR_DATA: u32 = 1 << 8;
const STATUS_APP_CMD: u32 = 1 << 5;
const STATE_TRANSFER: u32 = 4;
/// The card finishes programming a write within this, as in ESP-IDF.
const READY_TIMEOUT_US: u64 = 5_000_000;
/// ESP-IDF's card initialisation: up to 300 tries 10 ms apart, forgiving 2 failed commands.
const OP_COND_TRIES: u32 = 300;
const OP_COND_ERRORS: u32 = 3;
const GO_IDLE_DELAY_US: u32 = 20_000;
/// A transfer of the other slot takes well under a millisecond, so waiting this long for the
/// shared engine means that transfer is stuck.
const ENGINE_TIMEOUT_US: u64 = 5_000_000;
/// Sector reads in one volume operation. A 65,536-entry directory, the most `FatFs` walks, takes
/// fewer, so more means a corrupt FAT chain loops.
const MAX_READS: u32 = 1 << 18;

static READS: AtomicU32 = AtomicU32::new(0);

type Fs = FileSystem<Disk, Clock, LossyOemCpConverter>;
pub type File = fatfs::File<'static, Disk, Clock, LossyOemCpConverter>;
pub type Dir = fatfs::Dir<'static, Disk, Clock, LossyOemCpConverter>;
pub type DirIter = fatfs::DirIter<'static, Disk, Clock, LossyOemCpConverter>;

/// Data that tasks use only while holding its mutex, which can span slow I/O unlike a critical
/// section.
struct Locked<T> {
    lock: SemaphoreHandle,
    data: UnsafeCell<T>,
}

// SAFETY: `with` is the only access to `data`, and it holds `lock`.
unsafe impl<T> Sync for Locked<T> {}

impl<T> Locked<T> {
    fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        self.lock.take(None);
        // SAFETY: `lock` gives this task the only access until it gives it back.
        let result = f(unsafe { &mut *self.data.get() });
        self.lock.give();
        result
    }
}

/// The `errno` for a FAT failure.
pub fn errno(error: &fatfs::Error<()>) -> c_int {
    match error {
        fatfs::Error::NotFound => ENOENT,
        fatfs::Error::AlreadyExists => EEXIST,
        fatfs::Error::NotEnoughSpace => ENOSPC,
        _ => EIO,
    }
}

pub struct Entry {
    pub file: File,
    /// Set by a failed read or write, for `ferror`.
    pub failed: bool,
}

pub struct Volume {
    fs: &'static Fs,
    /// File descriptor `n + 3` is `files[n]`.
    files: Box<[Option<Entry>]>,
}

impl Volume {
    pub fn root(&self) -> Dir {
        self.fs.root_dir()
    }

    /// Opens a file with `open` and returns where the table put it.
    pub fn open(
        &mut self,
        open: impl FnOnce(&Dir) -> Result<File, fatfs::Error<()>>,
    ) -> Result<usize, c_int> {
        let (index, slot) = self
            .files
            .iter_mut()
            .enumerate()
            .find(|(_, slot)| slot.is_none())
            .ok_or(ENFILE)?;
        let file = open(&self.fs.root_dir()).map_err(|error| errno(&error))?;
        *slot = Some(Entry {
            file,
            failed: false,
        });
        Ok(index)
    }

    pub fn entry(&mut self, index: usize) -> Option<&mut Entry> {
        self.files.get_mut(index)?.as_mut()
    }

    /// Closes the file.
    pub fn close(&mut self, index: usize) -> bool {
        self.files.get_mut(index).and_then(Option::take).is_some()
    }
}

static VOLUME: Mutex<Cell<Option<&'static Locked<Volume>>>> = Mutex::new(Cell::new(None));

/// Runs `f` on the volume, or returns `None` before the card is mounted.
pub fn with_volume<R>(f: impl FnOnce(&mut Volume) -> R) -> Option<R> {
    let volume = critical_section::with(|cs| VOLUME.borrow(cs).get())?;
    Some(volume.with(|volume| {
        READS.store(0, Ordering::Relaxed);
        f(volume)
    }))
}

/// The time of the clock `settimeofday` sets; FAT counts from 1980.
#[derive(Debug)]
pub struct Clock;

impl TimeProvider for Clock {
    fn get_current_date(&self) -> Date {
        self.get_current_date_time().date
    }

    fn get_current_date_time(&self) -> DateTime {
        let field = |value: c_int| u16::try_from(value).unwrap_or_default();
        time::civil(time::now())
            .filter(|now| (80..=207).contains(&now.tm_year))
            .map_or_else(
                || DateTime::new(Date::new(1980, 1, 1), Time::new(0, 0, 0, 0)),
                |now| {
                    DateTime::new(
                        Date::new(
                            field(now.tm_year + 1900),
                            field(now.tm_mon + 1),
                            field(now.tm_mday),
                        ),
                        Time::new(field(now.tm_hour), field(now.tm_min), field(now.tm_sec), 0),
                    )
                },
            )
    }
}

/// The card as a byte stream, through a one-sector cache that is written back when another
/// sector is needed or on `flush`. Cached and addressed sectors count from the start of the card,
/// and the stream starts at the volume, `base` sectors in.
pub struct Disk {
    slot: Slot<'static, 0, Blocking>,
    rca: u16,
    /// Standard-capacity cards take byte addresses.
    block_addressing: bool,
    base: u32,
    buffer: Box<InternalMemory<[u8; SECTOR as usize]>>,
    sector: Option<u32>,
    dirty: bool,
    position: u64,
}

impl IoBase for Disk {
    type Error = ();
}

/// Waits while the other slot has a transfer on the shared engine.
fn retry<T>(mut operation: impl FnMut() -> Result<T, BlockingError>) -> Result<T, BlockingError> {
    let deadline = rtos::now() + ENGINE_TIMEOUT_US;
    loop {
        match operation() {
            Err(BlockingError::Busy) if rtos::now() < deadline => rtos::usleep(1000),
            result => return result,
        }
    }
}

fn code(error: BlockingError) -> esp_err_t {
    match error {
        BlockingError::Busy
        | BlockingError::Op(
            Error::Timeout | Error::ResponseTimeout | Error::DataTimeout | Error::NoCard,
        ) => ESP_ERR_TIMEOUT,
        _ => ESP_FAIL,
    }
}

impl Disk {
    /// Takes slot 0 with the pins the master wires.
    fn new(config: &sdmmc_slot_config_t) -> Result<Self, esp_err_t> {
        // SAFETY: both union members are the GPIO number, and `SDMMC_SLOT_NO_CD` and `_NO_WP` are
        // -1.
        let (detect, protect) =
            unsafe { (config.__bindgen_anon_1.cd.0, config.__bindgen_anon_2.wp.0) };
        assert!(
            (
                config.clk.0,
                config.cmd.0,
                config.d0.0,
                config.d1.0,
                config.d2.0,
                config.d3.0
            ) == (43, 44, 39, 40, 41, 42)
                && (detect, protect) == (-1, -1)
                && config.width == 4
                && config.flags == SDMMC_SLOT_FLAG_INTERNAL_PULLUP.cast_unsigned(),
            "the kernel supports only the master's four-line SD slot on its dedicated pins"
        );
        // SAFETY: the slot's pins belong to the SD host, and a second `slot::<0>` fails.
        let slot = unsafe {
            sdmmc::controller()?
                .slot::<0>(SlotConfig::default())
                .map_err(|_slot| ESP_ERR_INVALID_STATE)?
                .with_clk(GPIO43::steal())
                .with_cmd(GPIO44::steal())
                .with_data0(GPIO39::steal())
                .with_data1(GPIO40::steal())
                .with_data2(GPIO41::steal())
                .with_data3(GPIO42::steal())
        };
        Ok(Self {
            slot,
            rca: 0,
            block_addressing: false,
            base: 0,
            buffer: Box::new(InternalMemory::new([0; SECTOR as usize])),
            sector: None,
            dirty: false,
            position: 0,
        })
    }

    /// Sends a command with no data and returns the response words.
    fn command(
        &mut self,
        index: u8,
        arg: u32,
        response: ResponseLen,
    ) -> Result<[u32; 4], BlockingError> {
        // CMD0 has no response to wait for and CMD7 answers with busy. ACMD41's answer has no CRC.
        let flags = CommandFlags {
            wait_complete: index != 0,
            stop_abort: false,
            busy: index == 7,
        };
        let crc = !matches!(response, ResponseLen::None) && index != 41;
        retry(|| self.slot.command_blocking(index, arg, response, crc, flags))
    }

    fn app_command(
        &mut self,
        index: u8,
        arg: u32,
        response: ResponseLen,
    ) -> Result<[u32; 4], BlockingError> {
        let status = self.command(55, u32::from(self.rca) << 16, ResponseLen::Short)?[0];
        if status & STATUS_APP_CMD == 0 {
            return Err(Error::Unsupported.into());
        }
        self.command(index, arg, response)
    }

    /// A write is acknowledged before the card finishes programming it.
    fn wait_ready(&mut self) -> Result<(), BlockingError> {
        let deadline = rtos::now() + READY_TIMEOUT_US;
        loop {
            let status = self.command(13, u32::from(self.rca) << 16, ResponseLen::Short)?[0];
            if status & STATUS_READY_FOR_DATA != 0 && (status >> 9) & 0xF == STATE_TRANSFER {
                return Ok(());
            }
            if rtos::now() > deadline {
                return Err(Error::Timeout.into());
            }
            rtos::usleep(100);
        }
    }

    /// Moves the card through its start-up states, as `sdmmc_card_init` does for an SD memory
    /// card, and returns its OCR. Only version 2 cards, which answer CMD8, and the 4-bit bus.
    /// ESP-IDF also keeps the card clock running through ACMD41, which esp-hal does not offer.
    fn start(&mut self) -> Result<u32, BlockingError> {
        self.slot
            .set_bus_low_level(BusWidth::Bit1, SDMMC_FREQ_PROBING.unsigned_abs() * 1000)?;
        retry(|| self.slot.send_init_sequence())?;
        self.command(0, 0, ResponseLen::None)?;
        rtos::usleep(GO_IDLE_DELAY_US);
        if self.command(8, 0x1AA, ResponseLen::Short)?[0] & 0xFFF != 0x1AA {
            return Err(Error::ResponseError.into());
        }
        let mut errors = OP_COND_ERRORS;
        let mut tries = OP_COND_TRIES;
        let ocr = loop {
            if tries == 0 {
                return Err(Error::Timeout.into());
            }
            tries -= 1;
            match self.app_command(41, OCR_VOLTAGE | OCR_HIGH_CAPACITY, ResponseLen::Short) {
                Ok(response) if response[0] & OCR_READY != 0 => break response[0],
                Ok(_) => rtos::usleep(10_000),
                Err(error) => {
                    errors -= 1;
                    if errors == 0 {
                        return Err(error);
                    }
                }
            }
        };
        self.command(2, 0, ResponseLen::Long)?;
        let address = self.command(3, 0, ResponseLen::Short)?[0] >> 16;
        self.rca = u16::try_from(address).map_err(|_rca| Error::ResponseError)?;
        self.command(7, address << 16, ResponseLen::Short)?;
        self.block_addressing = ocr & OCR_HIGH_CAPACITY != 0;
        if !self.block_addressing {
            self.command(16, SECTOR, ResponseLen::Short)?;
        }
        self.wait_ready()?;
        self.app_command(6, 2, ResponseLen::Short)?;
        self.slot
            .set_bus_low_level(BusWidth::Bit4, SDMMC_FREQ_DEFAULT.unsigned_abs() * 1000)?;
        Ok(ocr)
    }

    /// Points the stream at the first partition when the card has a partition table. A card
    /// whose first sector is a FAT boot sector is one volume, as `FatFs` also accepts.
    fn find_partition(&mut self) -> Result<(), ()> {
        self.load(0, false)?;
        let sector = self.buffer.get_mut();
        let at = |from: usize, len: usize| sector.get(from..from + len);
        let is_boot = [54, 82]
            .iter()
            .any(|&from| at(from, 3) == Some(b"FAT".as_slice()));
        if at(510, 2) == Some([0x55, 0xAA].as_slice())
            && !is_boot
            && sector.get(450) != Some(&0)
            && let Some(start) = at(454, 4).and_then(<[u8]>::first_chunk::<4>)
        {
            self.base = u32::from_le_bytes(*start);
        }
        Ok(())
    }

    /// Moves the cached sector between the card and the buffer.
    fn transfer(&mut self, sector: u32, write: bool) -> Result<(), BlockingError> {
        let address = if self.block_addressing {
            sector
        } else {
            sector.checked_mul(SECTOR).ok_or(Error::Unsupported)?
        };
        let mut data = self.buffer.get_mut().unsize();
        let result = if write {
            retry(|| {
                self.slot
                    .write_blocks_blocking(24, address, data.reborrow(), SECTOR_U16, 1)
            })
        } else {
            retry(|| {
                self.slot
                    .read_blocks_blocking(17, address, data.reborrow(), SECTOR_U16, 1)
            })
        };
        match result {
            // esp-hal spins a fixed count on the busy line, which a slow write outlasts.
            Err(BlockingError::Op(Error::Timeout)) | Ok(_) if write => self.wait_ready(),
            Err(error) => Err(error),
            Ok(_) => Ok(()),
        }
    }

    fn flush_sector(&mut self) -> Result<(), ()> {
        if let (true, Some(sector)) = (self.dirty, self.sector) {
            self.transfer(sector, true).map_err(drop)?;
            self.dirty = false;
        }
        Ok(())
    }

    /// Makes the buffer hold `sector`, without reading it when all of it will be overwritten.
    fn load(&mut self, sector: u32, overwrite: bool) -> Result<(), ()> {
        if self.sector != Some(sector) {
            self.flush_sector()?;
            self.sector = None;
            if !overwrite {
                self.transfer(sector, false).map_err(drop)?;
            }
            self.sector = Some(sector);
        }
        Ok(())
    }

    /// The card sector the position is in, and the offset in it.
    fn locate(&self) -> Result<(u32, usize), ()> {
        let sector = u32::try_from(self.position / u64::from(SECTOR))
            .ok()
            .and_then(|sector| sector.checked_add(self.base))
            .ok_or(())?;
        let offset = usize::try_from(self.position % u64::from(SECTOR)).map_err(drop)?;
        Ok((sector, offset))
    }
}

impl Read for Disk {
    /// Reads up to the end of the sector.
    fn read(&mut self, out: &mut [u8]) -> Result<usize, ()> {
        if READS.fetch_add(1, Ordering::Relaxed) >= MAX_READS {
            return Err(());
        }
        let (sector, offset) = self.locate()?;
        self.load(sector, false)?;
        let count = out.len().min(SECTOR as usize - offset);
        let data = self.buffer.get_mut();
        out.iter_mut()
            .zip(data.iter().skip(offset))
            .take(count)
            .for_each(|(to, from)| *to = *from);
        self.position += count as u64;
        Ok(count)
    }
}

impl Write for Disk {
    /// Writes up to the end of the sector.
    fn write(&mut self, data: &[u8]) -> Result<usize, ()> {
        let (sector, offset) = self.locate()?;
        let count = data.len().min(SECTOR as usize - offset);
        self.load(sector, count == SECTOR as usize)?;
        self.buffer
            .get_mut()
            .iter_mut()
            .skip(offset)
            .zip(data)
            .for_each(|(to, from)| *to = *from);
        self.dirty = true;
        self.position += count as u64;
        Ok(count)
    }

    fn flush(&mut self) -> Result<(), ()> {
        self.flush_sector()
    }
}

impl Seek for Disk {
    fn seek(&mut self, position: SeekFrom) -> Result<u64, ()> {
        self.position = match position {
            SeekFrom::Start(offset) => offset,
            SeekFrom::Current(delta) => self.position.checked_add_signed(delta).ok_or(())?,
            SeekFrom::End(_) => unreachable!("fatfs seeks from the end only to format a disk"),
        };
        Ok(self.position)
    }
}

/// Brings up the card and mounts its volume. Only what the master passes: "/sdcard", a card
/// that is already formatted and at most `max_files` open files.
fn mount(
    host_config: &sdmmc_host_t,
    slot_config: &sdmmc_slot_config_t,
    max_files: usize,
) -> Result<(&'static mut sdmmc_card_t, &'static Locked<Volume>), esp_err_t> {
    let mut disk = Disk::new(slot_config)?;
    let ocr = disk.start().map_err(code)?;
    disk.find_partition().map_err(|()| ESP_FAIL)?;
    let rca = disk.rca;
    let fs =
        FileSystem::new(disk, FsOptions::new().time_provider(Clock)).map_err(|_fat| ESP_FAIL)?;
    let volume = Box::leak(Box::new(Locked {
        lock: SemaphoreHandle::new(SemaphoreKind::Mutex),
        data: UnsafeCell::new(Volume {
            fs: Box::leak(Box::new(fs)),
            files: (0..max_files).map(|_| None).collect(),
        }),
    }));
    // SAFETY: all-zero is a valid `sdmmc_card_t`: plain integers and function pointers. C reads
    // neither `cid` nor `csd`.
    let mut card: sdmmc_card_t = unsafe { core::mem::zeroed() };
    card.host = *host_config;
    card.ocr = ocr;
    card.rca = rca;
    card.set_is_mem(1);
    Ok((Box::leak(Box::new(card)), volume))
}

#[export]
fn esp_vfs_fat_sdmmc_mount(
    base_path: &str,
    host_config: &sdmmc_host_t,
    slot_config: *const c_void,
    mount_config: &esp_vfs_fat_mount_config_t,
    out_card: Option<&mut MaybeUninit<&'static mut sdmmc_card_t>>,
) -> Result<(), esp_err_t> {
    assert!(
        base_path == "/sdcard"
            && host_config.slot == 0
            && host_config.max_freq_khz == SDMMC_FREQ_DEFAULT
            && !mount_config.format_if_mount_failed,
        "the kernel supports only a mount at /sdcard on slot 0 at 20 MHz that never formats"
    );
    if critical_section::with(|cs| VOLUME.borrow(cs).get().is_some()) {
        return Err(ESP_ERR_INVALID_STATE);
    }
    let max_files =
        usize::try_from(mount_config.max_files).map_err(|_negative| ESP_ERR_INVALID_ARG)?;
    // SAFETY: C passes the slot configuration of the host it names, which is this driver's.
    let slot_config = unsafe { &*slot_config.cast::<sdmmc_slot_config_t>() };
    let (card, volume) = mount(host_config, slot_config, max_files)?;
    critical_section::with(|cs| VOLUME.borrow(cs).set(Some(volume)));
    if let Some(out_card) = out_card {
        out_card.write(card);
    }
    Ok(())
}
