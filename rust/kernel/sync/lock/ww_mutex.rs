// SPDX-License-Identifier: GPL-2.0

//! Rust abstractions for the kernel's wound-wait locking primitives.
//!
//! It is designed to avoid deadlocks when locking multiple [`Mutex`]es
//! that belong to the same [`Class`]. Each lock acquisition uses an
//! [`AcquireCtx`] to track ordering and ensure forward progress.

use crate::error::to_result;
use crate::prelude::*;
use crate::types::{NotThreadSafe, Opaque};
use crate::{bindings, container_of};

use core::cell::UnsafeCell;
use core::marker::PhantomData;

pub use acquire_ctx::AcquireCtx;
pub use class::Class;

mod acquire_ctx;
mod class;

/// A wound-wait (ww) mutex that is powered with deadlock avoidance
/// when acquiring multiple locks of the same [`Class`].
///
/// Each mutex belongs to a [`Class`], which the wound-wait algorithm
/// uses to figure out the order of acquisition and prevent deadlocks.
///
/// # Examples
///
/// ```
/// use kernel::c_str;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::ww_mutex::{AcquireCtx, Class, Mutex};
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = Class::new_wound_wait(c_str!("some_class")));
/// let mutex = Arc::pin_init(Mutex::new(42, &class), GFP_KERNEL)?;
///
/// let ctx = KBox::pin_init(AcquireCtx::new(&class), GFP_KERNEL)?;
///
/// // SAFETY: Both `ctx` and `mutex` uses the same class.
/// let guard = unsafe { ctx.lock(&mutex)? };
/// assert_eq!(*guard, 42);
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
pub struct Mutex<'a, T: ?Sized> {
    #[pin]
    inner: Opaque<bindings::ww_mutex>,
    _p: PhantomData<&'a Class>,
    data: UnsafeCell<T>,
}

// SAFETY: `Mutex` can be sent to another thread if the protected
// data `T` can be.
unsafe impl<T: ?Sized + Send> Send for Mutex<'_, T> {}

// SAFETY: `Mutex` can be shared across threads if the protected
// data `T` can be.
unsafe impl<T: ?Sized + Send + Sync> Sync for Mutex<'_, T> {}

impl<'class, T> Mutex<'class, T> {
    /// Initializes [`Mutex`] with the given `data` and [`Class`].
    pub fn new(data: T, class: &'class Class) -> impl PinInit<Self> {
        let class_ptr = class.inner.get();
        pin_init!(Mutex {
            inner <- Opaque::ffi_init(|slot: *mut bindings::ww_mutex| {
                // SAFETY: `class` is valid for the lifetime `'class` captured by `Self`.
                unsafe { bindings::ww_mutex_init(slot, class_ptr) }
            }),
            data: UnsafeCell::new(data),
            _p: PhantomData
        })
    }
}

impl<'class> Mutex<'class, ()> {
    /// Creates a [`Mutex`] from a raw pointer.
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is a valid pointer to a `ww_mutex`
    /// and that it remains valid for the lifetime `'a`.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::ww_mutex) -> &'a Self {
        // SAFETY: By the safety contract, the caller guarantees that `ptr`
        // points to a valid `ww_mutex` which is the `inner` field of `Mutex`
        // and remains valid for the lifetime `'a`.
        unsafe { &*container_of!(Opaque::cast_from(ptr), Self, inner) }
    }
}

impl<'class, T: ?Sized> Mutex<'class, T> {
    /// Checks if the mutex is currently locked.
    pub fn is_locked(&self) -> bool {
        // SAFETY: The mutex is pinned and valid.
        unsafe { bindings::ww_mutex_is_locked(self.inner.get()) }
    }

    /// Locks the given mutex without acquire context ([`AcquireCtx`]).
    pub fn lock<'a>(&'a self) -> Result<MutexGuard<'a, T>> {
        // SAFETY: `ctx` is `None`, so no class matching is required.
        unsafe { lock_common(self, None, LockKind::Regular) }
    }

    /// Similar to `lock`, but can be interrupted by signals.
    pub fn lock_interruptible<'a>(&'a self) -> Result<MutexGuard<'a, T>> {
        // SAFETY: `ctx` is `None`, so no class matching is required.
        unsafe { lock_common(self, None, LockKind::Interruptible) }
    }

    /// Locks the given mutex without acquire context ([`AcquireCtx`]) using the slow path.
    ///
    /// This function should be used when `lock` fails (typically due to a potential deadlock).
    pub fn lock_slow<'a>(&'a self) -> Result<MutexGuard<'a, T>> {
        // SAFETY: `ctx` is `None`, so no class matching is required.
        unsafe { lock_common(self, None, LockKind::Slow) }
    }

    /// Similar to `lock_slow`, but can be interrupted by signals.
    pub fn lock_slow_interruptible<'a>(&'a self) -> Result<MutexGuard<'a, T>> {
        // SAFETY: `ctx` is `None`, so no class matching is required.
        unsafe { lock_common(self, None, LockKind::SlowInterruptible) }
    }

    /// Tries to lock the mutex with no [`AcquireCtx`] and without blocking.
    ///
    /// Unlike `lock`, no deadlock handling is performed.
    pub fn try_lock<'a>(&'a self) -> Result<MutexGuard<'a, T>> {
        // SAFETY: `ctx` is `None`, so no class matching is required.
        unsafe { lock_common(self, None, LockKind::Try) }
    }
}

/// A guard that provides exclusive access to the data protected
/// by a [`Mutex`].
///
/// # Invariants
///
/// The guard holds an exclusive lock on the associated [`Mutex`]. The lock is held
/// for the entire lifetime of this guard and is automatically released when the
/// guard is dropped.
#[must_use = "the lock unlocks immediately when the guard is unused"]
pub struct MutexGuard<'a, T: ?Sized> {
    mutex: &'a Mutex<'a, T>,
    _not_send: NotThreadSafe,
}

// SAFETY: [`MutexGuard`] can be shared between threads if the data can.
unsafe impl<T: ?Sized + Sync> Sync for MutexGuard<'_, T> {}

impl<'a, T: ?Sized> MutexGuard<'a, T> {
    /// Creates a new guard for a locked mutex.
    fn new(mutex: &'a Mutex<'a, T>) -> Self {
        Self {
            mutex,
            _not_send: NotThreadSafe,
        }
    }
}

impl<'a> MutexGuard<'a, ()> {
    /// Creates a [`MutexGuard`] from a raw pointer.
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is a valid pointer to a `ww_mutex`
    /// and that it remains valid for the lifetime `'a`.
    pub unsafe fn from_raw<'b>(ptr: *mut bindings::ww_mutex) -> MutexGuard<'b, ()> {
        // SAFETY: By the safety contract, the caller guarantees that `ptr`
        // points to a valid `ww_mutex` which is the `mutex` field of `Mutex`
        // and remains valid for the lifetime `'a`.
        let mutex = unsafe { Mutex::from_raw(ptr) };

        MutexGuard::new(mutex)
    }
}

impl<T: ?Sized> core::ops::Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: We hold the lock, so we have exclusive access.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized + Unpin> core::ops::DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: We hold the lock, so we have exclusive access.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: We hold the lock and are about to release it.
        unsafe { bindings::ww_mutex_unlock(self.mutex.inner.get()) };
    }
}

/// Locking kinds used by [`lock_common`] to unify the internal
/// locking logic.
///
/// It's best not to expose this type (and [`lock_common`]) to the
/// kernel, as it allows internal API changes without worrying
/// about breaking external compatibility.
#[derive(Copy, Clone, Debug)]
enum LockKind {
    /// Blocks until lock is acquired.
    Regular,
    /// Blocks but can be interrupted by signals.
    Interruptible,
    /// Used in slow path after deadlock detection.
    Slow,
    /// Slow path but interruptible.
    SlowInterruptible,
    /// Does not block, returns immediately if busy.
    Try,
}

/// Internal helper that unifies the different locking kinds.
///
/// # Safety
///
/// If `ctx` is `Some`, the given `mutex` must be created with the [`Class`] that
/// was used to initialize `ctx`.
unsafe fn lock_common<'a, T: ?Sized>(
    mutex: &'a Mutex<'a, T>,
    ctx: Option<&AcquireCtx<'_>>,
    kind: LockKind,
) -> Result<MutexGuard<'a, T>> {
    let ctx_ptr = ctx.map_or(core::ptr::null_mut(), |c| c.inner.get());

    let mutex_ptr = mutex.inner.get();

    match kind {
        LockKind::Regular => {
            // SAFETY: `Mutex` is always pinned. If `AcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_lock(mutex_ptr, ctx_ptr) };

            to_result(ret)?;
        }
        LockKind::Interruptible => {
            // SAFETY: `Mutex` is always pinned. If `AcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_lock_interruptible(mutex_ptr, ctx_ptr) };

            to_result(ret)?;
        }
        LockKind::Slow => {
            // SAFETY: `Mutex` is always pinned. If `AcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            unsafe { bindings::ww_mutex_lock_slow(mutex_ptr, ctx_ptr) };
        }
        LockKind::SlowInterruptible => {
            // SAFETY: `Mutex` is always pinned. If `AcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_lock_slow_interruptible(mutex_ptr, ctx_ptr) };

            to_result(ret)?;
        }
        LockKind::Try => {
            // SAFETY: `Mutex` is always pinned. If `AcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_trylock(mutex_ptr, ctx_ptr) };

            if ret == 0 {
                return Err(EBUSY);
            } else {
                to_result(ret)?;
            }
        }
    };

    Ok(MutexGuard::new(mutex))
}
