// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

//! Channel related code.

use core::{
    num::NonZero,
    ops::{
        Deref,
        Range, //
    }, //
};

use kernel::{
    maple_tree::MapleTreeAlloc,
    prelude::*,
    ptr::{
        Alignable,
        Alignment, //
    },
    sync::{
        new_mutex,
        Mutex, //
    }, //
};

/// Pool for tracking reservations of channel IDs.
#[pin_data]
pub(crate) struct ChannelIdPool {
    #[pin]
    inner: Mutex<MapleTreeAlloc<()>>,
    num_chids: usize,
}

impl ChannelIdPool {
    /// Creates a pool managing `num_chids` channel IDs.
    pub(crate) fn new(num_chids: usize) -> impl PinInit<Self> {
        pin_init!(Self {
            inner <- new_mutex!(MapleTreeAlloc::new()),
            num_chids,
        })
    }

    /// Reserves a contiguous area of `count` channel IDs starting at a multiple of `align`.
    /// Returns a guard that releases the area on drop.
    pub(crate) fn alloc_area(
        &self,
        count: NonZero<usize>,
        align: Alignment,
    ) -> Result<ChannelIdArea<'_>> {
        // Hold the lock over the whole loop so the release and retry below cannot race with another
        // allocation.
        let tree = self.inner.lock();
        let mut base = 0;
        while base < self.num_chids {
            let start = tree.alloc_range(count.get(), (), base..self.num_chids, GFP_KERNEL)?;
            let aligned = start.align_up(align);

            if aligned == Some(start) {
                return Ok(ChannelIdArea {
                    pool: self,
                    range: start..start + count.get(),
                });
            }

            // Release the unaligned area and retry from the next aligned channel ID.
            tree.erase(start);
            base = aligned.ok_or(EBUSY)?;
        }
        Err(EBUSY)
    }
}

/// A reserved contiguous area of channel IDs.
///
/// Releases the whole area back to its [`ChannelIdPool`] when dropped. Releasing locks a sleeping
/// [`Mutex`] and may allocate memory with `GFP_KERNEL`, so the area must be dropped in a context
/// that is allowed to do so.
#[must_use = "the channel ID area is released immediately when unused"]
pub(crate) struct ChannelIdArea<'a> {
    pool: &'a ChannelIdPool,
    range: Range<usize>,
}

impl Drop for ChannelIdArea<'_> {
    fn drop(&mut self) {
        self.pool.inner.lock().erase(self.range.start);
    }
}

impl Deref for ChannelIdArea<'_> {
    type Target = Range<usize>;

    fn deref(&self) -> &Self::Target {
        &self.range
    }
}

#[kunit_tests(nova_core_channel)]
mod tests {
    use super::*;

    const fn nz(count: usize) -> NonZero<usize> {
        NonZero::new(count).unwrap()
    }

    #[test]
    fn chid_area() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(2048), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();

        let first = pool.alloc_area(nz(48), unaligned)?;
        let second = pool.alloc_area(nz(48), unaligned)?;
        assert_eq!(0, first.start);
        assert_eq!(48, second.start);

        drop(first);
        let third = pool.alloc_area(nz(48), unaligned)?;
        assert_eq!(0, third.start);
        assert_eq!(96, pool.alloc_area(nz(48), unaligned)?.start);
        Ok(())
    }

    #[test]
    fn chid_bounded_by_num_chids() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(4), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();

        assert_eq!(0, pool.alloc_area(nz(4), unaligned)?.start);

        // An area larger than the pool is rejected by the maple tree itself.
        assert_eq!(Some(EINVAL), pool.alloc_area(nz(5), unaligned).err());

        let a = pool.alloc_area(nz(3), unaligned)?;
        assert_eq!(0, a.start);
        assert_eq!(Some(EBUSY), pool.alloc_area(nz(2), unaligned).err());

        let b = pool.alloc_area(nz(1), unaligned)?;
        assert_eq!(3, b.start);
        assert_eq!(Some(EBUSY), pool.alloc_area(nz(1), unaligned).err());
        Ok(())
    }

    #[test]
    fn chid_area_aligned() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(16), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();
        let align4 = Alignment::new::<4>();

        // Alloc 0 so the first fit for the next area is unaligned.
        let pad = pool.alloc_area(nz(1), unaligned)?;
        assert_eq!(0, pad.start);

        let a = pool.alloc_area(nz(4), align4)?;
        assert_eq!(4, a.start);

        // The area skipped over by the aligned allocation should still be available.
        let b = pool.alloc_area(nz(1), unaligned)?;
        assert_eq!(1, b.start);

        let c = pool.alloc_area(nz(8), Alignment::new::<8>())?;
        assert_eq!(8, c.start);

        // Only 2 IDs left.
        assert_eq!(Some(EBUSY), pool.alloc_area(nz(4), align4).err());
        assert_eq!(
            Some(EBUSY),
            pool.alloc_area(nz(1), Alignment::new::<32>()).err()
        );

        assert_eq!(2, pool.alloc_area(nz(2), unaligned)?.start);
        Ok(())
    }
}
