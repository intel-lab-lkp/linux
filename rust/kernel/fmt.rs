// SPDX-License-Identifier: GPL-2.0

//! Formatting utilities.
//!
//! This module is intended to be used in place of `core::fmt` in kernel code.

pub use core::fmt::{Arguments, Debug, Error, Formatter, Result, Write};

use crate::ptr::{
    HashedPtr,
    RawPtr,
    RestrictedPtr, //
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

use core::fmt::{Binary, LowerExp, LowerHex, Octal, Pointer, UpperExp, UpperHex};
impl_fmt_adapter_forward!(Debug, LowerHex, UpperHex, Octal, Binary, LowerExp, UpperExp);

// Special handling for Pointer: default to HashedPtr for raw pointers.
// This overrides the default Pointer implementation for raw pointers to use hashing,
// which is the safe default behavior for kernel pointers.
impl<T> Pointer for Adapter<*const T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr::from(*ptr), f)
    }
}

impl<T> Pointer for Adapter<*mut T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr::from_mut(*ptr), f)
    }
}

// Handle references to raw pointers (needed when pointers are passed by reference in macros).
impl<T> Pointer for Adapter<&*const T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr::from(**ptr), f)
    }
}

impl<T> Pointer for Adapter<&*mut T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        let Self(ptr) = self;
        Pointer::fmt(&HashedPtr::from_mut(**ptr), f)
    }
}

// For wrapper types that implement Pointer (like HashedPtr, RawPtr, RestrictedPtr),
// forward to their implementation. This allows explicit wrapper types to use their
// own formatting logic instead of being converted to HashedPtr.
macro_rules! impl_pointer_adapter_forward {
    ($($ty:ty),* $(,)?) => {
        $(
            impl Pointer for Adapter<$ty> {
                fn fmt(&self, f: &mut Formatter<'_>) -> Result {
                    let Self(t) = self;
                    Pointer::fmt(t, f)
                }
            }

            impl Pointer for Adapter<&$ty> {
                fn fmt(&self, f: &mut Formatter<'_>) -> Result {
                    let Self(t) = self;
                    Pointer::fmt(*t, f)
                }
            }
        )*
    };
}

impl_pointer_adapter_forward!(
    HashedPtr,
    RawPtr,
    RestrictedPtr, //
);

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
