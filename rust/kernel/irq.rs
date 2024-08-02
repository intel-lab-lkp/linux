// SPDX-License-Identifier: GPL-2.0

//! Interrupt controls
//!
//! This module allows Rust code to control processor interrupts. [`with_irqs_disabled()`] may be
//! used for nested disables of interrupts, whereas [`IrqDisabled`] can be used for annotating code
//! that requires interrupts to be disabled.

use bindings;
use core::marker::*;

/// A token that is only available in contexts where IRQs are disabled.
///
/// [`IrqDisabled`] is marker made available when interrupts are not active. Certain functions take
/// an [`IrqDisabled`] in order to indicate that they may only be run in IRQ-free contexts.
///
/// This is a marker type; it has no size, and is simply used as a compile-time guarantee that
/// interrupts are disabled where required.
///
/// This token can be created by [`with_irqs_disabled`]. See [`with_irqs_disabled`] for examples and
/// further information.
#[derive(Copy, Clone, Debug, Ord, Eq, PartialOrd, PartialEq, Hash)]
pub struct IrqDisabled<'a>(PhantomData<(&'a (), *mut ())>);

impl IrqDisabled<'_> {
    /// Create a new [`IrqDisabled`] without disabling interrupts.
    ///
    /// This creates an [`IrqDisabled`] token, which can be passed to functions that must be run
    /// without interrupts. If debug assertions are enabled, this function will assert that
    /// interrupts are disabled upon creation. Otherwise, it has no size or cost at runtime.
    ///
    /// # Panics
    ///
    /// If debug assertions are enabled, this function will panic if interrupts are not disabled
    /// upon creation.
    ///
    /// # Safety
    ///
    /// This function must only be called in contexts where it is already known that interrupts have
    /// been disabled for the current CPU, as the user is making a promise that they will remain
    /// disabled at least until this [`IrqDisabled`] is dropped.
    pub unsafe fn new() -> Self {
        // SAFETY: FFI call with no special requirements
        debug_assert!(unsafe { bindings::irqs_disabled() });

        Self(PhantomData)
    }
}

/// Run the closure `cb` with interrupts disabled on the local CPU.
///
/// This creates an [`IrqDisabled`] token, which can be passed to functions that must be run
/// without interrupts.
///
/// # Examples
///
/// Using [`with_irqs_disabled`] to call a function that can only be called with interrupts
/// disabled:
///
/// ```
/// use kernel::irq::{IrqDisabled, with_irqs_disabled};
///
/// // Requiring interrupts be disabled to call a function
/// fn dont_interrupt_me(_irq: IrqDisabled<'_>) {
///     /* When this token is available, IRQs are known to be disabled. Actions that rely on this
///      * can be safely performed
///      */
/// }
///
/// // Disabling interrupts. They'll be re-enabled once this closure completes.
/// with_irqs_disabled(|irq| dont_interrupt_me(irq));
/// ```
#[inline]
pub fn with_irqs_disabled<T>(cb: impl for<'a> FnOnce(IrqDisabled<'a>) -> T) -> T {
    // SAFETY: FFI call with no special requirements
    let flags = unsafe { bindings::local_irq_save() };

    let ret = cb(IrqDisabled(PhantomData));

    // SAFETY: `flags` comes from our previous call to local_irq_save
    unsafe { bindings::local_irq_restore(flags) };

    ret
}
