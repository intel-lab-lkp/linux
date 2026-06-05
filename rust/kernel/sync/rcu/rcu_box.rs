// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

//! Provides the `RcuBox` type for Rust allocations that live for a grace period.

use core::{
    marker::PhantomData,
    ops::Deref,
    ptr::NonNull, //
};

use crate::{
    alloc::{
        self,
        allocator::{
            KVmalloc,
            Kmalloc,
            Vmalloc, //
        },
        AllocError,
        Allocator, //
    },
    bindings,
    ffi::c_void,
    prelude::*,
    types::ForeignOwnable,
};

use super::{
    ForeignOwnableRcu,
    Guard, //
};

/// A box that is freed with rcu.
///
/// The value must be `Send`, as rcu may drop it on another thread.
///
/// # Invariants
///
/// * The pointer is valid and references a pinned `RcuBoxInner<T>` allocated with `A`.
/// * This `RcuBox` holds exclusive permissions to rcu free the allocation.
pub struct RcuBox<T: Send, A: Allocator>(NonNull<RcuBoxInner<T>>, PhantomData<A>);

/// Type alias for [`RcuBox`] with a [`Kmalloc`] allocator.
///
/// # Examples
///
/// ```
/// # use kernel::sync::rcu::{self, RcuKBox};
/// let rb = RcuKBox::new(42, GFP_KERNEL)?;
///
/// assert_eq!(*rb, 42);
/// assert_eq!(*rb.with_rcu(&rcu::read_lock()), 42);
/// # Ok::<(), Error>(())
/// ```
pub type RcuKBox<T> = RcuBox<T, Kmalloc>;

/// Type alias for [`RcuBox`] with a [`Vmalloc`] allocator.
///
/// # Examples
///
/// ```
/// # use kernel::sync::rcu::{self, RcuVBox};
/// let rb = RcuVBox::new(42, GFP_KERNEL)?;
///
/// assert_eq!(*rb, 42);
/// assert_eq!(*rb.with_rcu(&rcu::read_lock()), 42);
/// # Ok::<(), Error>(())
/// ```
pub type RcuVBox<T> = RcuBox<T, Vmalloc>;

/// Type alias for [`RcuBox`] with a [`KVmalloc`] allocator.
///
/// # Examples
///
/// ```
/// # use kernel::sync::rcu::{self, RcuKVBox};
/// let rb = RcuKVBox::new(42, GFP_KERNEL)?;
///
/// assert_eq!(*rb, 42);
/// assert_eq!(*rb.with_rcu(&rcu::read_lock()), 42);
/// # Ok::<(), Error>(())
/// ```
pub type RcuKVBox<T> = RcuBox<T, KVmalloc>;

struct RcuBoxInner<T> {
    rcu_head: bindings::callback_head,
    value: T,
}

// Note that `T: Sync` is required since when moving an `RcuBox<T, A>`, the previous owner may
// still access `&T` for one grace period.
//
// SAFETY: Ownership of the `RcuBox<T, A>` allows for `&T` and dropping the `T`, so `T: Send +
// Sync` implies `RcuBox<T, A>: Send`.
unsafe impl<T: Send + Sync, A: Allocator> Send for RcuBox<T, A> {}

// SAFETY: `&RcuBox<T, A>` allows for no operations other than those permitted by `&T`, so `T:
// Sync` implies `RcuBox<T, A>: Sync`.
unsafe impl<T: Send + Sync, A: Allocator> Sync for RcuBox<T, A> {}

impl<T: Send, A: Allocator> RcuBox<T, A> {
    /// Create a new `RcuBox`.
    pub fn new(x: T, flags: alloc::Flags) -> Result<Self, AllocError> {
        let b = Box::<_, A>::new(
            RcuBoxInner {
                value: x,
                rcu_head: Default::default(),
            },
            flags,
        )?;

        // INVARIANT:
        // * The pointer contains a valid `RcuBoxInner` allocated with `A`.
        // * We just allocated it, so we own free permissions.
        Ok(RcuBox(NonNull::from(Box::leak(b)), PhantomData))
    }

    /// Access the value for a grace period.
    pub fn with_rcu<'rcu>(&self, _read_guard: &'rcu Guard) -> &'rcu T {
        // SAFETY: The `RcuBox` has not been dropped yet, so the value is valid for at least one
        // grace period.
        unsafe { &(*self.0.as_ptr()).value }
    }
}

impl<T: Send, A: Allocator> Deref for RcuBox<T, A> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: While the `RcuBox<T>` exists, the value remains valid.
        unsafe { &(*self.0.as_ptr()).value }
    }
}

// SAFETY:
// * The `RcuBoxInner<T>` was allocated with `A`.
// * `NonNull::as_ptr` returns a non-null pointer.
unsafe impl<T: Send + 'static, A: Allocator> ForeignOwnable for RcuBox<T, A> {
    const FOREIGN_ALIGN: usize = <Box<RcuBoxInner<T>, A> as ForeignOwnable>::FOREIGN_ALIGN;

    type Borrowed<'a> = &'a T;
    type BorrowedMut<'a> = &'a T;

    fn into_foreign(self) -> *mut c_void {
        self.0.as_ptr().cast()
    }

    unsafe fn from_foreign(ptr: *mut c_void) -> Self {
        // INVARIANT: Pointer returned by `into_foreign, A` carries same invariants as `RcuBox<T>`.
        // SAFETY: `into_foreign` never returns a null pointer.
        Self(unsafe { NonNull::new_unchecked(ptr.cast()) }, PhantomData)
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

impl<T: Send + 'static, A: Allocator> ForeignOwnableRcu for RcuBox<T, A> {
    type RcuBorrowed<'a> = &'a T;

    unsafe fn rcu_borrow<'a>(ptr: *mut c_void) -> &'a T {
        // SAFETY: `RcuBox::drop` can only run after `from_foreign` is called, and the value is
        // valid until `RcuBox::drop` plus one grace period.
        unsafe { &(*ptr.cast::<RcuBoxInner<T>>()).value }
    }
}

impl<T: Send, A: Allocator> Drop for RcuBox<T, A> {
    fn drop(&mut self) {
        // SAFETY: The `rcu_head` field is in-bounds of a valid allocation.
        let rcu_head = unsafe { &raw mut (*self.0.as_ptr()).rcu_head };
        if core::mem::needs_drop::<T>() {
            // SAFETY: `rcu_head` is the `rcu_head` field of `RcuBoxInner<T>`. All users will be
            // gone in an rcu grace period. This is the destructor, so we may pass ownership of the
            // allocation.
            unsafe { bindings::call_rcu(rcu_head, Some(drop_rcu_box::<T, A>)) };
        } else {
            // SAFETY: All users will be gone in an rcu grace period.
            // TODO: We are luckily since `kvfree_call_rcu()` works on both kmalloc and vmalloc,
            // maybe a new `Allocator` method is needed.
            unsafe { bindings::kvfree_call_rcu(rcu_head, self.0.as_ptr().cast()) };
        }
    }
}

/// Free this `RcuBoxInner<T>`.
///
/// # Safety
///
/// `head` references the `rcu_head` field of an `RcuBoxInner<T>` that has no references to it.
/// Ownership of the `Box<RcuBoxInner<T>, A>` must be passed.
unsafe extern "C" fn drop_rcu_box<T, A: Allocator>(head: *mut bindings::callback_head) {
    // SAFETY: Caller provides a pointer to the `rcu_head` field of a `RcuBoxInner<T>`.
    let box_inner = unsafe { crate::container_of!(head, RcuBoxInner<T>, rcu_head) };

    // SAFETY: Caller ensures exclusive access and passed ownership.
    drop(unsafe { Box::<_, A>::from_raw(box_inner) });
}

#[kunit_tests(rust_rcu_box)]
mod tests {
    use super::*;

    #[test]
    fn rcu_box_basic() -> Result {
        let rb = RcuBox::<_, alloc::allocator::Kmalloc>::new(42i32, alloc::flags::GFP_KERNEL)?;

        assert_eq!(*rb, 42);
        assert_eq!(*rb.with_rcu(&Guard::new()), 42);

        drop(rb);

        let rb = RcuBox::<_, alloc::allocator::Vmalloc>::new(42i32, alloc::flags::GFP_KERNEL)?;

        assert_eq!(*rb, 42);
        assert_eq!(*rb.with_rcu(&Guard::new()), 42);

        drop(rb);

        Ok(())
    }
}
