//! The POSIX file and directory calls, and C's `FILE`s, over the volume `storage` mounts at
//! "/sdcard". Only the master's: files in the root folder, created once and appended to.

use alloc::boxed::Box;
use core::{
    ffi::{c_char, c_int, c_uint, c_void},
    mem::MaybeUninit,
    ptr, slice,
};

use fatfs::{Read, Seek, SeekFrom, Write};
use macros::export;
use sys::{
    DT_DIR, DT_REG, EBADF, EEXIST, EIO, ENOENT, O_CREAT, O_EXCL, O_WRONLY, S_IFDIR, S_IFREG,
    dirent, off_t,
};

use crate::{
    libc::{EOF, Stream, set_errno},
    storage::{DirIter, Entry, Volume, errno, with_volume},
};

const ROOT: &str = "/sdcard";

/// The name of a file in the root folder, which holds all of them.
fn file_name(path: &str) -> Result<&str, c_int> {
    path.strip_prefix("/sdcard/")
        .filter(|name| !name.is_empty() && !name.contains('/'))
        .ok_or(ENOENT)
}

fn volume<T>(f: impl FnOnce(&mut Volume) -> Result<T, c_int>) -> Result<T, c_int> {
    with_volume(f).unwrap_or(Err(ENOENT))
}

/// Sets `errno` for a failure, and gives a call its C result.
fn status(result: Result<c_int, c_int>) -> c_int {
    result.unwrap_or_else(|error| {
        set_errno(error);
        -1
    })
}

/// File descriptor `n + 3` is the volume's `n`th file.
fn index(fd: c_int) -> Result<usize, c_int> {
    usize::try_from(fd - 3).map_err(|_standard| EBADF)
}

/// Whether the root folder holds `name`, as a folder or not, and its size. FAT ignores case.
fn find(volume: &Volume, name: &str) -> Result<Option<(bool, u64)>, c_int> {
    let lower = |character: char| character.to_ascii_lowercase();
    for entry in volume.root().iter() {
        let entry = entry.map_err(|_fat| EIO)?;
        let named = match entry.long_file_name_as_ucs2_units() {
            Some(units) => char::decode_utf16(units.iter().copied())
                .map(|unit| lower(unit.unwrap_or(char::REPLACEMENT_CHARACTER)))
                .eq(name.chars().map(lower)),
            None => entry
                .short_file_name_as_bytes()
                .eq_ignore_ascii_case(name.as_bytes()),
        };
        if named {
            return Ok(Some((entry.is_dir(), entry.len())));
        }
    }
    Ok(None)
}

#[export]
unsafe fn open(path: &str, flags: c_int, _args: ...) -> c_int {
    assert!(
        flags == O_WRONLY | O_CREAT | O_EXCL,
        "the kernel supports only creating a new file for writing"
    );
    status(file_name(path).and_then(|name| {
        volume(|volume| {
            if find(volume, name)?.is_some() {
                return Err(EEXIST);
            }
            let index = volume.open(|root| root.create_file(name))?;
            c_int::try_from(index + 3).map_err(|_many| EIO)
        })
    }))
}

#[export]
fn close(fildes: c_int) -> c_int {
    status(index(fildes).and_then(|index| {
        volume(|volume| {
            if volume.close(index) {
                Ok(0)
            } else {
                Err(EBADF)
            }
        })
    }))
}

#[export]
fn unlink(path: &str) -> c_int {
    status(file_name(path).and_then(|name| {
        volume(|volume| {
            volume
                .root()
                .remove(name)
                .map(|()| 0)
                .map_err(|error| errno(&error))
        })
    }))
}

#[export]
fn rename(from: &str, to: &str) -> c_int {
    status(file_name(from).and_then(|from| {
        let to = file_name(to)?;
        volume(|volume| {
            let root = volume.root();
            root.rename(from, &root, to)
                .map(|()| 0)
                .map_err(|error| errno(&error))
        })
    }))
}

#[export]
fn stat(path: &str, buf: &mut MaybeUninit<sys::stat>) -> c_int {
    let found = file_name(path).and_then(|name| volume(|volume| find(volume, name)?.ok_or(ENOENT)));
    status(found.map(|(folder, size)| {
        // SAFETY: all-zero is a valid `stat`: plain integers.
        let mut info: sys::stat = unsafe { core::mem::zeroed() };
        info.st_mode = (if folder { S_IFDIR } else { S_IFREG } | 0o777).cast_unsigned();
        info.st_size = off_t::try_from(size).unwrap_or(off_t::MAX);
        buf.write(info);
        0
    }))
}

/// The C `DIR`, which reads the folder as `readdir` asks.
pub struct DirStream {
    iter: DirIter,
    entry: dirent,
}

// SAFETY: C uses a `DIR` from one task, and `readdir` steps `iter` only holding the volume lock.
unsafe impl Sync for DirStream {}

sys::c_handle!(DirStream, DirPtr, owned);

#[export]
fn opendir(name: &str) -> Option<Box<DirStream>> {
    assert!(name == ROOT, "the kernel supports only the root folder");
    volume(|volume| Ok(volume.root().iter()))
        .map(|iter| {
            Box::new(DirStream {
                iter,
                // SAFETY: all-zero is a valid `dirent`: plain integers and bytes.
                entry: unsafe { core::mem::zeroed() },
            })
        })
        .map_err(set_errno)
        .ok()
}

#[export]
fn readdir(dirp: &mut DirStream) -> *mut dirent {
    let entry = match with_volume(|_volume| dirp.iter.next()).flatten() {
        Some(Ok(entry)) => entry,
        Some(Err(_fat)) => {
            set_errno(EIO);
            return ptr::null_mut();
        }
        None => return ptr::null_mut(),
    };
    dirp.entry.d_type =
        u8::try_from(if entry.is_dir() { DT_DIR } else { DT_REG }).unwrap_or_default();
    dirp.entry
        .d_name
        .iter_mut()
        .zip(entry.file_name().bytes().take(255).chain([0]))
        .for_each(|(to, from)| *to = from as c_char);
    &raw mut dirp.entry
}

#[export]
fn closedir(_dirp: Box<DirStream>) -> c_int {
    0
}

/// A file in the volume's table. Dropping it closes the file.
pub struct Handle(usize);

impl Handle {
    fn with<T>(&self, f: impl FnOnce(&mut Entry) -> T) -> Option<T> {
        with_volume(|volume| volume.entry(self.0).map(f)).flatten()
    }

    /// Returns how many bytes it read, which is fewer than asked at the end of the file or on a
    /// failure.
    fn read(&self, buffer: &mut [u8]) -> usize {
        self.with(|entry| {
            let mut done = 0;
            while let Some(rest) = buffer.get_mut(done..).filter(|rest| !rest.is_empty()) {
                match entry.file.read(rest) {
                    Ok(0) => break,
                    Ok(count) => done += count,
                    Err(_fat) => {
                        entry.failed = true;
                        break;
                    }
                }
            }
            done
        })
        .unwrap_or(0)
    }

    /// Returns how many bytes it wrote, which is fewer than given only on a failure.
    fn write(&self, bytes: &[u8]) -> usize {
        self.with(|entry| {
            let mut done = 0;
            while let Some(rest) = bytes.get(done..).filter(|rest| !rest.is_empty()) {
                match entry.file.write(rest) {
                    Ok(0) | Err(_) => {
                        entry.failed = true;
                        break;
                    }
                    Ok(count) => done += count,
                }
            }
            done
        })
        .unwrap_or(0)
    }

    /// Whether a read or write has failed.
    pub fn failed(&self) -> bool {
        self.with(|entry| entry.failed).unwrap_or(true)
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        with_volume(|volume| volume.close(self.0));
    }
}

/// Takes the file descriptor `open` returned.
#[export]
fn fdopen(fildes: c_int, mode: &str) -> Option<Box<Stream>> {
    assert!(mode == "wb", "the kernel supports only writing a new file");
    index(fildes)
        .map(|index| Box::new(Stream::File(Handle(index))))
        .map_err(set_errno)
        .ok()
}

#[export]
#[expect(clippy::panic, reason = "other modes are a firmware bug")]
fn fopen(filename: &str, mode: &str) -> Option<Box<Stream>> {
    let append = match mode {
        "ab" => true,
        "rb" => false,
        _ => panic!("the kernel supports only the modes ab and rb"),
    };
    let opened = file_name(filename).and_then(|name| {
        volume(|volume| {
            volume.open(|root| {
                if !append {
                    return root.open_file(name);
                }
                let mut file = root.create_file(name)?;
                file.seek(SeekFrom::End(0))?;
                Ok(file)
            })
        })
    });
    opened
        .map(|index| Box::new(Stream::File(Handle(index))))
        .map_err(set_errno)
        .ok()
}

#[expect(
    clippy::panic,
    reason = "a console stream used as a file is a firmware bug"
)]
fn file(stream: &Stream) -> &Handle {
    match stream {
        Stream::File(file) => file,
        _ => panic!("the kernel supports only files here"),
    }
}

#[export]
unsafe fn fread(ptr: *mut c_void, size: c_uint, nmemb: c_uint, stream: &Stream) -> c_uint {
    let Some(length) = size.checked_mul(nmemb).filter(|&length| length > 0) else {
        return 0;
    };
    // SAFETY: C gives room for `nmemb` items of `size` bytes at `ptr`.
    let buffer = unsafe { slice::from_raw_parts_mut(ptr.cast::<u8>(), length as usize) };
    c_uint::try_from(file(stream).read(buffer) / size as usize).unwrap_or_default()
}

#[export]
unsafe fn fwrite(ptr: *const c_void, size: c_uint, nmemb: c_uint, stream: &Stream) -> c_uint {
    let Some(length) = size.checked_mul(nmemb).filter(|&length| length > 0) else {
        return 0;
    };
    // SAFETY: C passes `nmemb` items of `size` bytes at `ptr`.
    let bytes = unsafe { slice::from_raw_parts(ptr.cast::<u8>(), length as usize) };
    c_uint::try_from(file(stream).write(bytes) / size as usize).unwrap_or_default()
}

/// Writes the file's directory entry, which `fflush` leaves alone.
#[export]
#[expect(clippy::panic, reason = "closing a console stream is a firmware bug")]
fn fclose(stream: Box<Stream>) -> c_int {
    let Stream::File(file) = *stream else {
        panic!("the kernel supports only closing files");
    };
    let flushed = file
        .with(|entry| {
            entry.failed |= entry.file.flush().is_err();
            !entry.failed
        })
        .unwrap_or(false);
    if flushed { 0 } else { EOF }
}

#[export]
fn ferror(stream: &Stream) -> c_int {
    c_int::from(file(stream).failed())
}
