// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Utilities for working with `struct poll_table`.

use crate::{
    alloc::AllocError,
    bindings,
    container_of,
    fs::File,
    prelude::*,
    sync::atomic::{Acquire, Atomic, Relaxed, Release},
    sync::lock::{Backend, Lock},
    sync::{CondVar, LockClassKey},
    types::Opaque, //
};
use core::{
    marker::{PhantomData, PhantomPinned},
    ops::Deref,
    ptr,
};

/// Creates a [`PollCondVar`] initialiser with the given name and a newly-created lock class.
#[macro_export]
macro_rules! new_poll_condvar {
    ($($name:literal)?) => {
        $crate::sync::poll::PollCondVar::new(
            $crate::optional_name!($($name)?), $crate::static_lock_class!()
        )
    };
}

/// Wraps the kernel's `poll_table`.
///
/// # Invariants
///
/// The pointer must be null or reference a valid `poll_table`.
#[repr(transparent)]
pub struct PollTable<'a> {
    table: *mut bindings::poll_table,
    _lifetime: PhantomData<&'a bindings::poll_table>,
}

impl<'a> PollTable<'a> {
    /// Creates a [`PollTable`] from a valid pointer.
    ///
    /// # Safety
    ///
    /// The pointer must be null or reference a valid `poll_table` for the duration of `'a`.
    pub unsafe fn from_raw(table: *mut bindings::poll_table) -> Self {
        // INVARIANTS: The safety requirements are the same as the struct invariants.
        PollTable {
            table,
            _lifetime: PhantomData,
        }
    }

    /// Register this [`PollTable`] with the provided [`PollCondVar`], so that it can be notified
    /// using the condition variable.
    pub fn register_wait(&self, file: &File, cv: &PollCondVar) {
        // SAFETY:
        // * `file.as_ptr()` references a valid file for the duration of this call.
        // * `self.table` is null or references a valid poll_table for the duration of this call.
        // * Since `PollCondVar` is pinned, its destructor is guaranteed to run before the memory
        //   containing `cv.wait_queue_head` is invalidated. Since the destructor clears all
        //   waiters and then waits for an rcu grace period, it's guaranteed that
        //   `cv.wait_queue_head` remains valid for at least an rcu grace period after the removal
        //   of the last waiter.
        unsafe { bindings::poll_wait(file.as_ptr(), cv.wait_queue_head.get(), self.table) }
    }
}

/// A wrapper around [`CondVar`] that makes it usable with [`PollTable`].
///
/// [`CondVar`]: crate::sync::CondVar
#[pin_data(PinnedDrop)]
#[repr(transparent)]
pub struct PollCondVar {
    #[pin]
    inner: CondVar,
}

impl PollCondVar {
    /// Constructs a new condvar initialiser.
    pub fn new(name: &'static CStr, key: Pin<&'static LockClassKey>) -> impl PinInit<Self> {
        pin_init!(Self {
            inner <- CondVar::new(name, key),
        })
    }

    /// Use this `CondVar` as a `PollCondVar`.
    ///
    /// # Safety
    ///
    /// After the last use of the returned `&PollCondVar`, `__wake_up_pollfree` must be called on
    /// the `wait_queue_head` at least one grace period before the `CondVar` is destroyed.
    unsafe fn from_non_poll(c: &CondVar) -> &PollCondVar {
        // SAFETY: Layout is the same. Caller ensures that PollTables are cleared in time.
        unsafe { &*ptr::from_ref(c).cast() }
    }
}

// Make the `CondVar` methods callable on `PollCondVar`.
impl Deref for PollCondVar {
    type Target = CondVar;

    fn deref(&self) -> &CondVar {
        &self.inner
    }
}

#[pinned_drop]
impl PinnedDrop for PollCondVar {
    #[inline]
    fn drop(self: Pin<&mut Self>) {
        // Clear anything registered using `register_wait`.
        //
        // SAFETY: The pointer points at a valid `wait_queue_head`.
        unsafe { bindings::__wake_up_pollfree(self.inner.wait_queue_head.get()) };

        // Wait for epoll items to be properly removed.
        //
        // SAFETY: Just an FFI call.
        unsafe { bindings::synchronize_rcu() };
    }
}

/// Wrapper around [`CondVar`] that can be upgraded to [`PollCondVar`].
///
/// By using this wrapper, you can avoid rcu for cases that don't use [`PollTable`], and in all
/// cases you can avoid `synchronize_rcu()`.
///
/// # Invariants
///
/// `active` either references `simple`, or a `kmalloc` allocation holding an
/// `UpgradePollCondVarInner`. In the latter case, the allocation remains valid until
/// `Self::drop()` plus one grace period.
#[pin_data(PinnedDrop)]
pub struct UpgradePollCondVar {
    #[pin]
    simple: CondVar,
    active: Atomic<*const CondVar>,
    #[pin]
    _pin: PhantomPinned,
}

#[pin_data]
#[repr(C)]
struct UpgradePollCondVarInner {
    #[pin]
    upgraded: CondVar,
    #[pin]
    rcu: Opaque<bindings::callback_head>,
}

impl UpgradePollCondVar {
    /// Constructs a new upgradable condvar initialiser.
    pub fn new(name: &'static CStr, key: Pin<&'static LockClassKey>) -> impl PinInit<Self> {
        pin_init!(&this in Self {
            simple <- CondVar::new(name, key),
            // SAFETY: `this->simple` is in-bounds. Pointer remains valid since this type is
            // pinned.
            active: Atomic::new(unsafe { &raw const (*this.as_ptr()).simple }),
            _pin: PhantomPinned,
        })
    }

    /// Obtain a [`PollCondVar`], upgrading if necessary.
    ///
    /// You should use the same lock as what is passed to the `wait_*` methods. Otherwise wakeups
    /// may be missed.
    pub fn poll<T: ?Sized, B: Backend>(
        &self,
        lock: &Lock<T, B>,
        name: &'static CStr,
        key: Pin<&'static LockClassKey>,
    ) -> Result<&PollCondVar, AllocError> {
        let mut ptr = self.active.load(Acquire);
        if ptr::eq(ptr, &self.simple) {
            self.upgrade(lock, name, key)?;
            ptr = self.active.load(Acquire);
            debug_assert_ne!(ptr, ptr::from_ref(&self.simple));
        }
        // SAFETY: Signature ensures that last use of returned `&PollCondVar` is before drop(), and
        // drop() calls `__wake_up_pollfree` followed by waiting a grace period before the
        // `CondVar` is destroyed.
        Ok(unsafe { PollCondVar::from_non_poll(&*ptr) })
    }

    fn upgrade<T: ?Sized, B: Backend>(
        &self,
        lock: &Lock<T, B>,
        name: &'static CStr,
        key: Pin<&'static LockClassKey>,
    ) -> Result<(), AllocError> {
        let upgraded = KBox::pin_init(
            pin_init!(UpgradePollCondVarInner {
                upgraded <- CondVar::new(name, key),
                rcu: Opaque::uninit(),
            }),
            GFP_KERNEL,
        )
        .map_err(|_| AllocError)?;

        // SAFETY: The value is treated as pinned.
        let upgraded = KBox::into_raw(unsafe { Pin::into_inner_unchecked(upgraded) });

        let res = self.active.cmpxchg(
            ptr::from_ref(&self.simple),
            // SAFETY: This operation stays in-bounds of the above allocation.
            unsafe { &raw mut (*upgraded).upgraded },
            Release,
        );

        if res.is_err() {
            // Already upgraded, so still succeess.
            // SAFETY: The cmpxchg failed, so take back ownership of the box.
            drop(unsafe { KBox::from_raw(upgraded) });
            return Ok(());
        }

        // If a normal waiter registers in parallel with us, then either:
        // * We took the lock first. In that case, the waiter sees the above cmpxchg.
        // * They took the lock first. In that case, we wake them up below.
        drop(lock.lock());
        self.simple.notify_all();

        Ok(())
    }
}

// Make the `CondVar` methods callable on `UpgradePollCondVar`.
impl Deref for UpgradePollCondVar {
    type Target = CondVar;

    fn deref(&self) -> &CondVar {
        // SAFETY: By the type invariants, this is either `&self.simple` or references an
        // allocation that lives until `UpgradePollCondVar::drop`.
        unsafe { &*self.active.load(Acquire) }
    }
}

#[pinned_drop]
impl PinnedDrop for UpgradePollCondVar {
    #[inline]
    fn drop(self: Pin<&mut Self>) {
        // ORDERING: All calls to upgrade happens-before Drop, so no synchronization is required.
        let ptr = self.active.load(Relaxed);
        if ptr::eq(ptr, &self.simple) {
            return;
        }
        // SAFETY: When the pointer is not &self.active, it is an `UpgradePollCondVarInner`.
        let ptr = unsafe { container_of!(ptr.cast_mut(), UpgradePollCondVarInner, upgraded) };
        // SAFETY: The pointer points at a valid `wait_queue_head`.
        unsafe { bindings::__wake_up_pollfree((*ptr).upgraded.wait_queue_head.get()) };
        // This skips drop of `CondVar`, but that's ok because we reimplemented its drop here.
        //
        // SAFETY: `__wake_up_pollfree` ensures that all registered PollTable instances are gone in
        // one grace period, and this is the destructor so no new PollTable instances can be
        // registered. Thus, it's safety to rcu free the `UpgradePollCondVarInner`.
        unsafe { bindings::kvfree_call_rcu((*ptr).rcu.get(), ptr.cast::<c_void>()) };
    }
}
