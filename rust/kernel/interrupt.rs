// SPDX-License-Identifier: GPL-2.0

//! Interrupt controls
//!
//! This module allows Rust code to control processor interrupts. [`with_interrupt_disabled()`] may be
//! used for nested disables of interrupts, whereas [`InterruptDisabled`] can be used for annotating code
//! that requires interrupts to be disabled.

use bindings;
use core::marker::*;

/// XXX: Temporarily definition for NotThreadSafe
pub type NotThreadSafe = PhantomData<*mut ()>;

/// XXX: Temporarily const for NotThreadSafe
#[allow(non_upper_case_globals)]
pub const NotThreadSafe: NotThreadSafe = PhantomData;

/// A guard that represent interrupt disable protection.
///
/// [`InterruptDisabled`] is a guard type that represent interrupts have been disabled. Certain
/// functions take an immutable reference of [`InterruptDisabled`] in order to require that they may
/// only be run in interrupt-disabled contexts.
///
/// This is a marker type; it has no size, and is simply used as a compile-time guarantee that
/// interrupts are disabled where required.
///
/// This token can be created by [`with_interrupt_disabled`]. See [`with_interrupt_disabled`] for examples and
/// further information.
///
/// # Invariants
///
/// Interrupts are disabled with `local_interrupt_disable()` when an object of this type exists.
pub struct InterruptDisabled(NotThreadSafe);

/// Disable interrupts.
pub fn interrupt_disable() -> InterruptDisabled {
    // SAFETY: It's always safe to call `local_interrupt_disable()`.
    unsafe { bindings::local_interrupt_disable() };

    InterruptDisabled(NotThreadSafe)
}

impl Drop for InterruptDisabled {
    fn drop(&mut self) {
        // SAFETY: Per type invariants, a `local_interrupt_disable()` must be called to create this
        // object, hence call the corresponding `local_interrupt_enable()` is safe.
        unsafe { bindings::local_interrupt_enable() };
    }
}

impl InterruptDisabled {
    const ASSUME_INTERRUPT_DISABLED: &'static InterruptDisabled = &InterruptDisabled(NotThreadSafe);

    /// Assume that interrupts are disabled.
    ///
    /// # Safety
    ///
    /// For the whole life `'a`, interrupts must be considered disabled, for example an interrupt
    /// handler.
    pub unsafe fn assume_interrupt_disabled<'a>() -> &'a InterruptDisabled {
        Self::ASSUME_INTERRUPT_DISABLED
    }
}
