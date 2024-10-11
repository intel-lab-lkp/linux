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
///
/// Normally, this trait is derived for a command enum.
///
/// # Example
///
/// ```
/// #[derive(IoctlCommand)]
/// #[ioctl(code = 0x18, start_num = 0)]
/// enum Command {
///     NoReadWrite,                 // No read or write access.
///     NoReadWriteButTakesArg(u64), // No read or write access, but takes an argument.
///     ReadOnly(UserSliceWriter),   // We write data for the user to read.
///     WriteOnly(UserSliceReader),  // We read data that the user wrote.
///     WriteAndRead(UserSlice),     // We read data from the user and then write data to the user.
/// }
/// ```
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

/// Support macro for deriving the `IoctlCommand` trait.
#[doc(hidden)]
#[macro_export]
macro_rules! __derive_ioctl_cmd {
    (parse_input:
        @enum_name($enum_name:ident),
        @code($code:literal),
        @variants(
            $(
                @variant($i:literal, $variant:ident, $arg_type:tt),
            )*
        )
    ) => {
        #[automatically_derived]
        impl $crate::ioctl::IoctlCommand for $enum_name {
            type Err = $crate::error::Error;

            const USE_VTABLE_ATTR: () = ();

            const HAS_PARSE: bool = true;

            fn parse(
                cmd: ::core::ffi::c_uint,
                arg: ::core::ffi::c_ulong,
            ) -> ::core::result::Result<Self, Self::Err> {
                let ty = $crate::ioctl::_IOC_TYPE(cmd) as u8;

                if ty != $code {
                    return Err($crate::error::code::ENOTTY);
                }

                let nr = $crate::ioctl::_IOC_NR(cmd) as u8;
                let dir = $crate::ioctl::_IOC_DIR(cmd);
                let size = $crate::ioctl::_IOC_SIZE(cmd);

                // Make sure we don't get unused parameter warnings
                let _ = arg;

                match (nr, dir, size) {
                    $(
                        ::kernel::__derive_ioctl_cmd!(
                            match_pattern:
                                @variant($i, $arg_type)
                        ) => ::kernel::__derive_ioctl_cmd!(
                            match_body:
                                @dir(dir),
                                @size(size),
                                @arg(arg),
                                @variant($variant, $arg_type)
                        ),
                    )*
                    _ => Err($crate::error::code::ENOTTY),
                }
            }
        }
    };
    (match_pattern:
        @variant($i:literal, None)
    ) => {
        ($i, $crate::uapi::_IOC_NONE, 0)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, None)
    ) => {
        Ok(Self::$variant)
    };
    (match_pattern:
        @variant($i:literal, u64)
    ) => {
        ($i, $crate::uapi::_IOC_NONE, 0)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, u64)
    ) => {
        Ok(Self::$variant($arg))
    };
    (match_pattern:
        @variant($i:literal, UserSliceWriter)
    ) => {
        ($i, $crate::uapi::_IOC_READ, _)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, UserSliceWriter)
    ) => {
        {
            let user_writer = $crate::uaccess::UserSlice::new(
                $arg as $crate::uaccess::UserPtr,
                $size
            )
            .writer();

            Ok(Self::$variant(user_writer))
        }
    };
    (match_pattern:
        @variant($i:literal, UserSliceReader)
    ) => {
        ($i, $crate::uapi::_IOC_WRITE, _)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, UserSliceReader)
    ) => {
        {
            let user_reader = $crate::uaccess::UserSlice::new(
                $arg as $crate::uaccess::UserPtr,
                $size
            )
            .reader();

            Ok(Self::$variant(user_reader))
        }
    };
    (match_pattern:
        @variant($i:literal, UserSlice)
    ) => {
        ($i, _, _)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, UserSlice)
    ) => {
        // Unfortunately, we cannot just do a match guard
        if $dir != $crate::uapi::_IOC_READ | $crate::uapi::_IOC_WRITE {
            Err($crate::error::code::ENOTTY)
        } else {
            let user_slice = $crate::uaccess::UserSlice::new(
                $arg as $crate::uaccess::UserPtr,
                $size
            );

            Ok(Self::$variant(user_slice))
        }
    };
    (match_pattern:
        @variant($i:literal, $arg_type:tt)
    ) => {
        ($i, _, _)
    };
    (match_body:
        @dir($dir:ident),
        @size($size:ident),
        @arg($arg:ident),
        @variant($variant:ident, $arg_type:tt)
    ) => {
        {
            // We have an unsupported argument type
            const _: () = ::core::assert!(
                false,
                ::core::concat!(
                    "Invalid argument type for ioctl command ",
                    stringify!($variant),
                    ": ",
                    stringify!($arg_type),
                )
            );
            ::core::unreachable!()
        }
    };
}
