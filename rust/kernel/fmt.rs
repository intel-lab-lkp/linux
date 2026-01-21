// SPDX-License-Identifier: GPL-2.0

//! Formatting utilities.
//!
//! This module is intended to be used in place of `core::fmt` in kernel code.

pub use core::fmt::{Arguments, Debug, Error, Formatter, Result, Write};

/// Internal adapter used to route allow implementations of formatting traits for foreign types.
///
/// It is inserted automatically by the [`fmt!`] macro and is not meant to be used directly.
///
/// [`fmt!`]: crate::prelude::fmt!
#[doc(hidden)]
pub struct Adapter<T>(pub T);

macro_rules! impl_fmt_adapter_forward {
    ($($trait:ident),* $(,)?) => {
        $(
            impl<T: $trait> $trait for Adapter<T> {
                fn fmt(&self, f: &mut Formatter<'_>) -> Result {
                    let Self(t) = self;
                    $trait::fmt(t, f)
                }
            }
        )*
    };
}

use core::fmt::{Binary, LowerExp, LowerHex, Octal, UpperExp, UpperHex};
impl_fmt_adapter_forward!(Debug, LowerHex, UpperHex, Octal, Binary, LowerExp, UpperExp);

/// A copy of [`core::fmt::Display`] that allows us to implement it for foreign types.
///
/// Types should implement this trait rather than [`core::fmt::Display`]. Together with the
/// [`Adapter`] type and [`fmt!`] macro, it allows for formatting foreign types (e.g. types from
/// core) which do not implement [`core::fmt::Display`] directly.
///
/// [`fmt!`]: crate::prelude::fmt!
pub trait Display {
    /// Same as [`core::fmt::Display::fmt`].
    fn fmt(&self, f: &mut Formatter<'_>) -> Result;
}

impl<T: ?Sized + Display> Display for &T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        Display::fmt(*self, f)
    }
}

impl<T: ?Sized + Display> core::fmt::Display for Adapter<&T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(t) = self;
        Display::fmt(t, f)
    }
}

macro_rules! impl_display_forward {
    ($(
        $( { $($generics:tt)* } )? $ty:ty $( { where $($where:tt)* } )?
    ),* $(,)?) => {
        $(
            impl$($($generics)*)? Display for $ty $(where $($where)*)? {
                fn fmt(&self, f: &mut Formatter<'_>) -> Result {
                    core::fmt::Display::fmt(self, f)
                }
            }
        )*
    };
}

impl_display_forward!(
    bool,
    char,
    core::panic::PanicInfo<'_>,
    Arguments<'_>,
    i128,
    i16,
    i32,
    i64,
    i8,
    isize,
    str,
    u128,
    u16,
    u32,
    u64,
    u8,
    usize,
    {<T: ?Sized>} crate::sync::Arc<T> {where crate::sync::Arc<T>: core::fmt::Display},
    {<T: ?Sized>} crate::sync::UniqueArc<T> {where crate::sync::UniqueArc<T>: core::fmt::Display},
);

/// A copy of [`core::fmt::Pointer`] that allows us to implement it for
/// foreign types.
///
/// Types should implement this trait rather than [`core::fmt::Pointer`].
/// Together with the [`Adapter`] type and [`fmt!`] macro, it allows for
/// formatting foreign types (e.g. types from core) which do not implement
/// [`core::fmt::Pointer`] directly.
///
/// By default, raw pointers (`*const T` and `*mut T`) are formatted using
/// [`HashedPtr`], which hashes the pointer value to prevent leaking kernel
/// memory layout information.
///
/// [`fmt!`]: crate::prelude::fmt!
/// # Example
///
/// ```
/// use kernel::{
///     prelude::fmt,
///     str::CString, //
/// };
///
/// // Works with const pointers
/// let ptr_const = 0x12345678 as *const u8;
/// pr_info!("Default pointer: {:p}\n", ptr_const);
///
/// // Works with mut pointers
/// let ptr_mut = 0x12345678 as *mut u8;
/// pr_info!("Default pointer: {:p}\n", ptr_mut);
///
/// // Width option test
/// let cstr = CString::try_from_fmt(fmt!("{:30p}", ptr_const))?;
/// assert_eq!(cstr.to_str()?.len(), 30);
/// # Ok::<(), kernel::error::Error>(())
/// ```
pub trait Pointer {
    /// Same as [`core::fmt::Pointer::fmt`].
    fn fmt(&self, f: &mut Formatter<'_>) -> Result;
}

impl<T: ?Sized + Pointer> Pointer for &T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        Pointer::fmt(*self, f)
    }
}

impl<T: ?Sized + Pointer> core::fmt::Pointer for Adapter<&T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(t) = self;
        Pointer::fmt(t, f)
    }
}

macro_rules! impl_hashed_pointer {
    ($($ptr_ty:ty),* $(,)?) => {
        $(
            impl<T> Pointer for $ptr_ty {
                fn fmt(&self, f: &mut Formatter<'_>) -> Result {
                    Pointer::fmt(&HashedPtr(*self), f)
                }
            }
        )*
    };
}

impl_hashed_pointer!(*const T, *mut T);

/// A pointer that will be hashed when printed (corresponds to `%p`).
///
/// This is the default behavior for kernel pointers - they are hashed to
/// prevent leaking information about the kernel memory layout.
///
/// # Example
///
/// ```
/// use kernel::{
///     fmt::HashedPtr,
///     prelude::fmt,
///     str::CString, //
/// };
///
/// let ptr = HashedPtr(0x12345678 as *const u8);
/// pr_info!("Hashed pointer: {:p}\n", ptr);
///
/// // Width option test
/// let cstr = CString::try_from_fmt(fmt!("{:30p}", ptr))?;
/// let width_30 = cstr.to_str()?;
/// assert_eq!(width_30.len(), 30);
/// # Ok::<(), kernel::error::Error>(())
/// ```
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct HashedPtr<T>(pub *const T);

impl<T> Pointer for HashedPtr<T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let ptr = self.0;
        // The default width for %p is 2 * sizeof(ptr) (see lib/vsprintf.c:
        // pointer_string() and ptr_to_id()), which is 16 characters on
        // 64-bit systems. With the "0x" prefix and null terminator, this
        // is at most 19 characters. 32 bytes is sufficient for all pointer
        // formats including hash values.
        let mut buf = [0u8; 32];

        // SAFETY: scnprintf never writes more than size-1 characters, so
        // len is at most buf.len() - 1.
        // The "%p" format produces a fixed pointer format: "0x..." (e.g.,
        // "0x00000000b231c2e6").
        let len =
            unsafe { bindings::scnprintf(buf.as_mut_ptr(), buf.len(), c"0x%p".as_ptr(), ptr) };
        let formatted = core::str::from_utf8(&buf[..len as usize]).map_err(|_| Error)?;

        // Use pad() to respect width and alignment. Padding with explicit
        // characters (e.g., {:0>16p}) is supported, but zero-padding
        // shorthand (e.g., {:016p}) is not.
        f.pad(formatted)
    }
}

/// A wrapper that formats any type implementing [`core::fmt::Pointer`] as its
/// raw address without hashing (corresponds to `%px`).
///
/// **Warning**: This exposes the real kernel address and should only be
/// used for debugging purposes.
///
/// # Example
///
/// ```
/// use kernel::{
///     fmt::RawPtr,
///     prelude::fmt,
///     str::CString, //
/// };
///
/// // Works with const pointers
/// let ptr_const = RawPtr(0x12345678 as *const u8);
/// pr_info!("Raw pointer: {:018p}\n", ptr_const);
///
/// // Works with mut pointers
/// let ptr_mut = RawPtr(0x12345678 as *mut u8);
/// pr_info!("Raw pointer: {:018p}\n", ptr_mut);
///
/// // Works with references
/// let value = 42u8;
/// let ptr_ref = RawPtr(&value);
/// pr_info!("Raw pointer: {:018p}\n", ptr_ref);
///
/// // Width option test
/// let cstr = CString::try_from_fmt(fmt!("{:30p}", ptr_const))?;
/// let width_30 = cstr.to_str()?;
/// assert_eq!(width_30.len(), 30);
/// # Ok::<(), kernel::error::Error>(())
/// ```
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct RawPtr<T>(pub T);

impl<T: core::fmt::Pointer> Pointer for RawPtr<T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        // Directly format the raw address - no hashing or restriction.
        // This corresponds to %px behavior.
        core::fmt::Pointer::fmt(&self.0, f)
    }
}
