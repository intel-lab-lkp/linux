// SPDX-License-Identifier: GPL-2.0

//! GPU interrupt controller support (INTR_CTRL).
//!
//! Each PCIe function (PF and each VF, also known as a GFID) has its own
//! interrupt tree. In this module, we only interact with the PF tree.
//! The VF interacts with its own tree (which appears as a PF tree to it).
//!
//! See `Documentation/gpu/nova/core/intr-ctrl.rst` for detailed documentation
//! of the INTR_CTRL architecture.

use kernel::{
    io::{
        register::Array,
        Io, //
    },
    num::Bounded,
};

use crate::{driver::Bar0, gpu::Chipset, regs};

/// Type alias for a leaf interrupt index, bounded to valid values 0-15.
pub(super) type LeafIndex = Bounded<usize, 4>;

// Type-state for `Top` and `Leaf`.
//
// `Top` follows Idle -> Unarmed -> Pending -> consumed (rearmed).
// `Leaf` uses Idle -> Pending to catch.
//
// This catches issues at compile time where we perform an operation
// on an object in the wrong state (example, rearming `Top` without reading
// pending bits first).
/// Sealed trait representing the interrupt controller state.
pub(super) trait State: private::Sealed {}

/// Idle state: TOP_EN may or may not be armed; no snapshot held.
pub(super) struct Idle;
impl State for Idle {}

/// Unarmed state: TOP_EN was just cleared by this Top handle, snapshot not yet read.
pub(super) struct Unarmed;
impl State for Unarmed {}

/// Pending state: interrupt mask has been read from hardware.
pub(super) struct Pending {
    mask: u32,
}
impl State for Pending {}

mod private {
    pub(in crate::irq) trait Sealed {}
    impl Sealed for super::Idle {}
    impl Sealed for super::Unarmed {}
    impl Sealed for super::Pending {}
}

/// Interrupt controller for a single PCIe function's interrupt tree.
#[derive(Clone)]
pub(super) struct IntrCtrl {
    subtree_mask: u8,
}

impl IntrCtrl {
    /// Create an `IntrCtrl` configured for the given chipset's interrupt tree width.
    pub(super) fn new(chipset: Chipset) -> Self {
        // Each TOP bit covers 2 leaves; subtree_mask has one bit per subtree.
        //   Pre-Hopper:  8 leaves / 2 = 4 subtrees -> 0x0f (bits [3:0])
        //   Hopper+:    16 leaves / 2 = 8 subtrees -> 0xff (bits [7:0])
        Self {
            subtree_mask: if chipset.arch().is_pre_hopper() {
                0xf
            } else {
                0xff
            },
        }
    }

    /// Return a [`Top`] handle in the [`Idle`] state for this controller.
    pub(super) fn top(&self) -> Top<Idle> {
        Top {
            subtree_mask: self.subtree_mask,
            state: Idle,
        }
    }

    /// Return a [`Leaf`] handle in the [`Idle`] state for the given leaf index.
    pub(super) fn leaf(&self, index: LeafIndex) -> Leaf<Idle> {
        Leaf::from_index(index)
    }

    /// Trigger a CPU doorbell interrupt for the given MSI vector number.
    pub(super) fn trigger(&self, bar: &Bar0, vector: u32) {
        bar.write(regs::NV_VF_INTR_LEAF_TRIGGER, vector.into());
    }

    /// Drain any pending interrupts on this controller.
    ///
    /// Walks all enabled subtrees, reads each leaf's pending mask, and acks
    /// any pending bits. Useful for clearing stale interrupt state, e.g.,
    /// state leftover when GSP booted.
    pub(super) fn drain(&self, bar: &Bar0) {
        let top = self.top().unarm(bar).read_pending(bar);

        for subtree in top.iter_subtrees() {
            for leaf in subtree.iter_pending_leaves(self, bar) {
                leaf.ack(bar);
            }
        }

        top.rearm(bar);
    }
}

/// Top-level interrupt controller view.
pub(super) struct Top<S: State = Idle> {
    subtree_mask: u8,
    state: S,
}

impl Top<Idle> {
    /// Arm the controller (write TOP_EN_SET). Use for one-shot initial
    /// setup before any interrupts are expected. The ISR's normal
    /// re-arm path goes through `unarm()` -> `read_pending()` ->
    /// `Top<Pending>::rearm()` instead.
    pub(super) fn arm(self, bar: &Bar0) {
        bar.write(
            regs::NV_VF_INTR_TOP_EN_SET,
            u32::from(self.subtree_mask).into(),
        );
    }

    /// Unarm the controller (write TOP_EN_CLEAR). MSI is edge-triggered,
    /// so this stops the GPU from firing redundant MSI writes over PCIe
    /// while the host drains the tree. Consumes self and transitions to
    /// `Top<Unarmed>`, which can then `read_pending()`.
    pub(super) fn unarm(self, bar: &Bar0) -> Top<Unarmed> {
        bar.write(
            regs::NV_VF_INTR_TOP_EN_CLEAR,
            u32::from(self.subtree_mask).into(),
        );
        Top {
            subtree_mask: self.subtree_mask,
            state: Unarmed,
        }
    }
}

impl Top<Unarmed> {
    /// Read the TOP register's pending bitmask. Consumes self and
    /// returns a `Top<Pending>` carrying the snapshot.
    pub(super) fn read_pending(self, bar: &Bar0) -> Top<Pending> {
        let mask = bar.read(regs::NV_VF_INTR_TOP).into_raw();
        Top {
            subtree_mask: self.subtree_mask,
            state: Pending { mask },
        }
    }
}

/// One subtree in the INTR_TOP pending mask (covers two adjacent leaf indices).
#[derive(Clone, Copy)]
pub(super) struct Subtree {
    index: usize,
}

impl Subtree {
    /// Yields the two [`Leaf`] slots covered by this subtree's TOP bit.
    fn iter_leaves<'a>(
        self,
        ctrl: &'a IntrCtrl,
    ) -> impl Iterator<Item = Leaf<Idle>> + 'a {
        // Each subtree covers two adjacent leaf indices for all architectures.
        (0..2usize).filter_map(move |offset| {
            // self.index is 0-31 and offset is 0-1, so idx is at most 63.
            let idx = self.index * 2 + offset;
            LeafIndex::try_new(idx).map(|idx| ctrl.leaf(idx))
        })
    }

    /// Like [`Self::iter_leaves`], but keeps only leaves with a non-zero pending mask.
    pub(super) fn iter_pending_leaves<'a>(
        self,
        ctrl: &'a IntrCtrl,
        bar: &'a Bar0,
    ) -> impl Iterator<Item = Leaf<Pending>> + 'a {
        self.iter_leaves(ctrl).filter_map(move |idle| {
            let pending = idle.read_pending(bar);
            (pending.mask() != 0).then_some(pending)
        })
    }
}

impl Top<Pending> {
    /// Return the raw TOP pending bitmask snapshot.
    pub(super) fn mask(&self) -> u32 {
        self.state.mask
    }

    /// Iterate over all subtrees with a pending TOP bit set in the snapshot.
    pub(super) fn iter_subtrees(&self) -> impl Iterator<Item = Subtree> + '_ {
        (0..32usize)
            .filter(move |&bit| self.state.mask & (1u32 << bit) != 0)
            .map(|index| Subtree { index })
    }

    /// Re-arm the controller (write TOP_EN_SET). Consumes self so the
    /// pending snapshot cannot be consulted or re-iterated afterwards.
    pub(super) fn rearm(self, bar: &Bar0) {
        bar.write(
            regs::NV_VF_INTR_TOP_EN_SET,
            u32::from(self.subtree_mask).into(),
        );
    }
}

/// Leaf interrupt controller view for one interrupt leaf.
pub(super) struct Leaf<S: State = Idle> {
    index: LeafIndex,
    state: S,
}

impl<Left: State, Right: State> PartialEq<Leaf<Right>> for Leaf<Left> {
    fn eq(&self, other: &Leaf<Right>) -> bool {
        self.index == other.index
    }
}

impl<S: State> Eq for Leaf<S> {}

// All `try_at().unwrap()` calls below are safe: `LeafIndex` is `Bounded<usize, 4>`,
// guaranteeing values 0-15, and all INTR_CTRL leaf register arrays have 16 elements.
impl Leaf<Idle> {
    /// Construct a [`Leaf`] handle for the given leaf index.
    pub(super) fn from_index(index: LeafIndex) -> Self {
        Leaf { index, state: Idle }
    }

    /// Enable the bits in `mask` in this leaf's EN_SET register.
    pub(super) fn allow(&self, bar: &Bar0, mask: u32) {
        bar.write(
            regs::NV_VF_INTR_LEAF_EN_SET::try_at(self.index.get()).unwrap(),
            mask.into(),
        );
    }

    /// Disable the bits in `mask` in this leaf's EN_CLEAR register.
    pub(super) fn block(&self, bar: &Bar0, mask: u32) {
        bar.write(
            regs::NV_VF_INTR_LEAF_EN_CLEAR::try_at(self.index.get()).unwrap(),
            mask.into(),
        );
    }

    /// Read this leaf's pending interrupt mask and transition to [`Pending`].
    pub(super) fn read_pending(self, bar: &Bar0) -> Leaf<Pending> {
        let mask = bar
            .read(regs::NV_VF_INTR_LEAF::try_at(self.index.get()).unwrap())
            .into_raw();
        Leaf {
            index: self.index,
            state: Pending { mask },
        }
    }
}

impl Leaf<Pending> {
    /// Return the raw pending interrupt bitmask read from hardware.
    pub(super) fn mask(&self) -> u32 {
        self.state.mask
    }

    /// Acknowledge all pending bits by writing the mask back to the leaf register.
    pub(super) fn ack(&self, bar: &Bar0) {
        if self.state.mask != 0 {
            bar.write(
                regs::NV_VF_INTR_LEAF::try_at(self.index.get()).unwrap(),
                self.state.mask.into(),
            );
        }
    }
}
