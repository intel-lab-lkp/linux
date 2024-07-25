// SPDX-License-Identifier: GPL-2.0

//! Interrupt controls
//!
//! This module allows Rust code to control processor interrupts. [`with_irqs_disabled()`] may be
//! used for nested disables of interrupts, whereas [`IrqDisabled`] can be used for annotating code
//! that requires that interrupts already be disabled.

use bindings;
use core::marker::*;

/// A guarantee that IRQs are disabled on this CPU
///
/// An [`IrqDisabled`] represents a guarantee that interrupts will remain disabled on the current CPU
/// until the lifetime of the object ends. However, it does not disable or enable interrupts on its
/// own - see [`with_irqs_disabled()`] for that.
///
/// This object has no cost at runtime (TODO: …except if whatever kernel compile-time option that
/// would assert IRQs are enabled or not is enabled - in which case we should actually verify that
/// they're enabled).
///
/// # Examples
///
/// If you want to ensure that a function may only be invoked within contexts where interrupts are
/// disabled, you can do so by requiring that a reference to this type be passed. You can also
/// create this type using unsafe code in order to indicate that it's known that interrupts are
/// already disabled on this CPU
///
/// ```
/// use kernel::irq::{IrqDisabled, disable_irqs};
///
/// // Requiring interrupts be disabled to call a function
/// fn dont_interrupt_me(_irq: &IrqDisabled<'_>) { }
///
/// // Disabling interrupts. They'll be re-enabled once this closure completes.
/// disable_irqs(|irq| dont_interrupt_me(&irq));
/// ```
pub struct IrqDisabled<'a>(PhantomData<&'a ()>);

impl<'a> IrqDisabled<'a> {
    /// Create a new [`IrqDisabled`] without disabling interrupts
    ///
    /// If debug assertions are enabled, this function will check that interrupts are disabled.
    /// Otherwise, it has no cost at runtime.
    ///
    /// # Safety
    ///
    /// This function must only be called in contexts where it is already known that interrupts have
    /// been disabled for the current CPU, as the user is making a promise that they will remain
    /// disabled at least until this [`IrqDisabled`] is dropped.
    pub unsafe fn new() -> Self {
        Self(PhantomData)
    }
}

/// Run the closure `cb` with interrupts disabled on the local CPU.
///
/// Interrupts will be re-enabled once the closure returns. If interrupts were already disabled on
/// this CPU, this is a no-op.
#[inline]
pub fn with_irqs_disabled<T, F>(cb: F) -> T
where
    F: FnOnce(IrqDisabled<'_>) -> T,
{
    // SAFETY: FFI call with no special requirements
    let flags = unsafe { bindings::local_irq_save() };

    let ret = cb(IrqDisabled(PhantomData));

    // SAFETY: `flags` comes from our previous call to local_irq_save
    unsafe { bindings::local_irq_restore(flags) };

    ret
}
