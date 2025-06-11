// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! Rust API for an ID pool backed by a [`Bitmap`].

use crate::alloc::{AllocError, Flags};
use crate::bitmap::Bitmap;

/// Represents a dynamic ID pool backed by a [`Bitmap`].
///
/// Clients acquire and release IDs from unset bits in a bitmap.
///
/// The capacity of the ID pool may be adjusted by users as
/// needed. The API supports the scenario where users need precise control
/// over the time of allocation of a new backing bitmap, which may require
/// release of spinlock.
/// Due to concurrent updates, all operations are re-verified to determine
/// if the grow or shrink is sill valid.
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
/// let resizer = pool.grow_request().ok_or(ENOSPC)?.realloc(GFP_KERNEL)?;
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
/// fn get_id_maybe_realloc(guarded_pool: &SpinLock<IdPool>) -> Result<usize, AllocError> {
///   let mut pool = guarded_pool.lock();
///   loop {
///     match pool.acquire_next_id(0) {
///       Some(index) => return Ok(index),
///       None => {
///         let alloc_request = pool.grow_request();
///         drop(pool);
///         let resizer = alloc_request.ok_or(AllocError)?.realloc(GFP_KERNEL)?;
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

/// Indicates that an [`IdPool`] should change to a new target size.
pub struct ReallocRequest {
    num_ids: usize,
}

/// Contains a [`Bitmap`] of a size suitable for reallocating [`IdPool`].
pub struct PoolResizer {
    new: Bitmap,
}

impl ReallocRequest {
    /// Allocates a new backing [`Bitmap`] for [`IdPool`].
    ///
    /// This method only prepares reallocation and does not complete it.
    /// Reallocation will complete after passing the [`PoolResizer`] to the
    /// [`IdPool::grow`] or [`IdPool::shrink`] operation, which will check
    /// that reallocation still makes sense.
    pub fn realloc(&self, flags: Flags) -> Result<PoolResizer, AllocError> {
        let new = Bitmap::new(self.num_ids, flags)?;
        Ok(PoolResizer { new })
    }
}

impl IdPool {
    /// Constructs a new [`IdPool`].
    ///
    /// [BITS_PER_LONG]: srctree/include/asm-generic/bitsperlong.h
    /// A capacity below [`BITS_PER_LONG`][BITS_PER_LONG] is adjusted to
    /// [`BITS_PER_LONG`][BITS_PER_LONG].
    #[inline]
    pub fn new(num_ids: usize, flags: Flags) -> Result<Self, AllocError> {
        let num_ids = core::cmp::max(num_ids, bindings::BITS_PER_LONG as usize);
        let map = Bitmap::new(num_ids, flags)?;
        Ok(Self { map })
    }

    /// Returns how many IDs this pool can currently have.
    #[expect(clippy::len_without_is_empty)]
    #[inline]
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// Returns a [`ReallocRequest`] if the [`IdPool`] can be shrunk, [`None`] otherwise.
    ///
    /// [BITS_PER_LONG]: srctree/include/asm-generic/bitsperlong.h
    /// The capacity of an [`IdPool`] cannot be shrunk below [`BITS_PER_LONG`][BITS_PER_LONG].
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::{AllocError, flags::GFP_KERNEL};
    /// use kernel::id_pool::{ReallocRequest, IdPool};
    ///
    /// let mut pool = IdPool::new(1024, GFP_KERNEL)?;
    /// let alloc_request = pool.shrink_request().ok_or(AllocError)?;
    /// let resizer = alloc_request.realloc(GFP_KERNEL)?;
    /// pool.shrink(resizer);
    /// assert_eq!(pool.len(), kernel::bindings::BITS_PER_LONG as usize);
    /// # Ok::<(), AllocError>(())
    /// ```
    #[inline]
    pub fn shrink_request(&self) -> Option<ReallocRequest> {
        let len = self.map.len();
        // Shrinking below [`BITS_PER_LONG`] is never possible.
        if len <= bindings::BITS_PER_LONG as usize {
            return None;
        }
        // Determine if the bitmap can shrink based on the position of
        // its last set bit. If the bit is within the first quarter of
        // the bitmap then shrinking is possible. In this case, the
        // bitmap should shrink to half its current size.
        let Some(bit) = self.map.last_bit() else {
            return Some(ReallocRequest {
                num_ids: bindings::BITS_PER_LONG as usize,
            });
        };
        if bit >= (len / 4) {
            return None;
        }
        let num_ids = core::cmp::max(bindings::BITS_PER_LONG as usize, len / 2);
        Some(ReallocRequest { num_ids })
    }

    /// Shrinks pool by using a new [`Bitmap`], if still possible.
    #[inline]
    pub fn shrink(&mut self, mut resizer: PoolResizer) {
        // Between request to shrink that led to allocation of `resizer` and now,
        // bits may have changed.
        // Verify that shrinking is still possible. In case shrinking to
        // the size of `resizer` is no longer possible, do nothing,
        // drop `resizer` and move on.
        let Some(updated) = self.shrink_request() else {
            return;
        };
        if updated.num_ids > resizer.new.len() {
            return;
        }

        resizer.new.copy_and_extend(&self.map);
        self.map = resizer.new;
    }

    /// Returns a [`ReallocRequest`] for growing this [`IdPool`], if possible.
    ///
    /// The capacity of an [`IdPool`] cannot be grown above [`i32::MAX`].
    #[inline]
    pub fn grow_request(&self) -> Option<ReallocRequest> {
        let num_ids = self.map.len() * 2;
        if num_ids > i32::MAX.try_into().unwrap() {
            return None;
        }
        Some(ReallocRequest { num_ids })
    }

    /// Grows pool by using a new [`Bitmap`], if still necessary.
    ///
    /// The `resizer` arguments has to be obtained by calling [`Self::grow_request`]
    /// on this object and performing a [`ReallocRequest::realloc`].
    #[inline]
    pub fn grow(&mut self, mut resizer: PoolResizer) {
        // Between request to grow that led to allocation of `resizer` and now,
        // another thread may have already grown the capacity.
        // In this case, do nothing, drop `resizer` and move on.
        if resizer.new.len() <= self.map.len() {
            return;
        }

        resizer.new.copy_and_extend(&self.map);
        self.map = resizer.new;
    }

    /// Acquires a new ID by finding and setting the next zero bit in the
    /// bitmap.
    ///
    /// Upon success, returns its index. Otherwise, returns [`None`]
    /// to indicate that a [`Self::grow_request`] is needed.
    #[inline]
    pub fn acquire_next_id(&mut self, offset: usize) -> Option<usize> {
        let next_zero_bit = self.map.next_zero_bit(offset);
        if let Some(nr) = next_zero_bit {
            self.map.set_bit(nr);
        }
        next_zero_bit
    }

    /// Releases an ID.
    #[inline]
    pub fn release_id(&mut self, id: usize) {
        self.map.clear_bit(id);
    }
}
