// SPDX-License-Identifier: GPL-2.0

//! This module provides the `TagSet` struct to wrap the C `struct blk_mq_tag_set`.
//!
//! C header: [`include/linux/blk-mq.h`](srctree/include/linux/blk-mq.h)

use core::pin::Pin;

use crate::{
    bindings,
    block::mq::{operations::OperationsVTable, request::RequestDataWrapper, Operations},
    error::{self, Result},
    prelude::try_pin_init,
    types::Opaque,
};
use core::{convert::TryInto, marker::PhantomData};
use pin_init::{pin_data, pinned_drop, PinInit};

/// Flags that control blk-mq tag set behavior.
///
/// They can be combined with the operators `|`, `&`, and `!`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct TagSetFlags(u32);

impl TagSetFlags {
    /// Returns an empty instance where no flags are set.
    pub const fn empty() -> Self {
        Self(0)
    }

    /// Register as a blocking blk-mq driver device.
    pub const BLOCKING: Self = Self::new(bindings::BLK_MQ_F_BLOCKING as u32);

    /// Use an underlying blk-mq device for completing I/O.
    pub const STACKING: Self = Self::new(bindings::BLK_MQ_F_STACKING as u32);

    /// Share hardware contexts between tags.
    pub const TAG_HCTX_SHARED: Self = Self::new(bindings::BLK_MQ_F_TAG_HCTX_SHARED as u32);

    /// Allocate tags on a round-robin basis.
    pub const TAG_RR: Self = Self::new(bindings::BLK_MQ_F_TAG_RR as u32);

    /// Disable the I/O scheduler by default.
    pub const NO_SCHED_BY_DEFAULT: Self =
        Self::new(bindings::BLK_MQ_F_NO_SCHED_BY_DEFAULT as u32);

    /// Check whether `flags` is contained in `self`.
    pub fn contains(self, flags: Self) -> bool {
        (self & flags) == flags
    }

    pub(crate) const fn as_raw(self) -> u32 {
        self.0
    }

    const fn all_bits() -> u32 {
        Self::BLOCKING.0
            | Self::STACKING.0
            | Self::TAG_HCTX_SHARED.0
            | Self::TAG_RR.0
            | Self::NO_SCHED_BY_DEFAULT.0
    }

    const fn new(value: u32) -> Self {
        Self(value)
    }
}

impl core::ops::BitOr for TagSetFlags {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for TagSetFlags {
    type Output = Self;

    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for TagSetFlags {
    type Output = Self;

    fn not(self) -> Self::Output {
        Self(!self.0 & Self::all_bits())
    }
}

/// A wrapper for the C `struct blk_mq_tag_set`.
///
/// `struct blk_mq_tag_set` contains a `struct list_head` and so must be pinned.
///
/// # Invariants
///
/// - `inner` is initialized and valid.
#[pin_data(PinnedDrop)]
#[repr(transparent)]
pub struct TagSet<T: Operations> {
    #[pin]
    inner: Opaque<bindings::blk_mq_tag_set>,
    _p: PhantomData<T>,
}

impl<T: Operations> TagSet<T> {
    /// Try to create a new tag set
    pub fn new(
        nr_hw_queues: u32,
        num_tags: u32,
        num_maps: u32,
    ) -> impl PinInit<Self, error::Error> {
        Self::new_with_flags(nr_hw_queues, num_tags, num_maps, TagSetFlags::empty())
    }

    /// Try to create a new tag set with the given blk-mq flags.
    pub fn new_with_flags(
        nr_hw_queues: u32,
        num_tags: u32,
        num_maps: u32,
        flags: TagSetFlags,
    ) -> impl PinInit<Self, error::Error> {
        let tag_set: bindings::blk_mq_tag_set = pin_init::zeroed();
        let tag_set: Result<_> = core::mem::size_of::<RequestDataWrapper>()
            .try_into()
            .map(|cmd_size| {
                bindings::blk_mq_tag_set {
                    ops: OperationsVTable::<T>::build(),
                    nr_hw_queues,
                    timeout: 0, // 0 means default which is 30Hz in C
                    numa_node: bindings::NUMA_NO_NODE,
                    queue_depth: num_tags,
                    cmd_size,
                    flags: flags.as_raw(),
                    driver_data: core::ptr::null_mut::<crate::ffi::c_void>(),
                    nr_maps: num_maps,
                    ..tag_set
                }
            })
            .map(Opaque::new)
            .map_err(|e| e.into());

        try_pin_init!(TagSet {
            inner <- tag_set.pin_chain(|tag_set| {
                // SAFETY: we do not move out of `tag_set`.
                let tag_set: &mut Opaque<_> = unsafe { Pin::get_unchecked_mut(tag_set) };
                // SAFETY: `tag_set` is a reference to an initialized `blk_mq_tag_set`.
                error::to_result( unsafe { bindings::blk_mq_alloc_tag_set(tag_set.get())})
            }),
            _p: PhantomData,
        })
    }

    /// Return the pointer to the wrapped `struct blk_mq_tag_set`
    pub(crate) fn raw_tag_set(&self) -> *mut bindings::blk_mq_tag_set {
        self.inner.get()
    }
}

#[pinned_drop]
impl<T: Operations> PinnedDrop for TagSet<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: By type invariant `inner` is valid and has been properly
        // initialized during construction.
        unsafe { bindings::blk_mq_free_tag_set(self.inner.get()) };
    }
}
