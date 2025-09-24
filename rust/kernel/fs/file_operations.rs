// SPDX-License-Identifier: GPL-2.0

//! Wrapper for struct file_operations.
//!
//! C headers: [`include/linux/fs.h`](srctree/include/linux/fs.h).

use macros::vtable;

#[cfg(CONFIG_COMPAT)]
use crate::fs::File;
use crate::{
    build_error,
    error::{Result, VTABLE_DEFAULT_ERROR},
    miscdevice::MiscDeviceRegistration,
    mm::virt::VmaNew,
    seq_file::SeqFile,
    types::ForeignOwnable, uaccess::{UserSliceReader, UserSliceWriter},
};

/// Trait implemented by the private data of an open misc device.
#[vtable]
pub trait FileOperations: Sized {
    /// What kind of pointer should `Self` be wrapped in.
    type Ptr: ForeignOwnable + Send + Sync;

    /// Handler for read.
    fn read(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        _buf: UserSliceWriter,
        _offset: &mut i64,
    ) -> Result<i64> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Handler for write.
    fn write(
        _device: <Self::Ptr as ForeignOwnable>::Borrowed<'_>,
        mut _buf: UserSliceReader,
        _offset: &mut i64,
    ) -> Result<i64> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

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
