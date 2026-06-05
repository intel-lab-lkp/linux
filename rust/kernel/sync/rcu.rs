// SPDX-License-Identifier: GPL-2.0

//! RCU support.
//!
//! C header: [`include/linux/rcupdate.h`](srctree/include/linux/rcupdate.h)

use crate::{
    bindings,
    types::{
        ForeignOwnable,
        NotThreadSafe, //
    }, //
};

mod rcu_box;
pub use self::rcu_box::RcuBox;
pub use self::rcu_box::RcuKBox;
pub use self::rcu_box::RcuKVBox;
pub use self::rcu_box::RcuVBox;

/// Evidence that the RCU read side lock is held on the current thread/CPU.
///
/// The type is explicitly not `Send` because this property is per-thread/CPU.
///
/// # Invariants
///
/// The RCU read side lock is actually held while instances of this guard exist.
pub struct Guard(NotThreadSafe);

impl Guard {
    /// Acquires the RCU read side lock and returns a guard.
    #[inline]
    pub fn new() -> Self {
        // SAFETY: An FFI call with no additional requirements.
        unsafe { bindings::rcu_read_lock() };
        // INVARIANT: The RCU read side lock was just acquired above.
        Self(NotThreadSafe)
    }

    /// Explicitly releases the RCU read side lock.
    #[inline]
    pub fn unlock(self) {}
}

impl Default for Guard {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Guard {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: By the type invariants, the RCU read side is locked, so it is ok to unlock it.
        unsafe { bindings::rcu_read_unlock() };
    }
}

/// Acquires the RCU read side lock.
#[inline]
pub fn read_lock() -> Guard {
    Guard::new()
}

/// Declares that a pointer type is rcu safe.
pub trait ForeignOwnableRcu: ForeignOwnable {
    /// Type used to immutably borrow an rcu-safe value that is currently foreign-owned.
    type RcuBorrowed<'a>;

    /// Borrows a foreign-owned object immutably for an rcu grace period.
    ///
    /// This method provides a way to access a foreign-owned rcu-safe value from Rust immutably.
    ///
    /// # Safety
    ///
    /// * The provided pointer must have been returned by a previous call to [`into_foreign`].
    /// * If [`from_foreign`] is called, then `'a` must not end after the call to `from_foreign`
    ///   plus one rcu grace period.
    ///
    /// [`into_foreign`]: ForeignOwnable::into_foreign
    /// [`from_foreign`]: ForeignOwnable::from_foreign
    unsafe fn rcu_borrow<'a>(ptr: *mut ffi::c_void) -> Self::RcuBorrowed<'a>;
}
