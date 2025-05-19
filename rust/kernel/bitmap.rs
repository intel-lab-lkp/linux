// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! Rust API for bitmap.
//!
//! C headers: [`include/linux/bitmap.h`](srctree/include/linux/bitmap.h).

use crate::alloc::{AllocError, Flags};
use crate::bindings;
use core::ptr::NonNull;

/// Holds either a pointer to array of `unsigned long` or a small bitmap.
#[repr(C)]
union BitmapRepr {
    bitmap: usize,
    ptr: NonNull<usize>,
}

macro_rules! bitmap_hardening_assert {
    ($cond:expr, $($arg:tt)+) => {
        #[cfg(RUST_BITMAP_HARDENED)]
        assert!($e, $($arg)*);
    }
}

/// Represents a bitmap.
///
/// Wraps underlying C bitmap API.
///
/// # Examples
///
/// Basic usage
///
/// ```
/// use kernel::alloc::flags::GFP_KERNEL;
/// use kernel::bitmap::Bitmap;
///
/// let mut b = Bitmap::new(16, GFP_KERNEL)?;
///
/// assert_eq!(16, b.len());
/// for i in 0..16 {
///     if i % 4 == 0 {
///       b.set_bit(i);
///     }
/// }
/// assert_eq!(Some(0), b.next_bit(0));
/// assert_eq!(Some(1), b.next_zero_bit(0));
/// assert_eq!(Some(4), b.next_bit(1));
/// assert_eq!(Some(5), b.next_zero_bit(4));
/// assert_eq!(Some(12), b.last_bit());
/// # Ok::<(), Error>(())
/// ```
///
/// # Invariants
///
/// * `nbits` is `<= i32::MAX` and never changes.
/// * if `nbits <= bindings::BITS_PER_LONG`, then `repr` is a bitmap.
/// * otherwise, `repr` holds a non-null pointer that was obtained from a
///   successful call to `bitmap_zalloc` and holds the address of an initialized
///   array of `unsigned long` that is large enough to hold `nbits` bits.
pub struct Bitmap {
    /// Representation of bitmap.
    repr: BitmapRepr,
    /// Length of this bitmap. Must be `<= i32::MAX`.
    nbits: usize,
}

/// Enable unsynchronized concurrent access to [`Bitmap`] through shared references.
///
/// # Safety
///
/// * When no thread performs any mutations, concurrent access is safe.
/// * Mutations are permitted through atomic operations and interior mutability.
///   All such methods are marked unsafe, to account for the lack of ordering
///   guarantees. Callers must acknowledge that updates may be observed in any
///   order.
unsafe impl Sync for Bitmap {}

impl Drop for Bitmap {
    fn drop(&mut self) {
        if self.nbits <= bindings::BITS_PER_LONG as _ {
            return;
        }
        // SAFETY: `self.ptr` was returned by the C `bitmap_zalloc`.
        //
        // INVARIANT: there is no other use of the `self.ptr` after this
        // call and the value is being dropped so the broken invariant is
        // not observable on function exit.
        unsafe { bindings::bitmap_free(self.as_mut_ptr()) };
    }
}

impl Bitmap {
    /// Constructs a new [`Bitmap`].
    ///
    /// Fails with [`AllocError`] when the [`Bitmap`] could not be allocated. This
    /// includes the case when `nbits` is greater than `i32::MAX`.
    #[inline]
    pub fn new(nbits: usize, flags: Flags) -> Result<Self, AllocError> {
        if nbits <= bindings::BITS_PER_LONG as _ {
            return Ok(Bitmap {
                repr: BitmapRepr { bitmap: 0 },
                nbits,
            });
        }
        if nbits > i32::MAX.try_into().unwrap() {
            return Err(AllocError);
        }
        let nbits_u32 = u32::try_from(nbits).unwrap();
        // SAFETY: `bindings::BITS_PER_LONG < nbits` and `nbits <= i32::MAX`.
        let ptr = unsafe { bindings::bitmap_zalloc(nbits_u32, flags.as_raw()) };
        let ptr = NonNull::new(ptr).ok_or(AllocError)?;
        // INVARIANT: `ptr` returned by C `bitmap_zalloc` and `nbits` checked.
        return Ok(Bitmap {
            repr: BitmapRepr { ptr },
            nbits,
        });
    }

    /// Returns length of this [`Bitmap`].
    #[inline]
    pub fn len(&self) -> usize {
        self.nbits
    }

    /// Fills this `Bitmap` with random bits.
    #[cfg(CONFIG_FIND_BIT_BENCHMARK_RUST)]
    pub fn fill_random(&mut self) {
        // SAFETY: `self.as_mut_ptr` points to either an array of the
        // appropriate length or one usize.
        unsafe {
            bindings::get_random_bytes(
                self.as_mut_ptr() as *mut ffi::c_void,
                usize::div_ceil(self.nbits, bindings::BITS_PER_LONG as usize)
                    * bindings::BITS_PER_LONG as usize,
            );
        }
    }

    /// Returns a mutable raw pointer to the backing [`Bitmap`].
    #[inline]
    fn as_mut_ptr(&mut self) -> *mut usize {
        if self.nbits <= bindings::BITS_PER_LONG as _ {
            // SAFETY: Bitmap is represented inline.
            unsafe { core::ptr::addr_of_mut!(self.repr.bitmap) }
        } else {
            // SAFETY: Bitmap is represented as array of `unsigned long`.
            unsafe { self.repr.ptr.as_mut() }
        }
    }

    /// Returns a raw pointer to the backing [`Bitmap`].
    #[inline]
    fn as_ptr(&self) -> *const usize {
        if self.nbits <= bindings::BITS_PER_LONG as _ {
            // SAFETY: Bitmap is represented inline.
            unsafe { core::ptr::addr_of!(self.repr.bitmap) }
        } else {
            // SAFETY: Bitmap is represented as array of `unsigned long`.
            unsafe { self.repr.ptr.as_ptr() }
        }
    }

    /// Set bit with index `index`.
    ///
    /// ATTENTION: `set_bit` is non-atomic, which differs from the naming
    /// convention in C code. The corresponding C function is `__set_bit`.
    ///
    /// # Panics
    ///
    /// Panics if `index` is greater than or equal to `self.nbits`.
    #[inline]
    pub fn set_bit(&mut self, index: usize) {
        assert!(
            index < self.nbits,
            "Bit `index` must be < {}, was {}",
            self.nbits,
            index
        );
        // SAFETY: Bit `index` is within bounds.
        unsafe { bindings::__set_bit(index as u32, self.as_mut_ptr()) };
    }

    /// Set bit with index `index`, atomically.
    ///
    /// ATTENTION: The naming convention differs from C, where the corresponding
    /// function is called `set_bit`.
    ///
    /// # Safety
    ///
    /// This is a relaxed atomic operation (no implied memory barriers, no
    /// ordering guarantees). The caller must ensure that this is safe, as
    /// the compiler cannot prevent code with an exclusive reference from
    /// calling atomic operations.
    ///
    /// # Panics
    ///
    /// Panics if `index` is greater than or equal to `self.nbits`.
    #[inline]
    pub unsafe fn set_bit_atomic(&self, index: usize) {
        assert!(
            index < self.nbits,
            "Bit `index` must be < {}, was {}",
            self.nbits,
            index
        );
        // SAFETY: `index` is within bounds and the caller has ensured that
        // there is no mix of non-atomic and atomic operations.
        unsafe { bindings::set_bit(index as u32, self.as_ptr() as *mut usize) };
    }

    /// Clear `index` bit.
    ///
    /// ATTENTION: `clear_bit` is non-atomic, which differs from the naming
    /// convention in C code. The corresponding C function is `__clear_bit`.
    /// # Panics
    ///
    /// Panics if `index` is greater than or equal to `self.nbits`.
    #[inline]
    pub fn clear_bit(&mut self, index: usize) {
        assert!(
            index < self.nbits,
            "Bit `index` must be < {}, was {}",
            self.nbits,
            index
        );
        // SAFETY: `index` is within bounds.
        unsafe { bindings::__clear_bit(index as u32, self.as_mut_ptr()) };
    }

    /// Clear `index` bit, atomically.
    ///
    /// ATTENTION: The naming convention differs from C, where the corresponding
    /// function is called `clear_bit`.
    ///
    /// # Safety
    ///
    /// This is a relaxed atomic operation (no implied memory barriers, no
    /// ordering guarantees). The caller must ensure that this is safe, as
    /// the compiler cannot prevent code with an exclusive reference from
    /// calling atomic operations.
    ///
    /// # Panics
    ///
    /// Panics if `index` is greater than or equal to `self.nbits`.
    #[inline]
    pub unsafe fn clear_bit_atomic(&self, index: usize) {
        assert!(
            index < self.nbits,
            "Bit `index` must be < {}, was {}",
            self.nbits,
            index
        );
        // SAFETY: `index` is within bounds and the caller has ensured that
        // there is no mix of non-atomic and atomic operations.
        unsafe { bindings::clear_bit(index as u32, self.as_ptr() as *mut usize) };
    }

    /// Copy `src` into this [`Bitmap`] and set any remaining bits to zero.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
    /// use kernel::bitmap::Bitmap;
    ///
    /// let mut long_bitmap = Bitmap::new(256, GFP_KERNEL)?;
    //
    /// assert_eq!(None, long_bitmap.last_bit());
    //
    /// let mut short_bitmap = Bitmap::new(16, GFP_KERNEL)?;
    //
    /// short_bitmap.set_bit(7);
    /// long_bitmap.copy_and_extend(&short_bitmap);
    /// assert_eq!(Some(7), long_bitmap.last_bit());
    ///
    /// # Ok::<(), AllocError>(())
    /// ```
    #[inline]
    pub fn copy_and_extend(&mut self, src: &Bitmap) {
        let len = core::cmp::min(src.nbits, self.nbits);
        // SAFETY: access to `self` and `src` is within bounds.
        unsafe {
            bindings::bitmap_copy_and_extend(
                self.as_mut_ptr(),
                src.as_ptr(),
                len as u32,
                self.nbits as u32,
            )
        };
    }

    /// Finds last set bit.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
    /// use kernel::bitmap::Bitmap;
    ///
    /// let bitmap = Bitmap::new(64, GFP_KERNEL)?;
    ///
    /// match bitmap.last_bit() {
    ///     Some(idx) => {
    ///         pr_info!("The last bit has index {idx}.\n");
    ///     }
    ///     None => {
    ///         pr_info!("All bits in this bitmap are 0.\n");
    ///     }
    /// }
    /// # Ok::<(), AllocError>(())
    /// ```
    #[inline]
    pub fn last_bit(&self) -> Option<usize> {
        // SAFETY: `_find_next_bit` access is within bounds due to invariant.
        let index = unsafe { bindings::_find_last_bit(self.as_ptr(), self.nbits) };
        if index >= self.nbits {
            None
        } else {
            Some(index)
        }
    }

    /// Finds next set bit, starting from `start`.
    /// Returns `None` if `start` is greater of equal than `self.nbits`.
    #[inline]
    pub fn next_bit(&self, start: usize) -> Option<usize> {
        bitmap_hardening_assert!(start < self.nbits, "`start` must be < {} was {}", self.nbits, start);
        // SAFETY: `_find_next_bit` tolerates out-of-bounds arguments and returns a
        // value larger than or equal to `self.nbits` in that case.
        let index = unsafe { bindings::_find_next_bit(self.as_ptr(), self.nbits, start) };
        if index >= self.nbits {
            None
        } else {
            Some(index)
        }
    }

    /// Finds next zero bit, starting from `start`.
    /// Returns `None` if `start` is greater than or equal to `self.nbits`.
    #[inline]
    pub fn next_zero_bit(&self, start: usize) -> Option<usize> {
        bitmap_hardening_assert!(start < self.nbits, "`start` must be < {} was {}", self.nbits, start);
        // SAFETY: `_find_next_zero_bit` tolerates out-of-bounds arguments and returns a
        // value larger than or equal to `self.nbits` in that case.
        let index = unsafe { bindings::_find_next_zero_bit(self.as_ptr(), self.nbits, start) };
        if index >= self.nbits {
            None
        } else {
            Some(index)
        }
    }
}

use macros::kunit_tests;

#[kunit_tests(rust_kernel_bitmap)]
mod tests {
    use super::*;
    use kernel::alloc::flags::GFP_KERNEL;

    #[test]
    fn bitmap_new() {
        let b = Bitmap::new(0, GFP_KERNEL).unwrap();
        assert_eq!(0, b.len());

        let b = Bitmap::new(3, GFP_KERNEL).unwrap();
        assert_eq!(3, b.len());

        let b = Bitmap::new(1024, GFP_KERNEL).unwrap();
        assert_eq!(1024, b.len());

        // Requesting too large values results in [`AllocError`].
        let b = Bitmap::new(1 << 31, GFP_KERNEL);
        assert!(b.is_err());
    }

    #[test]
    fn bitmap_set_clear_find() {
        let mut b = Bitmap::new(128, GFP_KERNEL).unwrap();

        // Zero-initialized
        assert_eq!(None, b.last_bit());

        b.set_bit(17);

        assert_eq!(Some(17), b.next_bit(0));
        assert_eq!(Some(17), b.last_bit());

        b.set_bit(107);

        assert_eq!(Some(17), b.next_bit(0));
        assert_eq!(Some(17), b.next_bit(17));
        assert_eq!(Some(107), b.next_bit(18));
        assert_eq!(Some(107), b.last_bit());

        b.clear_bit(17);

        assert_eq!(Some(107), b.next_bit(0));
        assert_eq!(Some(107), b.last_bit());
    }

    #[test]
    fn bitmap_out_of_bounds() {
        let mut b = Bitmap::new(128, GFP_KERNEL).unwrap();

        assert_eq!(None, b.next_bit(2048));
        assert_eq!(None, b.next_zero_bit(2048));
        assert_eq!(None, b.last_bit(2048));
    }

    #[test]
    fn bitmap_copy_and_extend() {
        let mut long_bitmap = Bitmap::new(256, GFP_KERNEL).unwrap();

        long_bitmap.set_bit(3);
        long_bitmap.set_bit(200);

        let mut short_bitmap = Bitmap::new(32, GFP_KERNEL).unwrap();

        short_bitmap.set_bit(17);

        long_bitmap.copy_and_extend(&short_bitmap);
        // The larger bits have been cleared.
        assert_eq!(Some(17), long_bitmap.next_bit(0));
        assert_eq!(Some(17), long_bitmap.last_bit());
    }
}
