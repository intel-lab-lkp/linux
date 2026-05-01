// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device::{Bound, Device},
    devres::Devres,
    irq, pci,
    prelude::*,
    sync::{
        atomic::{
            Atomic,
            Relaxed, //
        },
        Arc, Completion,
    },
    time,
};

use super::intr_ctrl::{
    IntrCtrl,
    Leaf,
    LeafIndex, //
};
use crate::{
    driver::Bar0,
    gpu::Chipset, //
};

// The following are constant across all architectures.

/// CPU doorbell vector.
const DOORBELL_VECTOR: u32 = 129;

/// Leaf index for the doorbell vector: 129 / 32 = 4.
const DOORBELL_LEAF: usize = 4;

/// Bit within the leaf: 129 % 32 = 1.
const DOORBELL_BIT: u32 = 1 << 1;

/// IRQ handler for the CPU doorbell self-test.
///
/// Performs a minimal interrupt-tree drain cycle:
/// unarm -> read TOP -> iterate leaves -> ack -> rearm.
/// Signals completion and increments the interrupt counter on each handled interrupt.
/// Records the leaf index and pending mask observed by the handler for verification.
#[pin_data]
struct DoorbellTestHandler {
    bar: Arc<Devres<Bar0>>,
    intr_ctrl: IntrCtrl,
    #[pin]
    completion: Completion,
    /// Used to confirm the number of interrupts handled.
    irq_count: Atomic<u32>,
    /// Used to confirm the mask observed on the doorbell leaf (leaf 4).
    doorbell_leaf_mask: Atomic<u32>,
}

impl irq::Handler for DoorbellTestHandler {
    fn handle(&self, dev: &Device<Bound>) -> irq::IrqReturn {
        let Ok(bar) = self.bar.access(dev) else {
            return irq::IrqReturn::None;
        };

        let top = self.intr_ctrl.top().unarm(bar).read_pending(bar);

        if top.mask() == 0 {
            top.rearm(bar);
            return irq::IrqReturn::None;
        }

        // Record the doorbell leaf mask for later verification.
        let doorbell_leaf = Leaf::from_index(LeafIndex::new::<DOORBELL_LEAF>());

        for subtree in top.iter_subtrees() {
            for leaf in subtree.iter_pending_leaves(&self.intr_ctrl, bar) {
                if leaf == doorbell_leaf {
                    self.doorbell_leaf_mask.store(leaf.mask(), Relaxed);
                }
                leaf.ack(bar);
            }
        }

        top.rearm(bar);

        // Increment the interrupt counter and signal the completion.
        self.irq_count.fetch_add(1, Relaxed);
        self.completion.complete_all();

        irq::IrqReturn::Handled
    }
}

/// Run the CPU doorbell IRQ self-test.
///
/// Registers an IRQ handler, triggers CPU doorbell vector, and verifies the
/// interrupt is received through the interrupt tree. This validates the full MSI path:
/// GPU -> PCIe -> CPU -> handler.
pub(crate) fn run_selftest(
    pdev: &pci::Device<Bound>,
    bar_devres: &Arc<Devres<Bar0>>,
    chipset: Chipset,
    irq_vector: pci::IrqVector<'_>,
) -> Result {
    let bar = bar_devres.access(pdev.as_ref())?;
    let intr_ctrl = IntrCtrl::new(chipset);

    // Clear stale pending bits before enabling the doorbell.
    intr_ctrl.drain(bar);

    let handler_init = try_pin_init!(DoorbellTestHandler {
        bar: bar_devres.clone(),
        intr_ctrl,
        completion <- Completion::new(),
        irq_count: Atomic::new(0),
        doorbell_leaf_mask: Atomic::new(0),
    }? Error);

    let reg = Arc::pin_init(
        pdev.request_irq(
            irq_vector,
            irq::Flags::TRIGGER_NONE,
            c"nova-core",
            handler_init,
        ),
        GFP_KERNEL,
    )?;

    let handler = reg.handler();

    // Allow doorbell leaf.
    let doorbell_leaf_idx = LeafIndex::new::<DOORBELL_LEAF>();
    handler
        .intr_ctrl
        .leaf(doorbell_leaf_idx)
        .allow(bar, DOORBELL_BIT);

    // The doorbell bit must be clear before triggering, otherwise the test
    // cannot prove that the IRQ came from the trigger below.
    let pre_mask = handler
        .intr_ctrl
        .leaf(doorbell_leaf_idx)
        .read_pending(bar)
        .mask();
    if pre_mask & DOORBELL_BIT != 0 {
        handler
            .intr_ctrl
            .leaf(doorbell_leaf_idx)
            .block(bar, DOORBELL_BIT);
        let _ = handler.intr_ctrl.top().unarm(bar);
        dev_warn!(
            pdev.as_ref(),
            "CPU doorbell self-test: FAIL (doorbell bit already pending, leaf[{}] mask={:#x})\n",
            DOORBELL_LEAF,
            pre_mask,
        );
        return Err(EIO);
    }

    // Arm the INTR_CTRL top level to enable MSI generation.
    handler.intr_ctrl.top().arm(bar);

    // Trigger the CPU doorbell interrupt.
    handler.intr_ctrl.trigger(bar, DOORBELL_VECTOR);

    // Wait up to 1 second for the interrupt handler to fire.
    let completed = handler
        .completion
        .wait_for_completion_timeout(time::msecs_to_jiffies(1000));

    let count = handler.irq_count.load(Relaxed);
    let leaf_mask = handler.doorbell_leaf_mask.load(Relaxed);

    // Block the doorbell leaf after the test.
    handler
        .intr_ctrl
        .leaf(doorbell_leaf_idx)
        .block(bar, DOORBELL_BIT);
    let _ = handler.intr_ctrl.top().unarm(bar);

    // Verify that the doorbell IRQ fired.
    let doorbell_bit_seen = leaf_mask & DOORBELL_BIT != 0;
    let pass = completed && count == 1 && doorbell_bit_seen;

    if pass {
        dev_info!(
            pdev.as_ref(),
            "CPU doorbell self-test: PASS (irq_count={}, leaf[{}] mask={:#x})\n",
            count,
            DOORBELL_LEAF,
            leaf_mask,
        );
    } else {
        dev_warn!(
            pdev.as_ref(),
            "CPU doorbell self-test: FAIL (completed={}, irq_count={}, leaf[{}] mask={:#x})\n",
            completed,
            count,
            DOORBELL_LEAF,
            leaf_mask,
        );
    }

    Ok(())
}
