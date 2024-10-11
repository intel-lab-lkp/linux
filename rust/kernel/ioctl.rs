// SPDX-License-Identifier: GPL-2.0

//! `ioctl()` number definitions.
//!
//! C header: [`include/asm-generic/ioctl.h`](srctree/include/asm-generic/ioctl.h)

#![expect(non_snake_case)]

use crate::{build_assert, error::VTABLE_DEFAULT_ERROR, prelude::*};
use core::ffi;

/// Build an ioctl number, analogous to the C macro of the same name.
#[inline(always)]
const fn _IOC(dir: u32, ty: u32, nr: u32, size: usize) -> u32 {
    build_assert!(dir <= uapi::_IOC_DIRMASK);
    build_assert!(ty <= uapi::_IOC_TYPEMASK);
    build_assert!(nr <= uapi::_IOC_NRMASK);
    build_assert!(size <= (uapi::_IOC_SIZEMASK as usize));

    (dir << uapi::_IOC_DIRSHIFT)
        | (ty << uapi::_IOC_TYPESHIFT)
        | (nr << uapi::_IOC_NRSHIFT)
        | ((size as u32) << uapi::_IOC_SIZESHIFT)
}

/// Build an ioctl number for an argumentless ioctl.
#[inline(always)]
pub const fn _IO(ty: u32, nr: u32) -> u32 {
    _IOC(uapi::_IOC_NONE, ty, nr, 0)
}

/// Build an ioctl number for a read-only ioctl.
#[inline(always)]
pub const fn _IOR<T>(ty: u32, nr: u32) -> u32 {
    _IOC(uapi::_IOC_READ, ty, nr, core::mem::size_of::<T>())
}

/// Build an ioctl number for a write-only ioctl.
#[inline(always)]
pub const fn _IOW<T>(ty: u32, nr: u32) -> u32 {
    _IOC(uapi::_IOC_WRITE, ty, nr, core::mem::size_of::<T>())
}

/// Build an ioctl number for a read-write ioctl.
#[inline(always)]
pub const fn _IOWR<T>(ty: u32, nr: u32) -> u32 {
    _IOC(
        uapi::_IOC_READ | uapi::_IOC_WRITE,
        ty,
        nr,
        core::mem::size_of::<T>(),
    )
}

/// Get the ioctl direction from an ioctl number.
pub const fn _IOC_DIR(nr: u32) -> u32 {
    (nr >> uapi::_IOC_DIRSHIFT) & uapi::_IOC_DIRMASK
}

/// Get the ioctl type from an ioctl number.
pub const fn _IOC_TYPE(nr: u32) -> u32 {
    (nr >> uapi::_IOC_TYPESHIFT) & uapi::_IOC_TYPEMASK
}

/// Get the ioctl number from an ioctl number.
pub const fn _IOC_NR(nr: u32) -> u32 {
    (nr >> uapi::_IOC_NRSHIFT) & uapi::_IOC_NRMASK
}

/// Get the ioctl size from an ioctl number.
pub const fn _IOC_SIZE(nr: u32) -> usize {
    ((nr >> uapi::_IOC_SIZESHIFT) & uapi::_IOC_SIZEMASK) as usize
}

/// Types implementing this trait can be used to parse ioctl commands.
#[vtable]
pub trait IoctlCommand: Sized + Send + Sync + 'static {
    /// The error type returned by the parse functions.
    ///
    /// This type must be convertible into the kernel [`Error`] type.
    type Err: Into<Error>;

    /// Parse an ioctl command.
    ///
    /// This function parses the `cmd` argument as an ioctl command number
    /// and returns a command that interprets the `arg` argument as needed.
    ///
    /// # Errors
    ///
    /// This function may return an error if the command is invalid.
    fn parse(cmd: ffi::c_uint, arg: ffi::c_ulong) -> Result<Self, Self::Err>;

    /// Parse an ioctl command for compatibility mode.
    ///
    /// If the compatibility mode is enabled, this function parses the `cmd`
    /// argument as an ioctl command number and returns a command that
    /// interprets the `arg` argument as needed. The values come from a 32-bit
    /// user-space application and may need to be parsed differently.
    ///
    /// # Errors
    ///
    /// This function may return an error if the command is invalid.
    #[cfg(CONFIG_COMPAT)]
    fn compat_parse(_cmd: ffi::c_uint, _arg: ffi::c_ulong) -> Result<Self, Self::Err> {
        kernel::build_error(VTABLE_DEFAULT_ERROR)
    }
}

#[vtable]
impl IoctlCommand for () {
    type Err = Error;

    fn parse(_cmd: ffi::c_uint, _arg: ffi::c_ulong) -> Result<Self> {
        Err(ENOTTY)
    }
}
