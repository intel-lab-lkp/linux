// SPDX-License-Identifier: GPL-2.0

//! Atomic and barrier primitives.
//!
//! These primitives should have the same semantics as their C counterparts, for precise definitions
//! of the semantics, please refer to tools/memory-model. Note that Linux Kernel Memory
//! (Consistency) Model is the only model for Rust development in kernel right now, please avoid to
//! use Rust's own atomics.

use core::cell::UnsafeCell;

mod arch;

/// An atomic `i32`.
pub struct AtomicI32(pub(crate) UnsafeCell<i32>);

impl AtomicI32 {
    /// Creates a new atomic value.
    pub fn new(v: i32) -> Self {
        Self(UnsafeCell::new(v))
    }

    /// Adds `i` to the atomic variable with RELAXED ordering.
    ///
    /// Returns the old value before the add.
    ///
    /// # Example
    ///
    /// ```rust
    /// use kernel::sync::atomic::AtomicI32;
    ///
    /// let a = AtomicI32::new(0);
    /// let b = a.fetch_add_relaxed(1);
    /// let c = a.fetch_add_relaxed(2);
    ///
    /// assert_eq!(b, 0);
    /// assert_eq!(c, 1);
    /// ```
    pub fn fetch_add_relaxed(&self, i: i32) -> i32 {
        arch::i32_fetch_add_relaxed(&self.0, i)
    }
}
