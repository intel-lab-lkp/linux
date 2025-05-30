// SPDX-License-Identifier: GPL-2.0

//! Formatting utilities.

use core::fmt;

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
            impl<T: fmt::$trait> fmt::$trait for Adapter<T> {
                fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    let Self(t) = self;
                    fmt::$trait::fmt(t, f)
                }
            }
        )*
    };
}

impl_fmt_adapter_forward!(Debug, LowerHex, UpperHex, Octal, Binary, Pointer, LowerExp, UpperExp);

macro_rules! impl_display_forward {
    ($(
        $( { $($generics:tt)* } )? $ty:ty $( { where $($where:tt)* } )?
    ),* $(,)?) => {
        $(
            impl$($($generics)*)? fmt::Display for Adapter<&$ty>
            $(where $($where)*)? {
                fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    let Self(t) = self;
                    fmt::Display::fmt(t, f)
                }
            }
        )*
    };
}

impl<T: ?Sized> fmt::Display for Adapter<&&T>
where
    for<'a> Adapter<&'a T>: fmt::Display,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self(t) = self;
        Adapter::<&T>(**t).fmt(f)
    }
}

impl_display_forward!(
    bool,
    char,
    core::panic::PanicInfo<'_>,
    crate::str::BStr,
    fmt::Arguments<'_>,
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
    {<T: ?Sized>} crate::sync::Arc<T> {where crate::sync::Arc<T>: fmt::Display},
    {<T: ?Sized>} crate::sync::UniqueArc<T> {where crate::sync::UniqueArc<T>: fmt::Display},
);
