// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright 2025 Collabora ltd.

use crate::bindings;

/// Flags to be used when registering IRQ handlers.
///
/// They can be combined with the operators `|`, `&`, and `!`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Flags(u64);

impl Flags {
    pub(crate) fn into_inner(self) -> u64 {
        self.0
    }
}

impl core::ops::BitOr for Flags {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Flags {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self::Output {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::Not for Flags {
    type Output = Self;
    fn not(self) -> Self::Output {
        Self(!self.0)
    }
}

/// Use the interrupt line as already configured.
pub const TRIGGER_NONE: Flags = Flags(bindings::IRQF_TRIGGER_NONE as u64);

/// The interrupt is triggered when the signal goes from low to high.
pub const TRIGGER_RISING: Flags = Flags(bindings::IRQF_TRIGGER_RISING as u64);

/// The interrupt is triggered when the signal goes from high to low.
pub const TRIGGER_FALLING: Flags = Flags(bindings::IRQF_TRIGGER_FALLING as u64);

/// The interrupt is triggered while the signal is held high.
pub const TRIGGER_HIGH: Flags = Flags(bindings::IRQF_TRIGGER_HIGH as u64);

/// The interrupt is triggered while the signal is held low.
pub const TRIGGER_LOW: Flags = Flags(bindings::IRQF_TRIGGER_LOW as u64);

/// Allow sharing the irq among several devices.
pub const SHARED: Flags = Flags(bindings::IRQF_SHARED as u64);

/// Set by callers when they expect sharing mismatches to occur.
pub const PROBE_SHARED: Flags = Flags(bindings::IRQF_PROBE_SHARED as u64);

/// Flag to mark this interrupt as timer interrupt.
pub const TIMER: Flags = Flags(bindings::IRQF_TIMER as u64);

/// Interrupt is per cpu.
pub const PERCPU: Flags = Flags(bindings::IRQF_PERCPU as u64);

/// Flag to exclude this interrupt from irq balancing.
pub const NOBALANCING: Flags = Flags(bindings::IRQF_NOBALANCING as u64);

/// Interrupt is used for polling (only the interrupt that is registered
/// first in a shared interrupt is considered for performance reasons).
pub const IRQPOLL: Flags = Flags(bindings::IRQF_IRQPOLL as u64);

/// Interrupt is not reenabled after the hardirq handler finished. Used by
/// threaded interrupts which need to keep the irq line disabled until the
/// threaded handler has been run.
pub const ONESHOT: Flags = Flags(bindings::IRQF_ONESHOT as u64);

/// Do not disable this IRQ during suspend. Does not guarantee that this
/// interrupt will wake the system from a suspended state.
pub const NO_SUSPEND: Flags = Flags(bindings::IRQF_NO_SUSPEND as u64);

/// Force enable it on resume even if [`NO_SUSPEND`] is set.
pub const FORCE_RESUME: Flags = Flags(bindings::IRQF_FORCE_RESUME as u64);

/// Interrupt cannot be threaded.
pub const NO_THREAD: Flags = Flags(bindings::IRQF_NO_THREAD as u64);

/// Resume IRQ early during syscore instead of at device resume time.
pub const EARLY_RESUME: Flags = Flags(bindings::IRQF_EARLY_RESUME as u64);

/// If the IRQ is shared with a [`NO_SUSPEND`] user, execute this interrupt
/// handler after suspending interrupts. For system wakeup devices users
/// need to implement wakeup detection in their interrupt handlers.
pub const COND_SUSPEND: Flags = Flags(bindings::IRQF_COND_SUSPEND as u64);

/// Don't enable IRQ or NMI automatically when users request it. Users will
/// enable it explicitly by `enable_irq` or `enable_nmi` later.
pub const NO_AUTOEN: Flags = Flags(bindings::IRQF_NO_AUTOEN as u64);

/// Exclude from runnaway detection for IPI and similar handlers, depends on
/// `PERCPU`.
pub const NO_DEBUG: Flags = Flags(bindings::IRQF_NO_DEBUG as u64);
