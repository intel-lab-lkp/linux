// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Miscdevice support.
//!
//! C headers: [`include/linux/miscdevice.h`](srctree/include/linux/miscdevice.h).
//!
//! Reference: <https://www.kernel.org/doc/html/latest/driver-api/misc_devices.html>

use bindings::loff_t;

use crate::{
    bindings,
    device::Device,
    error::{to_result, Error},
    ffi::{c_int, c_long, c_uint, c_ulong},
    fs::{file_operations::FileOperations, File},
    mm::virt::VmaNew,
    prelude::*,
    seq_file::SeqFile,
    types::{ForeignOwnable, Opaque}, uaccess::UserSlice,
};
use core::{marker::PhantomData, mem::MaybeUninit, pin::Pin};

/// Options for creating a misc device.
#[derive(Copy, Clone)]
pub struct MiscDeviceOptions {
    /// The name of the miscdevice.
    pub name: &'static CStr,
}

impl MiscDeviceOptions {
    /// Create a raw `struct miscdev` ready for registration.
    pub const fn into_raw<T: FileOperations>(self) -> bindings::miscdevice {
        // SAFETY: All zeros is valid for this C type.
        let mut result: bindings::miscdevice = unsafe { MaybeUninit::zeroed().assume_init() };
        result.minor = bindings::MISC_DYNAMIC_MINOR as ffi::c_int;
        result.name = self.name.as_char_ptr();
        result.fops = MiscdeviceVTable::<T>::build();
        result
    }
}

/// A registration of a miscdevice.
///
/// # Invariants
///
/// - `inner` contains a `struct miscdevice` that is registered using
///   `misc_register()`.
/// - This registration remains valid for the entire lifetime of the
///   [`MiscDeviceRegistration`] instance.
/// - Deregistration occurs exactly once in [`Drop`] via `misc_deregister()`.
/// - `inner` wraps a valid, pinned `miscdevice` created using
///   [`MiscDeviceOptions::into_raw`].
#[repr(transparent)]
#[pin_data(PinnedDrop)]
pub struct MiscDeviceRegistration<T> {
    #[pin]
    inner: Opaque<bindings::miscdevice>,
    _t: PhantomData<T>,
}

// SAFETY: It is allowed to call `misc_deregister` on a different thread from where you called
// `misc_register`.
unsafe impl<T> Send for MiscDeviceRegistration<T> {}
// SAFETY: All `&self` methods on this type are written to ensure that it is safe to call them in
// parallel.
unsafe impl<T> Sync for MiscDeviceRegistration<T> {}

impl<T: FileOperations> MiscDeviceRegistration<T> {
    /// Register a misc device.
    pub fn register(opts: MiscDeviceOptions) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            inner <- Opaque::try_ffi_init(move |slot: *mut bindings::miscdevice| {
                // SAFETY: The initializer can write to the provided `slot`.
                unsafe { slot.write(opts.into_raw::<T>()) };

                // SAFETY: We just wrote the misc device options to the slot. The miscdevice will
                // get unregistered before `slot` is deallocated because the memory is pinned and
                // the destructor of this type deallocates the memory.
                // INVARIANT: If this returns `Ok(())`, then the `slot` will contain a registered
                // misc device.
                to_result(unsafe { bindings::misc_register(slot) })
            }),
            _t: PhantomData,
        })
    }

    /// Returns a raw pointer to the misc device.
    pub fn as_raw(&self) -> *mut bindings::miscdevice {
        self.inner.get()
    }

    /// Access the `this_device` field.
    pub fn device(&self) -> &Device {
        // SAFETY: This can only be called after a successful register(), which always
        // initialises `this_device` with a valid device. Furthermore, the signature of this
        // function tells the borrow-checker that the `&Device` reference must not outlive the
        // `&MiscDeviceRegistration<T>` used to obtain it, so the last use of the reference must be
        // before the underlying `struct miscdevice` is destroyed.
        unsafe { Device::from_raw((*self.as_raw()).this_device) }
    }
}

#[pinned_drop]
impl<T> PinnedDrop for MiscDeviceRegistration<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: We know that the device is registered by the type invariants.
        unsafe { bindings::misc_deregister(self.inner.get()) };
    }
}
/// A vtable for the file operations of a Rust miscdevice.
struct MiscdeviceVTable<T: FileOperations>(PhantomData<T>);

impl<T: FileOperations> MiscdeviceVTable<T> {
    unsafe extern "C" fn read(
        raw_file: *mut bindings::file,
        raw_buf: *mut ffi::c_char,
        size: usize,
        raw_offset: *mut loff_t,
    ) -> c_long {
        // SAFETY: The read call of a file can access file and its private_data.
        let raw_device = unsafe { (*raw_file).private_data };

        // SAFETY: The read call of a file can borrow the private_data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };

        let user_slice = UserSlice::new(UserPtr::from_ptr(raw_buf as *mut c_void), size);
        let user_slice_writer = user_slice.writer();

        // SAFETY: The read call of a file can access and modify the offset pointer value.
        let offset = unsafe { &mut *raw_offset };

        match T::read(device, user_slice_writer, offset) {
            Ok(ret) => ret as c_long,
            Err(err) => err.to_errno() as c_long,
        }
    }

    unsafe extern "C" fn write(
        raw_file: *mut bindings::file,
        raw_buf: *const ffi::c_char,
        size: usize,
        raw_offset: *mut loff_t,
    ) -> c_long {
        // SAFETY: The read call of a file can access file and its private_data.
        let raw_device = unsafe { (*raw_file).private_data };

        // SAFETY: The read call of a file can borrow the private_data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };

        let user_slice = UserSlice::new(UserPtr::from_ptr(raw_buf as *mut c_void), size);
        let user_slice_reader = user_slice.reader();

        // SAFETY: The read call of a file can access and modify the offset pointer value.
        let offset = unsafe { &mut *raw_offset };

        match T::write(device, user_slice_reader, offset) {
            Ok(ret) => ret as c_long,
            Err(err) => err.to_errno() as c_long,
        }
    }

    /// # Safety
    ///
    /// `file` and `inode` must be the file and inode for a file that is undergoing initialization.
    /// The file must be associated with a `MiscDeviceRegistration<T>`.
    unsafe extern "C" fn open(inode: *mut bindings::inode, raw_file: *mut bindings::file) -> c_int {
        // SAFETY: The pointers are valid and for a file being opened.
        let ret = unsafe { bindings::generic_file_open(inode, raw_file) };
        if ret != 0 {
            return ret;
        }

        // SAFETY: The open call of a file can access the private data.
        let raw_misc = unsafe { (*raw_file).private_data };

        // SAFETY: This is a miscdevice, so `misc_open()` set the private data to a pointer to the
        // associated `struct miscdevice` before calling into this method. Furthermore,
        // `misc_open()` ensures that the miscdevice can't be unregistered and freed during this
        // call to `fops_open`.
        let misc = unsafe { &*raw_misc.cast::<MiscDeviceRegistration<T>>() };

        // SAFETY:
        // * This underlying file is valid for (much longer than) the duration of `T::open`.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };

        let ptr = match T::open(file, misc) {
            Ok(ptr) => ptr,
            Err(err) => return err.to_errno(),
        };

        // This overwrites the private data with the value specified by the user, changing the type
        // of this file's private data. All future accesses to the private data is performed by
        // other fops_* methods in this file, which all correctly cast the private data to the new
        // type.
        //
        // SAFETY: The open call of a file can access the private data.
        unsafe { (*raw_file).private_data = ptr.into_foreign() };

        0
    }

    /// # Safety
    ///
    /// `file` and `inode` must be the file and inode for a file that is being released. The file
    /// must be associated with a `MiscDeviceRegistration<T>`.
    unsafe extern "C" fn release(
        _inode: *mut bindings::inode,
        raw_file: *mut bindings::file,
    ) -> c_int {
        // SAFETY: The release call of a file owns the private data.
        let raw_device = unsafe { (*raw_file).private_data };
        // SAFETY: The release call of a file owns the private data.
        let device = unsafe { <T::Ptr as ForeignOwnable>::from_foreign(raw_device) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        T::release(device, unsafe { File::from_raw_file(raw_file) });

        0
    }

    /// # Safety
    ///
    /// `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    /// `vma` must be a vma that is currently being mmap'ed with this file.
    unsafe extern "C" fn mmap(
        raw_file: *mut bindings::file,
        vma: *mut bindings::vm_area_struct,
    ) -> c_int {
        // SAFETY: The mmap call of a file can access the private data.
        let raw_device = unsafe { (*raw_file).private_data };
        // SAFETY: This is a Rust Miscdevice, so we call `into_foreign` in `open` and
        // `from_foreign` in `release`, and `fops_mmap` is guaranteed to be called between those
        // two operations.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };
        // SAFETY: The caller provides a vma that is undergoing initial VMA setup.
        let area = unsafe { VmaNew::from_raw(vma) };
        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };

        match T::mmap(device, file, area) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    unsafe extern "C" fn ioctl(raw_file: *mut bindings::file, cmd: c_uint, arg: c_ulong) -> c_long {
        // SAFETY: The ioctl call of a file can access the private data.
        let raw_device = unsafe { (*raw_file).private_data };
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };

        match T::ioctl(device, file, cmd, arg) {
            Ok(ret) => ret as c_long,
            Err(err) => err.to_errno() as c_long,
        }
    }

    /// # Safety
    ///
    /// `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    #[cfg(CONFIG_COMPAT)]
    unsafe extern "C" fn compat_ioctl(
        raw_file: *mut bindings::file,
        cmd: c_uint,
        arg: c_ulong,
    ) -> c_long {
        // SAFETY: The compat ioctl call of a file can access the private data.
        let raw_device = unsafe { (*raw_file).private_data };
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };

        match T::compat_ioctl(device, file, cmd, arg) {
            Ok(ret) => ret as c_long,
            Err(err) => err.to_errno() as c_long,
        }
    }

    /// # Safety
    ///
    /// - `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    /// - `seq_file` must be a valid `struct seq_file` that we can write to.
    unsafe extern "C" fn show_fdinfo(
        seq_file: *mut bindings::seq_file,
        raw_file: *mut bindings::file,
    ) {
        // SAFETY: The release call of a file owns the private data.
        let raw_device = unsafe { (*raw_file).private_data };
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(raw_device) };
        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };
        // SAFETY: The caller ensures that the pointer is valid and exclusive for the duration in
        // which this method is called.
        let m = unsafe { SeqFile::from_raw(seq_file) };

        T::show_fdinfo(device, m, file);
    }

    const VTABLE: bindings::file_operations = bindings::file_operations {
        read: if T::HAS_READ { Some(Self::read) } else { None },
        write: if T::HAS_WRITE {
            Some(Self::write)
        } else {
            None
        },
        open: Some(Self::open),
        release: Some(Self::release),
        mmap: if T::HAS_MMAP { Some(Self::mmap) } else { None },
        unlocked_ioctl: if T::HAS_IOCTL {
            Some(Self::ioctl)
        } else {
            None
        },
        #[cfg(CONFIG_COMPAT)]
        compat_ioctl: if T::HAS_COMPAT_IOCTL {
            Some(Self::compat_ioctl)
        } else if T::HAS_IOCTL {
            Some(bindings::compat_ptr_ioctl)
        } else {
            None
        },
        show_fdinfo: if T::HAS_SHOW_FDINFO {
            Some(Self::show_fdinfo)
        } else {
            None
        },
        // SAFETY: All zeros is a valid value for `bindings::file_operations`.
        ..unsafe { MaybeUninit::zeroed().assume_init() }
    };

    const fn build() -> &'static bindings::file_operations {
        &Self::VTABLE
    }
}
