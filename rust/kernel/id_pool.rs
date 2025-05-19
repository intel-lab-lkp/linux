// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! Rust API for an ID pool backed by a `Bitmap`.

use crate::alloc::{AllocError, Flags};
use crate::bitmap::Bitmap;

/// Represents a dynamic ID pool backed by a `Bitmap`.
///
/// Clients acquire and release IDs from zero bits in a bitmap.
///
/// The ID pool can grow or shrink as needed. It has been designed
/// to support the scenario where users need to control the time
/// of allocation of a new backing bitmap, which may require release
/// of locks.
/// These operations then, are verified to determine if the grow or
/// shrink is sill valid.
///
/// # Examples
///
/// Basic usage
///
/// ```
/// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
/// use kernel::id_pool::IdPool;
///
/// let mut pool = IdPool::new(64, GFP_KERNEL)?;
/// for i in 0..64 {
///   assert_eq!(i, pool.acquire_next_id(i).ok_or(ENOSPC)?);
/// }
///
/// pool.release_id(23);
/// assert_eq!(23, pool.acquire_next_id(0).ok_or(ENOSPC)?);
///
/// assert_eq!(None, pool.acquire_next_id(0));  // time to realloc.
/// let resizer = pool.grow_alloc().alloc(GFP_KERNEL)?;
/// pool.grow(resizer);
///
/// assert_eq!(pool.acquire_next_id(0), Some(64));
/// # Ok::<(), Error>(())
/// ```
///
/// Releasing spinlock to grow the pool
///
/// ```no_run
/// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
/// use kernel::sync::{new_spinlock, SpinLock};
/// use kernel::id_pool::IdPool;
///
/// fn get_id_maybe_alloc(guarded_pool: &SpinLock<IdPool>) -> Result<usize, AllocError> {
///   let mut pool = guarded_pool.lock();
///   loop {
///     match pool.acquire_next_id(0) {
///       Some(index) => return Ok(index),
///       None => {
///         let alloc_request = pool.grow_alloc();
///         drop(pool);
///         let resizer = alloc_request.alloc(GFP_KERNEL)?;
///         pool = guarded_pool.lock();
///         pool.grow(resizer)
///       }
///     }
///   }
/// }
/// ```
pub struct IdPool {
    map: Bitmap,
}

/// Returned when the `IdPool` should change size.
pub struct AllocRequest {
    nbits: usize,
}

/// Contains an allocated `Bitmap` for resizing `IdPool`.
pub struct PoolResizer {
    new: Bitmap,
}

impl AllocRequest {
    /// Allocates a new `Bitmap` for `IdPool`.
    pub fn alloc(&self, flags: Flags) -> Result<PoolResizer, AllocError> {
        let new = Bitmap::new(self.nbits, flags)?;
        Ok(PoolResizer { new })
    }
}

impl IdPool {
    /// Constructs a new `[IdPool]`.
    #[inline]
    pub fn new(nbits: usize, flags: Flags) -> Result<Self, AllocError> {
        let map = Bitmap::new(nbits, flags)?;
        Ok(Self { map })
    }

    /// Returns how many IDs this pool can currently have.
    #[inline]
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns an [`AllocRequest`] if the [`IdPool`] can be shrunk, [`None`] otherwise.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
    /// use kernel::id_pool::{AllocRequest, IdPool};
    ///
    /// let mut pool = IdPool::new(1024, GFP_KERNEL)?;
    /// let alloc_request = pool.shrink_alloc().ok_or(AllocError)?;
    /// let resizer = alloc_request.alloc(GFP_KERNEL)?;
    /// pool.shrink(resizer);
    /// assert_eq!(pool.len(), kernel::bindings::BITS_PER_LONG as usize);
    /// # Ok::<(), AllocError>(())
    /// ```
    #[inline]
    pub fn shrink_alloc(&self) -> Option<AllocRequest> {
        let len = self.map.len();
        if len <= bindings::BITS_PER_LONG as usize {
            return None;
        }
        /*
         * Determine if the bitmap can shrink based on the position of
         * its last set bit. If the bit is within the first quarter of
         * the bitmap then shrinking is possible. In this case, the
         * bitmap should shrink to half its current size.
         */
        match self.map.last_bit() {
            Some(bit) => {
                if bit < (len >> 2) {
                    Some(AllocRequest { nbits: len >> 1 })
                } else {
                    None
                }
            }
            None => Some(AllocRequest {
                nbits: bindings::BITS_PER_LONG as usize,
            }),
        }
    }

    /// Shrinks pool by using a new `Bitmap`, if still possible.
    #[inline]
    pub fn shrink(&mut self, mut resizer: PoolResizer) {
        // Verify that shrinking is still possible. The `resizer`
        // bitmap might have been allocated without locks, so this call
        // could now be outdated. In this case, drop `resizer` and move on.
        if let Some(AllocRequest { nbits }) = self.shrink_alloc() {
            if nbits <= resizer.new.len() {
                resizer.new.copy_and_extend(&self.map);
                self.map = resizer.new;
                return;
            }
        }
    }

    /// Returns an `AllocRequest` for growing this `IdPool`.
    #[inline]
    pub fn grow_alloc(&self) -> AllocRequest {
        AllocRequest {
            nbits: self.map.len() << 1,
        }
    }

    /// Grows pool by using a new `Bitmap`, if still necessary.
    #[inline]
    pub fn grow(&mut self, mut resizer: PoolResizer) {
        // `resizer` bitmap might have been allocated without locks,
        // so this call could now be outdated. In this case, drop
        // `resizer` and move on.
        if resizer.new.len() <= self.map.len() {
            return;
        }

        resizer.new.copy_and_extend(&self.map);
        self.map = resizer.new;
    }

    /// Acquires a new ID by finding and setting the next zero bit in the
    /// bitmap. Upon success, returns its index. Otherwise, returns `None`
    /// to indicate that a `grow_alloc` is needed.
    #[inline]
    pub fn acquire_next_id(&mut self, offset: usize) -> Option<usize> {
        match self.map.next_zero_bit(offset) {
            res @ Some(nr) => {
                self.map.set_bit(nr);
                res
            }
            None => None,
        }
    }

    /// Releases an ID.
    #[inline]
    pub fn release_id(&mut self, id: usize) {
        self.map.clear_bit(id);
    }
}
