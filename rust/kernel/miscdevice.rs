// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Miscdevice support.
//!
//! C headers: [`include/linux/miscdevice.h`](srctree/include/linux/miscdevice.h).
//!
//! Reference: <https://www.kernel.org/doc/html/latest/driver-api/misc_devices.html>

use crate::{
    bindings, container_of,
    device::Device,
    error::{to_result, Error, Result, VTABLE_DEFAULT_ERROR},
    ffi::{c_int, c_long, c_uint, c_ulong},
    fs::File,
    mm::virt::VmaNew,
    prelude::*,
    seq_file::SeqFile,
    str::CStr,
    types::{ForeignOwnable, Opaque},
};
use core::{marker::PhantomData, mem::MaybeUninit, pin::Pin};
use pin_init::Wrapper;

/// Options for creating a misc device.
#[derive(Copy, Clone)]
pub struct MiscDeviceOptions {
    /// The name of the miscdevice.
    pub name: &'static CStr,
}

impl MiscDeviceOptions {
    /// Create a raw `struct miscdev` ready for registration.
    pub const fn into_raw<T: MiscDevice>(self) -> bindings::miscdevice
    where
        T::Data: Sync,
    {
        // SAFETY: All zeros is valid for this C type.
        let mut result: bindings::miscdevice = unsafe { MaybeUninit::zeroed().assume_init() };
        result.minor = bindings::MISC_DYNAMIC_MINOR as _;
        result.name = self.name.as_char_ptr();
        result.fops = MiscdeviceVTable::<T>::build();
        result
    }
}

/// A registration of a miscdevice.
///
/// # Invariants
///
/// - `inner` is a registered misc device.
/// - `data` contains a valid `T::Data` for the whole lifetime of [`MiscDeviceRegistration`]
/// - `data` must be valid until `misc_deregister` (called when dropped) has returned.
/// - no mutable references to `data` may be created.
#[pin_data(PinnedDrop)]
pub struct MiscDeviceRegistration<T: MiscDevice> {
    #[pin]
    inner: Opaque<bindings::miscdevice>,
    #[pin]
    data: Opaque<T::Data>,
}

// SAFETY:
// - It is allowed to call `misc_deregister` on a different thread from where you called
//   `misc_register`.
// - Only implements `Send` if `MiscDevice::Data` is also `Send`.
unsafe impl<T: MiscDevice> Send for MiscDeviceRegistration<T> where T::Data: Send {}

// SAFETY:
// - All `&self` methods on this type are written to ensure that it is safe to call them in
//   parallel.
// - Only implements `Sync` if `MiscDevice::Data` is also `Sync`.
unsafe impl<T: MiscDevice> Sync for MiscDeviceRegistration<T> where T::Data: Sync {}

impl<T: MiscDevice> MiscDeviceRegistration<T> {
    /// Register a misc device.
    pub fn register(
        opts: MiscDeviceOptions,
        data: impl PinInit<T::Data, Error>,
    ) -> impl PinInit<Self, Error>
    where
        T::Data: Sync,
    {
        try_pin_init!(Self {
            data <- Opaque::pin_init(data),
            inner <- Opaque::try_ffi_init(move |slot: *mut bindings::miscdevice| {
                // SAFETY: The initializer can write to the provided `slot`.
                unsafe { slot.write(opts.into_raw::<T>()) };

                // SAFETY:
                // * We just wrote the misc device options to the slot. The miscdevice will
                //   get unregistered before `slot` is deallocated because the memory is pinned and
                //   the destructor of this type deallocates the memory.
                // * `data` is Initialized before `misc_register` so no race with `fops->open()`
                //   is possible.
                // INVARIANT: If this returns `Ok(())`, then the `slot` will contain a registered
                // misc device.
                to_result(unsafe { bindings::misc_register(slot) })
            }),
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
        unsafe { Device::as_ref((*self.as_raw()).this_device) }
    }

    /// Access the additional data stored in this registration.
    pub fn data(&self) -> &T::Data {
        // SAFETY:
        // * No mutable reference to the value contained by `self.data` can ever be created.
        // * The value contained by `self.data` is valid for the entire lifetime of `&self`.
        unsafe { &*self.data.get() }
    }
}

#[pinned_drop]
impl<T: MiscDevice> PinnedDrop for MiscDeviceRegistration<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: We know that the device is registered by the type invariants.
        unsafe { bindings::misc_deregister(self.inner.get()) };

        // SAFETY: `self.data` contains a valid `Data` and does not need to be valid anymore.
        unsafe { core::ptr::drop_in_place(self.data.get()) };
    }
}

/// Trait implemented by the private data of an open misc device.
#[vtable]
pub trait MiscDevice: Sized {
    /// What kind of pointer should `Self` be wrapped in.
    type Ptr: ForeignOwnable + Send + Sync;

    /// Additional data carried by the [`MiscDeviceRegistration`] for this [`MiscDevice`].
    /// If no additional data is required than the unit type `()` should be used.
    ///
    /// This can be accessed in [`MiscDevice::open()`] using
    /// [`MiscDeviceRegistration::data()`].
    type Data;

    /// Called when the misc device is opened.
    ///
    /// The returned pointer will be stored as the private data for the file.
    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Self::Ptr>;

    /// Called when the misc device is released.
    fn release(device: Self::Ptr, _file: &File) {
        drop(device);
    }

    /// Handle for mmap.
    ///
    /// This function is invoked when a user space process invokes the `mmap` system call on
    /// `file`. The function is a callback that is part of the VMA initializer. The kernel will do
    /// initial setup of the VMA before calling this function. The function can then interact with
    /// the VMA initialization by calling methods of `vma`. If the function does not return an
    /// error, the kernel will complete initialization of the VMA according to the properties of
    /// `vma`.
    fn mmap(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _file: &File,
        _vma: &VmaNew,
    ) -> Result {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Handler for ioctls.
    ///
    /// The `cmd` argument is usually manipulated using the utilities in [`kernel::ioctl`].
    ///
    /// [`kernel::ioctl`]: mod@crate::ioctl
    fn ioctl(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _file: &File,
        _cmd: u32,
        _arg: usize,
    ) -> Result<isize> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Handler for ioctls.
    ///
    /// Used for 32-bit userspace on 64-bit platforms.
    ///
    /// This method is optional and only needs to be provided if the ioctl relies on structures
    /// that have different layout on 32-bit and 64-bit userspace. If no implementation is
    /// provided, then `compat_ptr_ioctl` will be used instead.
    #[cfg(CONFIG_COMPAT)]
    fn compat_ioctl(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _file: &File,
        _cmd: u32,
        _arg: usize,
    ) -> Result<isize> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Show info for this fd.
    fn show_fdinfo(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _m: &SeqFile,
        _file: &File,
    ) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// A vtable for the file operations of a Rust miscdevice.
struct MiscdeviceVTable<T: MiscDevice>(PhantomData<T>);

impl<T: MiscDevice> MiscdeviceVTable<T>
where
    T::Data: Sync,
{
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
        let misc_ptr = unsafe { (*raw_file).private_data };

        // This is a miscdevice, so `misc_open()` sets the private data to a pointer to the
        // associated `struct miscdevice` before calling into this method.
        let misc_ptr = misc_ptr.cast::<Opaque<bindings::miscdevice>>();

        // SAFETY:
        // * `misc_open()` ensures that the `struct miscdevice` can't be unregistered and freed
        //   during this call to `fops_open`.
        // * The `misc_ptr` always points to the `inner` field of a `MiscDeviceRegistration<T>`.
        // * The `MiscDeviceRegistration<T>` is valid until the `struct miscdevice` was
        //   unregistered.
        // * `MiscDeviceRegistration<T>` is `Send` since `MiscDeviceRegistration::register` has a
        //   `T::Data: Sync` bound,  `MiscDeviceRegistration<T>` is Send if `T::Data: Sync` and is
        //   the only way to create a `MiscDeviceRegistration`. This means that a reference to it
        //   can be shared between contexts.
        // TODO: add `assert_sync` for `MiscDeviceRegistration<T>` and
        // `MiscDeviceRegistration<T>::Data`.
        let registration = unsafe { &*container_of!(misc_ptr, MiscDeviceRegistration<T>, inner) };

        // SAFETY:
        // * This underlying file is valid for (much longer than) the duration of `T::open`.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(raw_file) };

        let ptr = match T::open(file, registration) {
            Ok(ptr) => ptr,
            Err(err) => return err.to_errno(),
        };

        // This overwrites the private data with the value specified by the user, changing the type
        // of this file's private data. All future accesses to the private data is performed by
        // other fops_* methods in this file, which all correctly cast the private data to the new
        // type.
        //
        // SAFETY: The open call of a file can access the private data.
        unsafe { (*raw_file).private_data = ptr.into_foreign().cast() };

        0
    }

    /// # Safety
    ///
    /// `file` and `inode` must be the file and inode for a file that is being released. The file
    /// must be associated with a `MiscDeviceRegistration<T>`.
    unsafe extern "C" fn release(_inode: *mut bindings::inode, file: *mut bindings::file) -> c_int {
        // SAFETY: The release call of a file owns the private data.
        let private = unsafe { (*file).private_data }.cast();
        // SAFETY: The release call of a file owns the private data.
        let ptr = unsafe { <T::Ptr as ForeignOwnable>::from_foreign(private) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        T::release(ptr, unsafe { File::from_raw_file(file) });

        0
    }

    /// # Safety
    ///
    /// `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    /// `vma` must be a vma that is currently being mmap'ed with this file.
    unsafe extern "C" fn mmap(
        file: *mut bindings::file,
        vma: *mut bindings::vm_area_struct,
    ) -> c_int {
        // SAFETY: The mmap call of a file can access the private data.
        let private = unsafe { (*file).private_data };
        // SAFETY: This is a Rust Miscdevice, so we call `into_foreign` in `open` and
        // `from_foreign` in `release`, and `fops_mmap` is guaranteed to be called between those
        // two operations.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(private.cast()) };
        // SAFETY: The caller provides a vma that is undergoing initial VMA setup.
        let area = unsafe { VmaNew::from_raw(vma) };
        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(file) };

        match T::mmap(device, file, area) {
            Ok(()) => 0,
            Err(err) => err.to_errno(),
        }
    }

    /// # Safety
    ///
    /// `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    unsafe extern "C" fn ioctl(file: *mut bindings::file, cmd: c_uint, arg: c_ulong) -> c_long {
        // SAFETY: The ioctl call of a file can access the private data.
        let private = unsafe { (*file).private_data }.cast();
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(file) };

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
        file: *mut bindings::file,
        cmd: c_uint,
        arg: c_ulong,
    ) -> c_long {
        // SAFETY: The compat ioctl call of a file can access the private data.
        let private = unsafe { (*file).private_data }.cast();
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };

        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(file) };

        match T::compat_ioctl(device, file, cmd, arg) {
            Ok(ret) => ret as c_long,
            Err(err) => err.to_errno() as c_long,
        }
    }

    /// # Safety
    ///
    /// - `file` must be a valid file that is associated with a `MiscDeviceRegistration<T>`.
    /// - `seq_file` must be a valid `struct seq_file` that we can write to.
    unsafe extern "C" fn show_fdinfo(seq_file: *mut bindings::seq_file, file: *mut bindings::file) {
        // SAFETY: The release call of a file owns the private data.
        let private = unsafe { (*file).private_data }.cast();
        // SAFETY: Ioctl calls can borrow the private data of the file.
        let device = unsafe { <T::Ptr as ForeignOwnable>::borrow(private) };
        // SAFETY:
        // * The file is valid for the duration of this call.
        // * There is no active fdget_pos region on the file on this thread.
        let file = unsafe { File::from_raw_file(file) };
        // SAFETY: The caller ensures that the pointer is valid and exclusive for the duration in
        // which this method is called.
        let m = unsafe { SeqFile::from_raw(seq_file) };

        T::show_fdinfo(device, m, file);
    }

    const VTABLE: bindings::file_operations = bindings::file_operations {
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
