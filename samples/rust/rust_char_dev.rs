// SPDX-License-Identifier: GPL-2.0

//! Rust character device sample.
//!
//! This sample demonstrates how to create a simple character device in Rust,
//! by reimplementing the `scull` device from the Linux Device Drivers book.

use core::{mem, ptr::NonNull};

use kernel::{
    c_str,
    char_dev::{CharDevice, CharDeviceID, DeviceRegistration, OpenCharDevice, Whence},
    fs::{file::flags, File, LocalFile},
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
    uaccess::{UserSlice, UserSliceReader, UserSliceWriter},
};

module! {
    type: RustCharDevModule,
    name: "rust_char_dev",
    author: "Rust for Linux Contributors",
    description: "Rust character device sample",
    license: "GPL",
}

const DEVICE_NAME: &CStr = c_str!("rust_scull");
const DEFAULT_QSET_SIZE: usize = 1000;
const DEFAULT_QUANTUM_SIZE: usize = 4000;
const NUM_DEVS: usize = 4;

// This is probably too specific a function to be in the Rust standard library...
trait OptionExt<T> {
    fn get_or_try_insert_with<F, E>(&mut self, f: F) -> Result<&mut T, E>
    where
        F: FnOnce() -> Result<T, E>;
}

impl<T> OptionExt<T> for Option<T> {
    fn get_or_try_insert_with<F, E>(&mut self, f: F) -> Result<&mut T, E>
    where
        F: FnOnce() -> Result<T, E>,
    {
        if self.is_none() {
            *self = Some(f()?);
        }

        Ok(self.as_mut().unwrap())
    }
}

#[derive(IoctlCommand)]
#[ioctl(code = b'k')]
enum Command {
    Reset,                       // 0
    SetQuantum(UserSliceReader), // 1
    SetQset(UserSliceReader),    // 2
    TellQuantum(u64),            // 3
    TellQset(u64),               // 4
    GetQuantum(UserSliceWriter), // 5
    GetQset(UserSliceWriter),    // 6
    QueryQuantum,                // 7
    QueryQset,                   // 8
    ExchangeQuantum(UserSlice),  // 9
    ExchangeQset(UserSlice),     // 10
    ShiftQuantum(u64),           // 11
    ShiftQset(u64),              // 12
}

// We implement `ScullQset` as close as possible to the `struct scull_qset` implementation from
// the book. This means that we have to use raw pointers and some unsafe code for the data.
// Otherwise, we'd have massive memory overhead by storing sizes/capacities unnecessarily.
// E.g. every `ScullQset` would be 8 * qset_size bytes larger if we used `Box<[_]>` and
// 16 * qset_size bytes larger if we used `Vec<_>`.
// However, the knowledge that all data arrays are of the same size isn't possible to express
// in safe Rust without this memory overhead.
struct ScullQset {
    data: Option<NonNull<Option<NonNull<u8>>>>,
    quantum_size: usize,
    qset_size: usize,
    next: Option<Box<ScullQset>>,
}

impl ScullQset {
    fn new(quantum_size: usize, qset_size: usize) -> Self {
        Self {
            data: None,
            quantum_size,
            qset_size,
            next: None,
        }
    }

    /// Returns a reference to the quantum at index `i` if it exists.
    fn get_quantum(&self, i: usize) -> Option<&[u8]> {
        let data_slice = NonNull::slice_from_raw_parts(self.data?, self.qset_size);
        // SAFETY: `data_slice` points to a valid slice of `Option<NonNull<u8>>`.
        let quantum = unsafe { data_slice.as_ref()[i] };
        let quantum_slice = NonNull::slice_from_raw_parts(quantum?, self.quantum_size);
        // SAFETY: `quantum_slice` points to a valid slice of `u8`.
        Some(unsafe { quantum_slice.as_ref() })
    }

    /// Returns a mutable reference to the quantum at index `i`, allocating it first if necessary.
    fn get_quantum_mut(&mut self, i: usize) -> Option<&mut [u8]> {
        let data = self
            .data
            .get_or_try_insert_with(|| {
                let mut data =
                    mem::ManuallyDrop::new(Vec::with_capacity(self.qset_size, GFP_KERNEL)?);
                for _ in 0..self.qset_size {
                    data.push(None, GFP_KERNEL)?;
                }

                assert!(data.len() == data.capacity());

                // SAFETY: `data.as_mut_ptr()` is non-null.
                Ok::<_, Error>(unsafe { NonNull::new_unchecked(data.as_mut_ptr()) })
            })
            .ok()?;

        let mut data_slice = NonNull::slice_from_raw_parts(*data, self.qset_size);

        // SAFETY: `data_slice` points to a valid slice of `Option<NonNull<u8>>`.
        let maybe_quantum = unsafe { &mut data_slice.as_mut()[i] };
        let quantum = maybe_quantum
            .get_or_try_insert_with(|| {
                let mut quantum =
                    mem::ManuallyDrop::new(Vec::with_capacity(self.quantum_size, GFP_KERNEL)?);
                for _ in 0..self.quantum_size {
                    quantum.push(0, GFP_KERNEL)?;
                }

                assert!(quantum.len() == quantum.capacity());

                // SAFETY: `quantum.as_mut_ptr()` is non-null.
                Ok::<_, Error>(unsafe { NonNull::new_unchecked(quantum.as_mut_ptr()) })
            })
            .ok()?;

        let mut quantum_slice = NonNull::slice_from_raw_parts(*quantum, self.quantum_size);
        // SAFETY: `quantum_slice` points to a valid slice of `u8`.
        Some(unsafe { quantum_slice.as_mut() })
    }
}

impl Drop for ScullQset {
    fn drop(&mut self) {
        if let Some(data) = self.data.take() {
            // SAFETY: `data` was created by `Vec::with_capacity` with a capacity of `qset_size`.
            let data_vec =
                unsafe { Vec::from_raw_parts(data.as_ptr(), self.qset_size, self.qset_size) };

            for quantum in data_vec {
                let Some(quantum) = quantum else { continue };

                // SAFETY: `quantum` was created by `Vec::with_capacity` with a capacity of
                // `quantum_size`.
                let _ = unsafe {
                    Vec::from_raw_parts(quantum.as_ptr(), self.quantum_size, self.quantum_size)
                };
            }
        }
    }
}

// SAFETY: The raw pointers are uniquely owned by `ScullQset` and not shared, so it's safe to send
// it to another thread.
unsafe impl Send for ScullQset {}

struct ScullDevInner {
    data: Option<Box<ScullQset>>,
    quantum_size: usize,
    qset_size: usize,
    size: usize,
}

impl Default for ScullDevInner {
    fn default() -> Self {
        Self {
            data: None,
            quantum_size: DEFAULT_QUANTUM_SIZE,
            qset_size: DEFAULT_QSET_SIZE,
            size: 0,
        }
    }
}

impl ScullDevInner {
    fn trim(&mut self) {
        mem::take(&mut self.data);
        self.size = 0;
    }

    fn follow(&mut self, n: usize) -> Option<&mut ScullQset> {
        let mut qs = self
            .data
            .get_or_try_insert_with(|| {
                Box::new(
                    ScullQset::new(self.quantum_size, self.qset_size),
                    GFP_KERNEL,
                )
            })
            .ok()?;

        for _ in 0..n {
            qs = qs
                .next
                .get_or_try_insert_with(|| {
                    // We use `qs.quantum_size` and `qs.qset_size` here to avoid subtly
                    // different behavior from the original C implementation.
                    // If we used the sizes from `self`, we could end up with differently
                    // sized qsets in the linked list (which would not be a safety problem).
                    // Like this, we only use an updated size after `trim` has been called,
                    // which is the same behavior as in the book.
                    Box::new(ScullQset::new(qs.quantum_size, qs.qset_size), GFP_KERNEL)
                })
                .ok()?;
        }

        Some(qs)
    }
}

#[derive(Clone)]
struct ScullDev {
    inner: Arc<Mutex<ScullDevInner>>,
}

#[vtable]
impl CharDevice for ScullDev {
    type OpenPtr = Box<Self>;
    type Err = Error;

    fn new(_dev_id: CharDeviceID) -> Result<Self> {
        Ok(Self {
            inner: Arc::pin_init(new_mutex!(ScullDevInner::default()), GFP_KERNEL)?,
        })
    }

    fn open(&self, file: &File) -> Result<Self::OpenPtr> {
        if file.flags() & flags::O_ACCMODE == flags::O_WRONLY {
            // TODO: this should be lock_interruptible, but that's not in the Rust API yet
            self.inner.lock().trim();
        }

        Ok(Box::new(self.clone(), GFP_KERNEL)?)
    }
}

#[vtable]
impl OpenCharDevice for ScullDev {
    type IoctlCmd = Command;
    type Err = Error;

    fn read(&self, _file: &LocalFile, mut buf: UserSliceWriter, offset: &mut i64) -> Result<usize> {
        let pos = usize::try_from(*offset).map_err(|_| EINVAL)?;

        // TODO: this should be lock_interruptible, but that's not in the Rust API yet
        let mut inner = self.inner.lock();

        // To keep the behavior of the original C implementation, namely that the quantum and qset
        // sizes are only updated after a trim, we use the sizes from the inner data if it exists.
        let (quantum_size, qset_size) = inner
            .data
            .as_ref()
            .map_or((inner.quantum_size, inner.qset_size), |qs| {
                (qs.quantum_size, qs.qset_size)
            });
        let item_size = quantum_size * qset_size;

        if pos >= inner.size {
            return Ok(0);
        }

        let mut count = buf.len().min(inner.size - pos);
        let item = pos / item_size;
        let rest = pos % item_size;
        let s_pos = rest / quantum_size;
        let q_pos = rest % quantum_size;

        let Some(q) = inner.follow(item).and_then(|qs| qs.get_quantum(s_pos)) else {
            return Ok(0);
        };

        count = count.min(quantum_size - q_pos);

        buf.write_slice(&q[q_pos..q_pos + count])?;

        *offset += count as i64;

        Ok(count)
    }

    fn write(
        &self,
        _file: &LocalFile,
        mut buf: UserSliceReader,
        offset: &mut i64,
    ) -> Result<usize> {
        let pos = usize::try_from(*offset).map_err(|_| EINVAL)?;

        // TODO: this should be lock_interruptible, but that's not in the Rust API yet
        let mut inner = self.inner.lock();

        // To keep the behavior of the original C implementation, namely that the quantum and qset
        // sizes are only updated after a trim, we use the sizes from the inner data if it exists.
        let (quantum_size, qset_size) = inner
            .data
            .as_ref()
            .map_or((inner.quantum_size, inner.qset_size), |qs| {
                (qs.quantum_size, qs.qset_size)
            });
        let item_size = quantum_size * qset_size;

        let item = pos / item_size;
        let rest = pos % item_size;
        let s_pos = rest / quantum_size;
        let q_pos = rest % quantum_size;

        let Some(q) = inner.follow(item).and_then(|qs| qs.get_quantum_mut(s_pos)) else {
            return Err(ENOMEM);
        };

        let count = buf.len().min(quantum_size - q_pos);

        buf.read_slice(&mut q[q_pos..q_pos + count])?;

        let new_pos = pos + count;
        *offset = new_pos as i64;

        if new_pos > inner.size {
            inner.size = new_pos;
        }

        Ok(count)
    }

    fn ioctl(
        &self,
        _file: &File,
        cmd: Self::IoctlCmd,
        #[cfg(CONFIG_COMPAT)] _compat: bool,
    ) -> Result<u64> {
        // The original implementation from the book actually doesn't consider the lock here at all,
        // but Rust forces us to do so :)
        let mut inner = self.inner.lock();

        // We should definitely check if the user is trying to set a size to 0, or we'll
        // end up with panics in the read/write functions due to division by zero.
        // However, the original implementation doesn't account for this, so we won't either.
        match cmd {
            Command::Reset => {
                inner.quantum_size = DEFAULT_QUANTUM_SIZE;
                inner.qset_size = DEFAULT_QSET_SIZE;
            }
            Command::SetQuantum(mut reader) => {
                // TODO: guard this command (and all others where a size can be set by the user)
                // with `capability(CAP_SYS_ADMIN)`, which is not yet possible in the Rust API.
                let quantum_size = reader.read()?;

                if !reader.is_empty() {
                    return Err(EINVAL);
                }

                inner.quantum_size = quantum_size;
            }
            Command::TellQuantum(quantum) => {
                inner.quantum_size = quantum as usize;
            }
            Command::GetQuantum(mut writer) => {
                writer.write(&inner.quantum_size)?;

                if !writer.is_empty() {
                    return Err(EINVAL);
                }
            }
            Command::QueryQuantum => {
                return Ok(inner.quantum_size as u64);
            }
            Command::ExchangeQuantum(slice) => {
                let (mut reader, mut writer) = slice.reader_writer();
                let quantum_size = reader.read()?;

                if !reader.is_empty() {
                    return Err(EINVAL);
                }

                writer.write(&inner.quantum_size)?;

                inner.quantum_size = quantum_size;
            }
            Command::ShiftQuantum(quantum) => {
                let old_quantum = inner.quantum_size;
                inner.quantum_size = quantum as usize;
                return Ok(old_quantum as u64);
            }
            Command::SetQset(mut reader) => {
                let qset_size = reader.read()?;

                if !reader.is_empty() {
                    return Err(EINVAL);
                }

                inner.qset_size = qset_size;
            }
            Command::TellQset(qset) => {
                inner.qset_size = qset as usize;
            }
            Command::GetQset(mut writer) => {
                writer.write(&inner.qset_size)?;

                if !writer.is_empty() {
                    return Err(EINVAL);
                }
            }
            Command::QueryQset => {
                return Ok(inner.qset_size as u64);
            }
            Command::ExchangeQset(slice) => {
                let (mut reader, mut writer) = slice.reader_writer();
                let qset_size = reader.read()?;

                if !reader.is_empty() {
                    return Err(EINVAL);
                }

                writer.write(&inner.qset_size)?;

                inner.qset_size = qset_size;
            }
            Command::ShiftQset(qset) => {
                let old_qset = inner.qset_size;
                inner.qset_size = qset as usize;
                return Ok(old_qset as u64);
            }
        }

        Ok(0)
    }

    fn llseek(
        &self,
        _file: &LocalFile,
        pos: &mut i64,
        offset: i64,
        whence: Whence,
    ) -> Result<u64, Self::Err> {
        let size = self.inner.lock().size as i64;

        let new_offset = match whence {
            Whence::Set => offset,
            Whence::Cur => *pos + offset,
            Whence::End => size + offset,
            _ => return Err(EINVAL),
        };

        if new_offset < 0 {
            return Err(EINVAL);
        }

        *pos = new_offset;

        Ok(new_offset as u64)
    }
}

struct RustCharDevModule {
    reg: Pin<Box<DeviceRegistration<ScullDev, NUM_DEVS>>>,
}

impl kernel::Module for RustCharDevModule {
    fn init(module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust character device sample (init)\n");

        let reg = Box::pin_init(
            DeviceRegistration::register(module, DEVICE_NAME),
            GFP_KERNEL,
        )?;

        let base_dev_id = reg.get_base_dev_id();
        pr_info!(
            "Registered device {DEVICE_NAME} with major {} and minors {} through {}\n",
            base_dev_id.major(),
            base_dev_id.minor(),
            base_dev_id.minor() + NUM_DEVS as u32 - 1
        );

        Ok(RustCharDevModule { reg })
    }
}

impl Drop for RustCharDevModule {
    fn drop(&mut self) {
        let dev_id = self.reg.get_base_dev_id();
        pr_info!(
            "Device {DEVICE_NAME} had major {} and minors {} through {}\n",
            dev_id.major(),
            dev_id.minor(),
            dev_id.minor() + NUM_DEVS as u32 - 1
        );

        pr_info!("Rust character device sample (exit)\n");
    }
}
