// SPDX-License-Identifier: GPL-2.0

//! Types and functions to work with pointers and addresses.

use core::fmt::Debug;
use core::num::NonZero;
use core::ops::{BitAnd, Not};

use crate::build_assert;
use crate::num::CheckedAdd;

/// Type representing an alignment, which is always a power of two.
///
/// It be used to validate that a given value is a valid alignment, and to perform masking and
/// align down/up operations. The alignment operations are done using the [`align_up!`] and
/// [`align_down!`] macros.
///
/// Heavily inspired by the [`Alignment`] nightly feature from the Rust standard library, and
/// hopefully to be eventually replaced by it.
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
    /// Validates that `align` is a power of two at build-time, and returns an [`Alignment`] of the
    /// same value.
    ///
    /// A build error is triggered if `align` cannot be asserted to be a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// let v = Alignment::new(16);
    /// assert_eq!(v.as_usize(), 16);
    /// ```
    #[inline(always)]
    pub const fn new(align: usize) -> Self {
        build_assert!(align.is_power_of_two());

        // INVARIANT: `align` is a power of two.
        // SAFETY: `align` is a power of two, and thus non-zero.
        Self(unsafe { NonZero::new_unchecked(align) })
    }

    /// Validates that `align` is a power of two at runtime, and returns an
    /// [`Alignment`] of the same value.
    ///
    /// [`None`] is returned if `align` was not a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new_checked(16), Some(Alignment::new(16)));
    /// assert_eq!(Alignment::new_checked(15), None);
    /// assert_eq!(Alignment::new_checked(1), Some(Alignment::new(1)));
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
    #[inline(always)]
    pub const fn of<T>() -> Self {
        // INVARIANT: `align_of` always returns a power of 2.
        Self(unsafe { NonZero::new_unchecked(align_of::<T>()) })
    }

    /// Returns the base-2 logarithm of the alignment.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::of::<u8>().log2(), 0);
    /// assert_eq!(Alignment::new(16).log2(), 4);
    /// ```
    #[inline(always)]
    pub const fn log2(self) -> u32 {
        self.0.ilog2()
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
    /// assert_eq!(Alignment::new(16).as_nonzero().get(), 16);
    /// ```
    #[inline(always)]
    pub const fn as_nonzero(self) -> NonZero<usize> {
        if !self.0.is_power_of_two() {
            // SAFETY: per the invariants, `self.0` is always a power of two so this block will
            // never be reached.
            unsafe { core::hint::unreachable_unchecked() }
        }
        self.0
    }

    /// Returns this alignment as a `usize`.
    ///
    /// It is guaranteed to be a power of two.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new(16).as_usize(), 16);
    /// ```
    #[inline(always)]
    pub const fn as_usize(self) -> usize {
        self.as_nonzero().get()
    }

    /// Returns the mask corresponding to `self.as_usize() - 1`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new(0x10).mask(), 0xf);
    /// ```
    #[inline(always)]
    pub const fn mask(self) -> usize {
        // INVARIANT: `self.as_usize()` is guaranteed to be a power of two (i.e. non-zero), thus
        // `1` can safely be substracted from it.
        self.as_usize() - 1
    }

    /// Aligns `value` down to this alignment.
    ///
    /// If the alignment contained in `self` is too large for `T`, then `0` is returned, which
    /// is correct as it is also the result that would have been returned if it did.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new(0x10).align_down(0x2f), 0x20);
    /// assert_eq!(Alignment::new(0x10).align_down(0x30), 0x30);
    /// assert_eq!(Alignment::new(0x1000).align_down(0xf0u8), 0x0);
    /// ```
    #[inline(always)]
    pub fn align_down<T>(self, value: T) -> T
    where
        T: TryFrom<usize> + BitAnd<Output = T> + Not<Output = T> + Default,
    {
        T::try_from(self.mask())
            .map(|mask| value & !mask)
            .unwrap_or(T::default())
    }

    /// Aligns `value` up to this alignment, returning `None` if aligning pushes the result above
    /// the limits of `value`'s type.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::ptr::Alignment;
    ///
    /// assert_eq!(Alignment::new(0x10).align_up(0x4f), Some(0x50));
    /// assert_eq!(Alignment::new(0x10).align_up(0x40), Some(0x40));
    /// assert_eq!(Alignment::new(0x10).align_up(0x0), Some(0x0));
    /// assert_eq!(Alignment::new(0x10).align_up(u8::MAX), None);
    /// assert_eq!(Alignment::new(0x100).align_up(0x10u8), None);
    /// assert_eq!(Alignment::new(0x100).align_up(0x0u8), Some(0x0));
    /// ```
    #[inline(always)]
    pub fn align_up<T>(self, value: T) -> Option<T>
    where
        T: TryFrom<usize>
            + BitAnd<Output = T>
            + Not<Output = T>
            + Default
            + PartialEq
            + Copy
            + CheckedAdd,
    {
        let aligned_down = self.align_down(value);
        if value == aligned_down {
            Some(aligned_down)
        } else {
            T::try_from(self.as_usize())
                .ok()
                .and_then(|align| aligned_down.checked_add(align))
        }
    }
}
