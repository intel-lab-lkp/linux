// SPDX-License-Identifier: GPL-2.0

//! Types and functions to work with pointers and addresses.
//!
//! This module provides wrapper types for formatting kernel pointers that correspond to the
//! C kernel's printk format specifiers `%p` and `%px`.

use core::{
    fmt,
    fmt::Pointer,
    mem::align_of,
    num::NonZero, //
};

use crate::{
    bindings,
    build_assert,
    prelude::*, //
};

/// Type representing an alignment, which is always a power of two.
///
/// It is used to validate that a given value is a valid alignment, and to perform masking and
/// alignment operations.
///
/// This is a temporary substitute for the [`Alignment`] nightly type from the standard library,
/// and to be eventually replaced by it.
///
/// [`Alignment`]: https://github.com/rust-lang/rust/issues/102070
///
/// # Invariants
///
/// An alignment is always a power of two.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Alignment(NonZero<usize>);

impl Alignment {
    /// Validates that `ALIGN` is a power of two at build-time, and returns an [`Alignment`] of the
    /// same value.
    ///
    /// A build error is triggered if `ALIGN` is not a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// let v = Alignment::new::<16>();
    /// assert_eq!(v.as_usize(), 16);
    /// ```
    #[inline(always)]
    pub const fn new<const ALIGN: usize>() -> Self {
        build_assert!(
            ALIGN.is_power_of_two(),
            "Provided alignment is not a power of two."
        );

        // INVARIANT: `align` is a power of two.
        // SAFETY: `align` is a power of two, and thus non-zero.
        Self(unsafe { NonZero::new_unchecked(ALIGN) })
    }

    /// Validates that `align` is a power of two at runtime, and returns an
    /// [`Alignment`] of the same value.
    ///
    /// Returns [`None`] if `align` is not a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new_checked(16), Some(Alignment::new::<16>()));
    /// assert_eq!(Alignment::new_checked(15), None);
    /// assert_eq!(Alignment::new_checked(1), Some(Alignment::new::<1>()));
    /// assert_eq!(Alignment::new_checked(0), None);
    /// ```
    #[inline(always)]
    pub const fn new_checked(align: usize) -> Option<Self> {
        if align.is_power_of_two() {
            // INVARIANT: `align` is a power of two.
            // SAFETY: `align` is a power of two, and thus non-zero.
            Some(Self(unsafe { NonZero::new_unchecked(align) }))
        } else {
            None
        }
    }

    /// Returns the alignment of `T`.
    ///
    /// This is equivalent to [`align_of`], but with the return value provided as an [`Alignment`].
    #[inline(always)]
    pub const fn of<T>() -> Self {
        #![allow(clippy::incompatible_msrv)]
        // This cannot panic since alignments are always powers of two.
        //
        // We unfortunately cannot use `new` as it would require the `generic_const_exprs` feature.
        const { Alignment::new_checked(align_of::<T>()).unwrap() }
    }

    /// Returns this alignment as a [`usize`].
    ///
    /// It is guaranteed to be a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new::<16>().as_usize(), 16);
    /// ```
    #[inline(always)]
    pub const fn as_usize(self) -> usize {
        self.as_nonzero().get()
    }

    /// Returns this alignment as a [`NonZero`].
    ///
    /// It is guaranteed to be a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new::<16>().as_nonzero().get(), 16);
    /// ```
    #[inline(always)]
    pub const fn as_nonzero(self) -> NonZero<usize> {
        // Allow the compiler to know that the value is indeed a power of two. This can help
        // optimize some operations down the line, like e.g. replacing divisions by bit shifts.
        if !self.0.is_power_of_two() {
            // SAFETY: Per the invariants, `self.0` is always a power of two so this block will
            // never be reached.
            unsafe { core::hint::unreachable_unchecked() }
        }
        self.0
    }

    /// Returns the base-2 logarithm of the alignment.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::of::<u8>().log2(), 0);
    /// assert_eq!(Alignment::new::<16>().log2(), 4);
    /// ```
    #[inline(always)]
    pub const fn log2(self) -> u32 {
        self.0.ilog2()
    }

    /// Returns the mask for this alignment.
    ///
    /// This is equivalent to `!(self.as_usize() - 1)`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new::<0x10>().mask(), !0xf);
    /// ```
    #[inline(always)]
    pub const fn mask(self) -> usize {
        // No underflow can occur as the alignment is guaranteed to be a power of two, and thus is
        // non-zero.
        !(self.as_usize() - 1)
    }
}

/// Trait for items that can be aligned against an [`Alignment`].
pub trait Alignable: Sized {
    /// Aligns `self` down to `alignment`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::{Alignable, Alignment};
    ///
    /// assert_eq!(0x2f_usize.align_down(Alignment::new::<0x10>()), 0x20);
    /// assert_eq!(0x30usize.align_down(Alignment::new::<0x10>()), 0x30);
    /// assert_eq!(0xf0u8.align_down(Alignment::new::<0x1000>()), 0x0);
    /// ```
    fn align_down(self, alignment: Alignment) -> Self;

    /// Aligns `self` up to `alignment`, returning `None` if aligning would result in an overflow.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::{Alignable, Alignment};
    ///
    /// assert_eq!(0x4fusize.align_up(Alignment::new::<0x10>()), Some(0x50));
    /// assert_eq!(0x40usize.align_up(Alignment::new::<0x10>()), Some(0x40));
    /// assert_eq!(0x0usize.align_up(Alignment::new::<0x10>()), Some(0x0));
    /// assert_eq!(u8::MAX.align_up(Alignment::new::<0x10>()), None);
    /// assert_eq!(0x10u8.align_up(Alignment::new::<0x100>()), None);
    /// assert_eq!(0x0u8.align_up(Alignment::new::<0x100>()), Some(0x0));
    /// ```
    fn align_up(self, alignment: Alignment) -> Option<Self>;
}

/// Implement [`Alignable`] for unsigned integer types.
macro_rules! impl_alignable_uint {
    ($($t:ty),*) => {
        $(
        impl Alignable for $t {
            #[inline(always)]
            fn align_down(self, alignment: Alignment) -> Self {
                // The operands of `&` need to be of the same type so convert the alignment to
                // `Self`. This means we need to compute the mask ourselves.
                ::core::num::NonZero::<Self>::try_from(alignment.as_nonzero())
                    .map(|align| self & !(align.get() - 1))
                    // An alignment larger than `Self` always aligns down to `0`.
                    .unwrap_or(0)
            }

            #[inline(always)]
            fn align_up(self, alignment: Alignment) -> Option<Self> {
                let aligned_down = self.align_down(alignment);
                if self == aligned_down {
                    Some(aligned_down)
                } else {
                    Self::try_from(alignment.as_usize())
                        .ok()
                        .and_then(|align| aligned_down.checked_add(align))
                }
            }
        }
        )*
    };
}

impl_alignable_uint!(u8, u16, u32, u64, usize);

/// Placeholder string used when pointer hashing is not ready yet.
const PTR_PLACEHOLDER: &str = if size_of::<*const c_void>() == 8 {
    "(____ptrval____)"
} else {
    "(ptrval)"
};

/// Helper function to hash a pointer and format it.
///
/// Returns `Ok(())` if the hash was successfully computed and formatted,
/// or the placeholder string if hashing is not ready yet.
fn format_hashed_ptr(ptr: *const c_void, f: &mut fmt::Formatter<'_>) -> fmt::Result {
    let mut hashval: crate::ffi::c_ulong = 0;
    // SAFETY: We're calling the kernel's ptr_to_hashval function which handles
    // hashing. This is safe as long as ptr is a valid pointer value.
    let ret = unsafe { bindings::ptr_to_hashval(ptr, &mut hashval) };

    if ret != 0 {
        // Hash not ready yet, print placeholder with formatting options applied
        // Using `pad()` ensures width, alignment, and padding options are respected
        return f.pad(PTR_PLACEHOLDER);
    }

    // Successfully got hash value, format it using Pointer::fmt to preserve
    // formatting options (width, alignment, padding, etc.)
    Pointer::fmt(&(hashval as *const c_void), f)
}

/// A pointer that will be hashed when printed (corresponds to `%p`).
///
/// This is the default behavior for kernel pointers - they are hashed to prevent
/// leaking information about the kernel memory layout.
///
/// # Example
///
/// ```
/// use kernel::{
///     prelude::fmt,
///     ptr::HashedPtr,
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

impl<T> fmt::Pointer for HashedPtr<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Handle NULL pointers - print them directly
        let ptr = self.0.cast::<c_void>();
        if ptr.is_null() {
            return Pointer::fmt(&ptr, f);
        }

        format_hashed_ptr(ptr, f)
    }
}

/// A pointer that will be printed as its raw address (corresponds to `%px`).
///
/// **Warning**: This exposes the real kernel address and should only be used
/// for debugging purposes. Consider using [`HashedPtr`] instead for production code.
///
/// # Example
///
/// ```
/// use kernel::{
///     prelude::fmt,
///     ptr::RawPtr,
///     str::CString, //
/// };
///
/// let ptr = RawPtr(0x12345678 as *const u8);
///
/// // Basic formatting
/// let cstr = CString::try_from_fmt(fmt!("{:p}", ptr))?;
/// let formatted = cstr.to_str()?;
/// assert_eq!(formatted, "0x12345678");
///
/// // Right align with zero padding, width 30
/// let cstr = CString::try_from_fmt(fmt!("{:0>30p}", ptr))?;
/// let right_zero = cstr.to_str()?;
/// assert_eq!(right_zero, "000000000000000000000x12345678");
///
/// // Left align with zero padding, width 30
/// let cstr = CString::try_from_fmt(fmt!("{:0<30p}", ptr))?;
/// let left_zero = cstr.to_str()?;
/// assert_eq!(left_zero, "0x1234567800000000000000000000");
///
/// // Center align with zero padding, width 30
/// let cstr = CString::try_from_fmt(fmt!("{:0^30p}", ptr))?;
/// let center_zero = cstr.to_str()?;
/// assert_eq!(center_zero, "00000000000x123456780000000000");
/// # Ok::<(), kernel::error::Error>(())
/// ```
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct RawPtr<T>(pub *const T);

impl<T> fmt::Pointer for RawPtr<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Directly format the raw address - no hashing or restriction.
        // This corresponds to %px behavior.
        Pointer::fmt(&self.0.cast::<c_void>(), f)
    }
}
