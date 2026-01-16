// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

//! Provides the `RcuBox` type for Rust allocations that live for a grace period.

use core::{ops::Deref, ptr::NonNull};

use kernel::{
    alloc::{self, AllocError},
    bindings,
    ffi::c_void,
    prelude::*,
    sync::rcu::{ForeignOwnableRcu, Guard},
    types::ForeignOwnable,
};

/// A box that is freed with rcu.
///
/// The value must be `Send`, as rcu may drop it on another thread.
///
/// # Invariants
///
/// * The pointer is valid and references a pinned `RcuBoxInner<T>` allocated with `kmalloc`.
/// * This `RcuBox` holds exclusive permissions to rcu free the allocation.
pub struct RcuBox<T: Send>(NonNull<RcuBoxInner<T>>);

struct RcuBoxInner<T> {
    value: T,
    rcu_head: bindings::callback_head,
}

// Note that `T: Sync` is required since when moving an `RcuBox<T>`, the previous owner may still
// access `&T` for one grace period.
//
// SAFETY: Ownership of the `RcuBox<T>` allows for `&T` and dropping the `T`, so `T: Send + Sync`
// implies `RcuBox<T>: Send`.
unsafe impl<T: Send + Sync> Send for RcuBox<T> {}

// SAFETY: `&RcuBox<T>` allows for no operations other than those permitted by `&T`, so `T: Sync`
// implies `RcuBox<T>: Sync`.
unsafe impl<T: Send + Sync> Sync for RcuBox<T> {}

impl<T: Send> RcuBox<T> {
    /// Create a new `RcuBox`.
    pub fn new(x: T, flags: alloc::Flags) -> Result<Self, AllocError> {
        let b = KBox::new(
            RcuBoxInner {
                value: x,
                rcu_head: Default::default(),
            },
            flags,
        )?;

        // INVARIANT:
        // * The pointer contains a valid `RcuBoxInner` allocated with `kmalloc`.
        // * We just allocated it, so we own free permissions.
        Ok(RcuBox(NonNull::from(KBox::leak(b))))
    }

    /// Access the value for a grace period.
    pub fn with_rcu<'rcu>(&self, _read_guard: &'rcu Guard) -> &'rcu T {
        // SAFETY: The `RcuBox` has not been dropped yet, so the value is valid for at least one
        // grace period.
        unsafe { &(*self.0.as_ptr()).value }
    }
}

impl<T: Send> Deref for RcuBox<T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: While the `RcuBox<T>` exists, the value remains valid.
        unsafe { &(*self.0.as_ptr()).value }
    }
}

// SAFETY:
// * The `RcuBoxInner<T>` was allocated with `kmalloc`.
// * `NonNull::as_ptr` returns a non-null pointer.
unsafe impl<T: Send + 'static> ForeignOwnable for RcuBox<T> {
    const FOREIGN_ALIGN: usize = <KBox<RcuBoxInner<T>> as ForeignOwnable>::FOREIGN_ALIGN;

    type Borrowed<'a> = &'a T;
    type BorrowedMut<'a> = &'a T;

    fn into_foreign(self) -> *mut c_void {
        self.0.as_ptr().cast()
    }

    unsafe fn from_foreign(ptr: *mut c_void) -> Self {
        // INVARIANT: Pointer returned by `into_foreign` carries same invariants as `RcuBox<T>`.
        // SAFETY: `into_foreign` never returns a null pointer.
        Self(unsafe { NonNull::new_unchecked(ptr.cast()) })
    }

    unsafe fn borrow<'a>(ptr: *mut c_void) -> &'a T {
        // SAFETY: Caller ensures that `'a` is short enough.
        unsafe { &(*ptr.cast::<RcuBoxInner<T>>()).value }
    }

    unsafe fn borrow_mut<'a>(ptr: *mut c_void) -> &'a T {
        // SAFETY: `borrow_mut` has strictly stronger preconditions than `borrow`.
        unsafe { Self::borrow(ptr) }
    }
}

impl<T: Send + 'static> ForeignOwnableRcu for RcuBox<T> {
    type RcuBorrowed<'a> = &'a T;

    unsafe fn rcu_borrow<'a>(ptr: *mut c_void) -> &'a T {
        // SAFETY: `RcuBox::drop` can only run after `from_foreign` is called, and the value is
        // valid until `RcuBox::drop` plus one grace period.
        unsafe { &(*ptr.cast::<RcuBoxInner<T>>()).value }
    }
}

impl<T: Send> Drop for RcuBox<T> {
    fn drop(&mut self) {
        // SAFETY: The `rcu_head` field is in-bounds of a valid allocation.
        let rcu_head = unsafe { &raw mut (*self.0.as_ptr()).rcu_head };
        if core::mem::needs_drop::<T>() {
            // SAFETY: `rcu_head` is the `rcu_head` field of `RcuBoxInner<T>`. All users will be
            // gone in an rcu grace period. This is the destructor, so we may pass ownership of the
            // allocation.
            unsafe { bindings::call_rcu(rcu_head, Some(drop_rcu_box::<T>)) };
        } else {
            // SAFETY: All users will be gone in an rcu grace period.
            unsafe { bindings::kvfree_call_rcu(rcu_head, self.0.as_ptr().cast()) };
        }
    }
}

/// Free this `RcuBoxInner<T>`.
///
/// # Safety
///
/// `head` references the `rcu_head` field of an `RcuBoxInner<T>` that has no references to it.
/// Ownership of the `KBox<RcuBoxInner<T>>` must be passed.
unsafe extern "C" fn drop_rcu_box<T>(head: *mut bindings::callback_head) {
    // SAFETY: Caller provides a pointer to the `rcu_head` field of a `RcuBoxInner<T>`.
    let box_inner = unsafe { crate::container_of!(head, RcuBoxInner<T>, rcu_head) };

    // SAFETY: Caller ensures exclusive access and passed ownership.
    drop(unsafe { KBox::from_raw(box_inner) });
}
