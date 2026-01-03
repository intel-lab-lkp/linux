// SPDX-License-Identifier: GPL-2.0

//! Rust abstractions for the kernel's wound-wait locking primitives.
//!
//! It is designed to avoid deadlocks when locking multiple [`Mutex`]es
//! that belong to the same [`Class`]. Each lock acquisition uses an
//! [`AcquireCtx`] to track ordering and ensure forward progress.
//!
//! See srctree/Documentation/locking/ww-mutex-design.rst for more details.

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
/// use kernel::define_ww_class;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::ww_mutex::{AcquireCtx, Class, Mutex};
/// use pin_init::stack_pin_init;
///
/// define_ww_class!(SOME_WW_CLASS);
///
/// let mutex = Arc::pin_init(Mutex::new(42, &SOME_WW_CLASS), GFP_KERNEL)?;
/// let ctx = KBox::pin_init(AcquireCtx::new(&SOME_WW_CLASS), GFP_KERNEL)?;
///
/// let guard = ctx.lock(&mutex)?;
/// assert_eq!(*guard, 42);
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
#[repr(C)]
pub struct Mutex<'a, T: ?Sized> {
    _p: PhantomData<&'a Class>,
    #[pin]
    inner: Opaque<bindings::ww_mutex>,
    data: UnsafeCell<T>,
}

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

impl<'class, T: ?Sized> Mutex<'class, T> {
    /// Checks if this [`Mutex`] is currently locked.
    ///
    /// The returned value is racy as another thread can acquire
    /// or release the lock immediately after this call returns.
    pub fn is_locked(&self) -> bool {
        // SAFETY: It's safe to call `ww_mutex_is_locked` on
        // a valid mutex.
        unsafe { bindings::ww_mutex_is_locked(self.inner.get()) }
    }

    /// Locks this [`Mutex`] without [`AcquireCtx`].
    pub fn lock(&self) -> Result<MutexGuard<'_, T>> {
        lock_common(self, None, LockKind::Regular)
    }

    /// Similar to [`Self::lock`], but can be interrupted by signals.
    pub fn lock_interruptible(&self) -> Result<MutexGuard<'_, T>> {
        lock_common(self, None, LockKind::Interruptible)
    }

    /// Locks this [`Mutex`] without [`AcquireCtx`] using the slow path.
    ///
    /// This function should be used when [`Self::lock`] fails (typically due
    /// to a potential deadlock).
    pub fn lock_slow(&self) -> Result<MutexGuard<'_, T>> {
        lock_common(self, None, LockKind::Slow)
    }

    /// Similar to [`Self::lock_slow`], but can be interrupted by signals.
    pub fn lock_slow_interruptible(&self) -> Result<MutexGuard<'_, T>> {
        lock_common(self, None, LockKind::SlowInterruptible)
    }

    /// Tries to lock this [`Mutex`] with no [`AcquireCtx`] and without blocking.
    ///
    /// Unlike [`Self::lock`], no deadlock handling is performed.
    pub fn try_lock(&self) -> Result<MutexGuard<'_, T>> {
        lock_common(self, None, LockKind::Try)
    }
}

impl<'class> Mutex<'class, ()> {
    /// Creates a [`Mutex`] from a raw pointer.
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    ///
    /// - `ptr` is a valid pointer to a `ww_mutex`.
    /// - `ptr` must remain valid for the lifetime `'a`.
    /// - ww_class associated with this mutex must be valid for
    ///   the lifetime `'class`.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::ww_mutex) -> &'a Self {
        // SAFETY: By the safety contract, the caller guarantees that `ptr`
        // points to a valid `ww_mutex` which is the `inner` field of `Mutex`,
        // that it remains valid for the lifetime `'a` and the associated
        // ww_class outlives `'class`.
        //
        // Because [`Mutex`] is `#[repr(C)]`, the `inner` field sits at a
        // stable offset that `container_of!` can safely rely on.
        unsafe { &*container_of!(Opaque::cast_from(ptr), Self, inner) }
    }
}

// SAFETY: `Mutex` can be sent to another thread if the protected
// data `T` can be.
unsafe impl<T: ?Sized + Send> Send for Mutex<'_, T> {}

// SAFETY: `Mutex` can be shared across threads if the protected
// data `T` can be.
unsafe impl<T: ?Sized + Send + Sync> Sync for Mutex<'_, T> {}

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

impl<'a, T: ?Sized> MutexGuard<'a, T> {
    /// Creates a new guard for the given [`Mutex`].
    fn new(mutex: &'a Mutex<'a, T>) -> Self {
        assert!(mutex.is_locked());

        Self {
            mutex,
            _not_send: NotThreadSafe,
        }
    }
}

impl<'a> MutexGuard<'a, ()> {
    /// Creates a [`MutexGuard`] from a raw pointer.
    ///
    /// If the given pointer refers to a mutex that is not locked,
    /// returns [`EINVAL`].
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that:
    ///
    /// - `ptr` is a valid pointer to a `ww_mutex`.
    /// - `ptr` must remain valid for the lifetime `'b`.
    /// - The `ww_class` associated with the `ww_mutex` must be valid for the lifetime `'b`.
    pub unsafe fn from_raw<'b>(ptr: *mut bindings::ww_mutex) -> Result<MutexGuard<'b, ()>> {
        // SAFETY: By this function's safety contract, the caller guarantees that `ptr` points to a
        // valid `ww_mutex` which is the `inner` field of a `Mutex`. The caller also guarantees
        // that both `ptr` and the associated `ww_class` are valid for the lifetime `'b`.
        let mutex = unsafe { Mutex::from_raw(ptr) };

        if !mutex.is_locked() {
            return Err(EINVAL);
        }

        Ok(MutexGuard::new(mutex))
    }
}

impl<T: ?Sized> core::ops::Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: self.mutex is locked, so we have exclusive access.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized + Unpin> core::ops::DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: self.mutex is locked, so we have exclusive access.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: self.mutex is locked and are about to release it.
        unsafe { bindings::ww_mutex_unlock(self.mutex.inner.get()) };
    }
}

// SAFETY: `MutexGuard` can be shared between threads if the data can.
unsafe impl<T: ?Sized + Sync> Sync for MutexGuard<'_, T> {}

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
/// Returns [`EINVAL`] if the [`Mutex`] has a different [`Class`].
fn lock_common<'a, T: ?Sized>(
    mutex: &'a Mutex<'a, T>,
    ctx: Option<&AcquireCtx<'_>>,
    kind: LockKind,
) -> Result<MutexGuard<'a, T>> {
    let mutex_ptr = mutex.inner.get();

    let ctx_ptr = match ctx {
        Some(acquire_ctx) => {
            let ctx_ptr = acquire_ctx.inner.get();

            // SAFETY: `ctx_ptr` is a valid pointer for the entire
            // lifetime of `ctx`.
            let ctx_class = unsafe { (*ctx_ptr).ww_class };

            // SAFETY: `mutex_ptr` is a valid pointer for the entire
            // lifetime of `mutex`.
            let mutex_class = unsafe { (*mutex_ptr).ww_class };

            // `ctx` and `mutex` must use the same class.
            if ctx_class != mutex_class {
                return Err(EINVAL);
            }

            ctx_ptr
        }
        None => core::ptr::null_mut(),
    };

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

#[kunit_tests(rust_kernel_ww_mutex)]
mod tests {
    use crate::prelude::*;
    use crate::sync::Arc;
    use crate::{define_wd_class, define_ww_class};

    use super::*;

    define_ww_class!(TEST_WOUND_WAIT_CLASS);
    define_wd_class!(TEST_WAIT_DIE_CLASS);

    #[test]
    fn test_ww_mutex_basic_lock_unlock() -> Result {
        let mutex = Arc::pin_init(Mutex::new(42, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let ctx = KBox::pin_init(AcquireCtx::new(&TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;

        let guard = ctx.lock(&mutex)?;
        assert_eq!(*guard, 42);

        // Drop the lock.
        drop(guard);

        let mut guard = ctx.lock(&mutex)?;
        *guard = 100;
        assert_eq!(*guard, 100);

        Ok(())
    }

    #[test]
    fn test_ww_mutex_trylock() -> Result {
        let mutex = Arc::pin_init(Mutex::new(123, &TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;
        let ctx = KBox::pin_init(AcquireCtx::new(&TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;

        // `try_lock` on unlocked mutex should succeed.
        let guard = ctx.try_lock(&mutex)?;
        assert_eq!(*guard, 123);

        // Now it should fail immediately as it's already locked.
        assert!(ctx.try_lock(&mutex).is_err());

        Ok(())
    }

    #[test]
    fn test_ww_mutex_is_locked() -> Result {
        let mutex = Arc::pin_init(Mutex::new("hello", &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let ctx = KBox::pin_init(AcquireCtx::new(&TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;

        // Should not be locked initially.
        assert!(!mutex.is_locked());

        let guard = ctx.lock(&mutex)?;
        assert!(mutex.is_locked());

        drop(guard);
        assert!(!mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_ww_acquire_context_done() -> Result {
        let mutex1 = Arc::pin_init(Mutex::new(1, &TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(Mutex::new(2, &TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;
        let ctx = KBox::pin_init(AcquireCtx::new(&TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;

        // Acquire multiple mutexes with the same context.
        let guard1 = ctx.lock(&mutex1)?;
        let guard2 = ctx.lock(&mutex2)?;

        assert_eq!(*guard1, 1);
        assert_eq!(*guard2, 2);

        // SAFETY: It's called exactly once here and nowhere else.
        unsafe { ctx.done() };

        // We shouldn't be able to lock once it's `done`.
        assert!(ctx.lock(&mutex1).is_err());
        assert!(ctx.lock(&mutex2).is_err());

        Ok(())
    }

    #[test]
    fn test_mutex_without_ctx() -> Result {
        let mutex = Arc::pin_init(Mutex::new(100, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let guard = mutex.lock()?;

        assert_eq!(*guard, 100);
        assert!(mutex.is_locked());

        drop(guard);

        assert!(!mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_guard_from_raw_with_unlocked_mutex() -> Result {
        let mutex = Arc::pin_init(Mutex::new((), &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;

        assert!(!mutex.is_locked());

        // SAFETY: `mutex` remains valid for the duration of this test.
        match unsafe { MutexGuard::from_raw(mutex.inner.get()) } {
            // Should fail with `EINVAL` because the mutex is not locked.
            Err(e) => assert_eq!(e, EINVAL),
            _ => unreachable!(),
        };

        Ok(())
    }
}
