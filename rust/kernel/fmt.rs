// SPDX-License-Identifier: GPL-2.0

//! Formatting utilities.
//!
//! This module is intended to be used in place of `core::fmt` in kernel code.

pub use core::fmt::{Arguments, Debug, Error, Formatter, Result, Write};

/// Internal adapter used to route and allow implementations of formatting traits for foreign types.
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

/// A copy of [`core::fmt::Pointer`] that allows implementing pointer formatting
/// for foreign types.
///
/// Together with the [`Adapter`] type and [`fmt!`] macro, it enables raw pointer
/// formatting to be intercepted and routed to [`HashedPtr`] (kernel's `%p` hashed
/// format), preventing kernel address leaks.
///
/// [`fmt!`]: crate::prelude::fmt!
pub trait Pointer {
    /// Same as [`core::fmt::Pointer::fmt`].
    fn fmt(&self, f: &mut Formatter<'_>) -> Result;
}

/// A wrapper for pointers that formats them using kernel's `%p` format specifier.
///
/// By default, `%p` prints a hashed representation of the pointer address to prevent
/// kernel address leaks. When the `no_hash_pointers` kernel command-line parameter is
/// enabled, the real address is printed instead (for debugging purposes).
pub struct HashedPtr<T: ?Sized>(pub *const T);

impl<T: ?Sized> Pointer for HashedPtr<T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        use crate::str::CStrExt as _;

        let mut buf = [0u8; 32];

        // SAFETY:
        // - `buf` is a valid writable buffer. 32 bytes is sufficient for all architectures:
        //   64-bit: "0x" + 16 hex digits (zero-padded by `pointer_string`) = 18 bytes;
        //   32-bit: "0x" + 8 hex digits (zero-padded by `pointer_string`) = 10 bytes.
        // - The format string is valid and `%p` expects a pointer argument
        // - `scnprintf` is safe to call with proper arguments
        //
        // Note: "0x" is added because Rust's `{:p}` includes a "0x" prefix,
        // but the kernel's `%p` does not.
        let len = unsafe {
            crate::bindings::scnprintf(
                buf.as_mut_ptr().cast(),
                buf.len(),
                c"0x%p".as_char_ptr(),
                self.0.cast::<core::ffi::c_void>(),
            )
        };

        // SAFETY:
        // - `buf` is a valid buffer (see above for size justification)
        // - `scnprintf` returns the number of characters written (excluding null terminator),
        //   which is always non-negative (see `vsnprintf` and `vscnprintf` in lib/vsprintf.c)
        // - `len` is bounded by `scnprintf` to at most `buf.len() - 1`
        // - The format string "0x%p" produces ASCII hex digits and "0x" prefix,
        //   which are valid UTF-8 (ASCII is a strict subset of UTF-8)
        let hashed_str = unsafe { core::str::from_utf8_unchecked(&buf[..len as usize]) };

        // Use `f.pad` to handle width/alignment formatting.
        f.pad(hashed_str)
    }
}

// Raw pointers are formatted via `HashedPtr` (kernel `%p`: hashed by default, plain with
// `no_hash_pointers`).
impl<T: ?Sized> Pointer for *const T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        Pointer::fmt(&HashedPtr(*self), f)
    }
}

impl<T: ?Sized> Pointer for *mut T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        <*const T as Pointer>::fmt(&(*self).cast_const(), f)
    }
}

impl<T: ?Sized> Pointer for &T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        <*const T as Pointer>::fmt(&core::ptr::from_ref(*self), f)
    }
}

impl<T: ?Sized> Pointer for &mut T {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        <*const T as Pointer>::fmt(&core::ptr::from_ref(*self), f)
    }
}

// `Adapter<&T>` bridges our `Pointer` trait to `core::fmt::Pointer`
impl<T: Pointer> core::fmt::Pointer for Adapter<&T> {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        Pointer::fmt(self.0, f)
    }
}

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

#[macros::kunit_tests(rust_kernel_fmt)]
mod tests {
    use crate::{
        bindings,
        prelude::fmt,
        str::CString, //
    };

    /// A RAII guard that temporarily sets `no_hash_pointers` and restores it on drop.
    ///
    /// # Safety
    ///
    /// `no_hash_pointers` is a `__ro_after_init` global variable. KUnit tests run during
    /// `kernel_init_freeable()` (init/main.c), before `mark_readonly()` makes the
    /// `.data..ro_after_init` section read-only. At this point there are no concurrent
    /// readers or writers, so it is safe to modify.
    struct NoHashPointersGuard {
        original: bool,
    }

    impl NoHashPointersGuard {
        /// Sets `no_hash_pointers` to `value` and returns a guard that will restore
        /// the original value on drop.
        ///
        /// # Safety
        ///
        /// See struct-level documentation.
        unsafe fn new(value: bool) -> Self {
            // SAFETY: See `NoHashPointersGuard` safety documentation.
            let original = unsafe { bindings::no_hash_pointers };
            // SAFETY: See `NoHashPointersGuard` safety documentation.
            unsafe { bindings::no_hash_pointers = value };
            Self { original }
        }
    }

    impl Drop for NoHashPointersGuard {
        fn drop(&mut self) {
            // SAFETY: See `NoHashPointersGuard` safety documentation.
            unsafe { bindings::no_hash_pointers = self.original };
        }
    }

    #[cfg(CONFIG_64BIT)]
    mod expected {
        pub(super) const PTR_VALUE: usize = 0xffffffffdeadbeef;
        pub(super) const HASHED_PREFIX: &str = "0x00000000";
        pub(super) const RAW_POINTER: &str = "0xffffffffdeadbeef";
        pub(super) const PADDED_LEFT: &str = "0xffffffffdeadbeef      ";
    }

    #[cfg(not(CONFIG_64BIT))]
    mod expected {
        pub(super) const PTR_VALUE: usize = 0xdeadbeef;
        pub(super) const HASHED_PREFIX: &str = "0x";
        pub(super) const RAW_POINTER: &str = "0xdeadbeef";
        pub(super) const PADDED_LEFT: &str = "0xdeadbeef              ";
    }

    #[test]
    fn test_ptr_formatting() -> core::result::Result<(), crate::error::Error> {
        let ptr = expected::PTR_VALUE as *const u8;

        // SAFETY: See `NoHashPointersGuard` safety documentation.
        let _hashed = unsafe { NoHashPointersGuard::new(false) };
        let cstr = CString::try_from_fmt(fmt!("{:p}", ptr))?;
        let formatted = cstr.to_str()?;
        assert!(formatted.starts_with(expected::HASHED_PREFIX));
        assert_ne!(formatted, expected::RAW_POINTER);
        drop(_hashed);

        // SAFETY: See `NoHashPointersGuard` safety documentation.
        let _guard = unsafe { NoHashPointersGuard::new(true) };
        let cstr = CString::try_from_fmt(fmt!("{:p}", ptr))?;
        assert_eq!(cstr.to_str()?, expected::RAW_POINTER);

        let cstr = CString::try_from_fmt(fmt!("{:024p}", ptr))?;
        assert_eq!(cstr.to_str()?, expected::PADDED_LEFT);

        Ok(())
    }
}
