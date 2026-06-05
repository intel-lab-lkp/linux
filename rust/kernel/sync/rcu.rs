// SPDX-License-Identifier: GPL-2.0

//! RCU support.
//!
//! C header: [`include/linux/rcupdate.h`](srctree/include/linux/rcupdate.h)

use core::pin::Pin;

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

/// Declares a struct is safe to free after a grace period if all readers are guarded by RCU.
///
/// # Safety
///
/// Implementation must guarantee `drop_before_gp()` makes sure no future RCU reader will access
/// any part of [`Self`], as a result, after `drop_before_gp()` return + one grace period, no RCU
/// reader will be on the object, and it's safe to free it.
///
/// Notes for implementators: implementing this trait in general requires `Self` being a
/// [`UnsafePinned`], i.e. a `&mut Self` is not a noalias reference if `Self` has non-trivial
/// `drop()` function.
pub unsafe trait RcuFreeSafe {
    fn drop_before_gp(self: Pin<&mut Self>);
}

macro_rules! impl_not_drop {
    ($($t:ty, )*) => {
        // SAFETY: Dropping `T` has no side effect means `T` is always ready to be freed. And an
        // empty `drop_before_gp()` suffices.
        $(unsafe impl RcuFreeSafe for $t {
            fn drop_before_gp(self: Pin<&mut Self>) {
                $crate::const_assert!(!core::mem::needs_drop::<$t>());
            }
        })*
    }
}

impl_not_drop! {i8,u8,i16,u16,i32,u32,isize,usize,i64,u64,}
