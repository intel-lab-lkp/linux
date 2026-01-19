// SPDX-License-Identifier: GPL-2.0

//! Formatting utilities.
//!
//! This module is intended to be used in place of `core::fmt` in kernel code.

pub use core::fmt::{Arguments, Debug, Error, Formatter, Result, Write};

use crate::{
    bindings,
    ffi::c_void, //
};

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
/// [`fmt!`]: crate::prelude::fmt!
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

/// Macro to implement `core::fmt::Pointer` bridge for types that already
/// implement `Pointer`. This creates a bridge from `core::fmt::Pointer`
/// to `Pointer`.
#[allow(unused_macros)]
macro_rules! impl_pointer_forward {
    ($(
        $( { $($generics:tt)* } )? $ty:ty $( { where $($where:tt)* } )?
    ),* $(,)?) => {
        $(
            impl$($($generics)*)? core::fmt::Pointer for $ty $(where $($where)*)? {
                fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
                    <Self as Pointer>::fmt(self, f)
                }
            }
        )*
    };
}

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
/// pr_info!("Hashed pointer: {:016p}\n", ptr);
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
        // Buffer size chosen to accommodate pointer formatting
        // 32 bytes is sufficient for all pointer formats including hash values
        let mut buf = [0u8; 32];

        // SAFETY: We're calling the kernel's scnprintf function which is
        // safe to use. The buffer is large enough to hold the formatted
        // pointer string. Using "%p" format specifier applies kernel's
        // pointer hashing automatically.
        let len =
            unsafe { bindings::scnprintf(buf.as_mut_ptr(), buf.len(), c"0x%p".as_ptr(), ptr) };

        // scnprintf returns the number of characters written (excluding
        // null terminator), and guarantees it never exceeds size-1
        let formatted = core::str::from_utf8(&buf[..len as usize]).map_err(|_| Error)?;

        // Use pad() to respect formatting options (width, alignment,
        // padding, etc.)
        f.pad(formatted)
    }
}

impl_pointer_forward!(
    {<T>} HashedPtr<T>,
);

// Special handling for Pointer: default to HashedPtr for raw pointers.
// This overrides the default Pointer implementation for raw pointers to
// use hashing, which is the safe default behavior for kernel pointers.
impl<T> Pointer for Adapter<*const T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr(*ptr), f)
    }
}

impl<T> Pointer for Adapter<*mut T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr(*ptr), f)
    }
}

// Handle references to raw pointers (needed when pointers are passed by
// reference in macros).
impl<T> Pointer for Adapter<&*const T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr(**ptr), f)
    }
}

impl<T> Pointer for Adapter<&*mut T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr(**ptr), f)
    }
}

// Bridge implementations for raw pointer adapters.
impl_pointer_forward!(
    {<T>} Adapter<*const T>,
    {<T>} Adapter<*mut T>,
    {<T>} Adapter<&*const T>,
    {<T>} Adapter<&*mut T>,
);

/// A pointer that will be printed as its raw address (corresponds to `%px`).
///
/// **Warning**: This exposes the real kernel address and should only be
/// used for debugging purposes. Consider using [`HashedPtr`] instead for
/// production code.
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
/// let ptr = RawPtr(0x12345678 as *const u8);
/// pr_info!("Raw pointer: {:016p}\n", ptr);
///
/// // Width option test
/// let cstr = CString::try_from_fmt(fmt!("{:30p}", ptr))?;
/// let width_30 = cstr.to_str()?;
/// assert_eq!(width_30.len(), 30);
/// # Ok::<(), kernel::error::Error>(())
/// ```
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct RawPtr<T>(pub *const T);

impl<T> Pointer for RawPtr<T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        // Directly format the raw address - no hashing or restriction.
        // This corresponds to %px behavior.
        core::fmt::Pointer::fmt(&self.0.cast::<c_void>(), f)
    }
}

impl_pointer_forward!(
    {<T>} RawPtr<T>,
);
