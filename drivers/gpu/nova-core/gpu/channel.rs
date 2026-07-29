// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

//! Channel ID allocation.

use core::{
    num::NonZero,
    ops::{
        Deref,
        Range, //
    }, //
};

use kernel::{
    id_pool::IdPool,
    prelude::*,
    ptr::Alignment,
    sync::{
        new_mutex,
        Mutex, //
    }, //
};

/// Pool for tracking reservations of channel IDs.
#[pin_data]
pub(crate) struct ChannelIdPool {
    #[pin]
    inner: Mutex<IdPool>,
    num_chids: usize,
}

impl ChannelIdPool {
    /// Creates a pool managing `num_chids` channel IDs.
    pub(crate) fn new(num_chids: usize) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            inner <- new_mutex!(IdPool::with_capacity(num_chids, GFP_KERNEL)?),
            num_chids,
        })
    }

    /// Reserves a contiguous area of `count` channel IDs starting at a multiple of `align`,
    /// returning a guard that releases the area on drop.
    pub(crate) fn alloc_area(
        &self,
        count: NonZero<usize>,
        align: Alignment,
    ) -> Result<ChannelIdArea<'_>> {
        let mut ids = self.inner.lock();
        let area = ids.find_unused_area(0, count, align).ok_or(ENOSPC)?;

        // If the pool is small, the backing bitmap may be rounded up to a larger size.
        if area.range().end > self.num_chids {
            return Err(ENOSPC);
        }
        Ok(ChannelIdArea {
            pool: self,
            range: area.acquire(),
        })
    }
}

/// A reserved contiguous area of channel IDs.
///
/// Releases the whole area back to its [`ChannelIdPool`] when dropped. Releasing locks a
/// sleeping [`Mutex`], so the area must be dropped in a context that is allowed to sleep.
#[must_use = "the channel ID area is released immediately when unused"]
pub(crate) struct ChannelIdArea<'a> {
    pool: &'a ChannelIdPool,
    range: Range<usize>,
}

impl Drop for ChannelIdArea<'_> {
    fn drop(&mut self) {
        self.pool.inner.lock().release_area(&self.range);
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

    const fn nz<const N: usize>() -> NonZero<usize> {
        const { NonZero::new(N).unwrap() }
    }

    #[test]
    fn chid_area() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(2048), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();

        let first = pool.alloc_area(nz::<48>(), unaligned)?;
        assert_eq!(0, first.start);
        assert_eq!(48, first.len());
        assert_eq!(48, first.end);

        let second = pool.alloc_area(nz::<48>(), unaligned)?;
        assert!(first.end <= second.start || second.end <= first.start);

        let first_start = first.start;
        drop(first);
        assert_eq!(first_start, pool.alloc_area(nz::<48>(), unaligned)?.start);
        Ok(())
    }

    #[test]
    fn chid_bounded_by_num_chids() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(4), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();

        {
            let a = pool.alloc_area(nz::<1>(), unaligned)?;
            let b = pool.alloc_area(nz::<1>(), unaligned)?;
            let c = pool.alloc_area(nz::<1>(), unaligned)?;
            let d = pool.alloc_area(nz::<1>(), unaligned)?;
            assert_eq!(0, a.start);
            assert_eq!(1, b.start);
            assert_eq!(2, c.start);
            assert_eq!(3, d.start);
            assert_eq!(
                Err(ENOSPC),
                pool.alloc_area(nz::<1>(), unaligned).map(|_| ())
            );
        }

        assert_eq!(0, pool.alloc_area(nz::<4>(), unaligned)?.start);
        assert_eq!(
            Err(ENOSPC),
            pool.alloc_area(nz::<5>(), unaligned).map(|_| ())
        );

        let head = pool.alloc_area(nz::<3>(), unaligned)?;
        assert_eq!(0, head.start);
        assert_eq!(
            Err(ENOSPC),
            pool.alloc_area(nz::<2>(), unaligned).map(|_| ())
        );
        assert_eq!(3, pool.alloc_area(nz::<1>(), unaligned)?.start);
        Ok(())
    }

    #[test]
    fn chid_area_aligned() -> Result {
        let pool = KBox::pin_init(ChannelIdPool::new(16), GFP_KERNEL)?;
        let unaligned = Alignment::new::<1>();
        let align4 = Alignment::new::<4>();

        // Alloc 0 so the first fit for the next area is unaligned.
        let pad = pool.alloc_area(nz::<1>(), unaligned)?;
        assert_eq!(0, pad.start);

        let a = pool.alloc_area(nz::<4>(), align4)?;
        assert_eq!(4, a.start);

        // The area skipped over by the aligned allocation should still be available.
        let b = pool.alloc_area(nz::<1>(), unaligned)?;
        assert_eq!(1, b.start);

        let c = pool.alloc_area(nz::<8>(), Alignment::new::<8>())?;
        assert_eq!(8, c.start);

        // Only 2 IDs left.
        assert_eq!(Err(ENOSPC), pool.alloc_area(nz::<4>(), align4).map(|_| ()));
        assert_eq!(
            Err(ENOSPC),
            pool.alloc_area(nz::<1>(), Alignment::new::<32>())
                .map(|_| ())
        );

        assert_eq!(2, pool.alloc_area(nz::<2>(), unaligned)?.start);
        Ok(())
    }
}
