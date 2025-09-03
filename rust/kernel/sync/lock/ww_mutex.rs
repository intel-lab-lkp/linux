// SPDX-License-Identifier: GPL-2.0

//! A kernel Wound/Wait Mutex.
//!
//! This module provides Rust abstractions for the Linux kernel's `ww_mutex` implementation,
//! which provides deadlock avoidance through a wait-wound or wait-die algorithm.
//!
//! C header: [`include/linux/ww_mutex.h`](srctree/include/linux/ww_mutex.h)
//!
//! For more information: <https://docs.kernel.org/locking/ww-mutex-design.html>

use crate::bindings;
use crate::error::to_result;
use crate::prelude::*;
use crate::types::{NotThreadSafe, Opaque};
use core::cell::UnsafeCell;
use core::marker::PhantomData;

/// Create static [`WwClass`] instances.
///
/// # Examples
///
/// ```
/// use kernel::{c_str, define_ww_class};
///
/// define_ww_class!(WOUND_WAIT_GLOBAL_CLASS, wound_wait, c_str!("wound_wait_global_class"));
/// define_ww_class!(WAIT_DIE_GLOBAL_CLASS, wait_die, c_str!("wait_die_global_class"));
/// ```
#[macro_export]
macro_rules! define_ww_class {
    ($name:ident, wound_wait, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::WwClass::unpinned_new($class_name, false) };
    };
    ($name:ident, wait_die, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::WwClass::unpinned_new($class_name, true) };
    };
}

/// A class used to group mutexes together for deadlock avoidance.
///
/// All mutexes that might be acquired together should use the same class.
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::WwClass;
/// use kernel::c_str;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let _wait_die_class = WwClass::new_wait_die(c_str!("graphics_buffers")));
/// stack_pin_init!(let _wound_wait_class = WwClass::new_wound_wait(c_str!("memory_pools")));
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
pub struct WwClass {
    #[pin]
    inner: Opaque<bindings::ww_class>,
}

// SAFETY: [`WwClass`] is set up once and never modified. It's fine to share it across threads.
unsafe impl Sync for WwClass {}
// SAFETY: Doesn't hold anything thread-specific. It's safe to send to other threads.
unsafe impl Send for WwClass {}

impl WwClass {
    /// Creates an unpinned [`WwClass`].
    ///
    /// # Safety
    ///
    /// Caller must guarantee that the returned value is not moved after creation.
    pub const unsafe fn unpinned_new(name: &'static CStr, is_wait_die: bool) -> Self {
        WwClass {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_char_ptr(),
                mutex_name: name.as_char_ptr(),
                is_wait_die: is_wait_die as u32,
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                acquire_key: unsafe { core::mem::zeroed() },
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                mutex_key: unsafe { core::mem::zeroed() },
            }),
        }
    }

    /// Creates a [`WwClass`].
    ///
    /// You should not use this function directly. Use the [`define_ww_class!`]
    /// macro or call [`WwClass::new_wait_die`] or [`WwClass::new_wound_wait`] instead.
    const fn new(name: &'static CStr, is_wait_die: bool) -> Self {
        WwClass {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_char_ptr(),
                mutex_name: name.as_char_ptr(),
                is_wait_die: is_wait_die as u32,
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                acquire_key: unsafe { core::mem::zeroed() },
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                mutex_key: unsafe { core::mem::zeroed() },
            }),
        }
    }

    /// Creates wait-die [`WwClass`].
    pub fn new_wait_die(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, true)
    }

    /// Creates wound-wait [`WwClass`].
    pub fn new_wound_wait(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, false)
    }
}

/// Locking kinds used by [`lock_common`] to unify internal FFI locking logic.
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
fn lock_common<'a, T: ?Sized>(
    ww_mutex: &'a WwMutex<'a, T>,
    ctx: Option<&WwAcquireCtx<'_>>,
    kind: LockKind,
) -> Result<WwMutexGuard<'a, T>> {
    let ctx_ptr = ctx.map_or(core::ptr::null_mut(), |c| c.inner.get());

    match kind {
        LockKind::Regular => {
            // SAFETY: `WwMutex` is always pinned. If `WwAcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_lock(ww_mutex.mutex.get(), ctx_ptr) };

            to_result(ret)?;
        }
        LockKind::Interruptible => {
            // SAFETY: `WwMutex` is always pinned. If `WwAcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret =
                unsafe { bindings::ww_mutex_lock_interruptible(ww_mutex.mutex.get(), ctx_ptr) };

            to_result(ret)?;
        }
        LockKind::Slow => {
            // SAFETY: `WwMutex` is always pinned. If `WwAcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            unsafe { bindings::ww_mutex_lock_slow(ww_mutex.mutex.get(), ctx_ptr) };
        }
        LockKind::SlowInterruptible => {
            // SAFETY: `WwMutex` is always pinned. If `WwAcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe {
                bindings::ww_mutex_lock_slow_interruptible(ww_mutex.mutex.get(), ctx_ptr)
            };

            to_result(ret)?;
        }
        LockKind::Try => {
            // SAFETY: `WwMutex` is always pinned. If `WwAcquireCtx` is `Some`, it is pinned,
            // if `None`, it is set to `core::ptr::null_mut()`. Both cases are safe.
            let ret = unsafe { bindings::ww_mutex_trylock(ww_mutex.mutex.get(), ctx_ptr) };

            if ret == 0 {
                return Err(EBUSY);
            } else {
                to_result(ret)?;
            }
        }
    };

    Ok(WwMutexGuard::new(ww_mutex))
}

/// Groups multiple mutex acquisitions together for deadlock avoidance.
///
/// Must be used when acquiring multiple mutexes of the same class.
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::{WwClass, WwAcquireCtx, WwMutex};
/// use kernel::c_str;
/// use kernel::sync::Arc;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("my_class")));
///
/// // Create mutexes.
/// let mutex1 = Arc::pin_init(WwMutex::new(1, &class), GFP_KERNEL)?;
/// let mutex2 = Arc::pin_init(WwMutex::new(2, &class), GFP_KERNEL)?;
///
/// // Create acquire context for deadlock avoidance.
/// let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;
///
/// // Acquire multiple locks safely.
/// let guard1 = ctx.lock(&mutex1)?;
/// let guard2 = ctx.lock(&mutex2)?;
///
/// // Mark acquisition phase as complete.
/// ctx.done();
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data(PinnedDrop)]
pub struct WwAcquireCtx<'a> {
    #[pin]
    inner: Opaque<bindings::ww_acquire_ctx>,
    _p: PhantomData<&'a WwClass>,
}

impl<'ww_class> WwAcquireCtx<'ww_class> {
    /// Initializes `Self` with calling C side `ww_acquire_init` inside.
    pub fn new(ww_class: &'ww_class WwClass) -> impl PinInit<Self> {
        let class = ww_class.inner.get();
        pin_init!(WwAcquireCtx {
            inner <- Opaque::ffi_init(|slot: *mut bindings::ww_acquire_ctx| {
                // SAFETY: `ww_class` is valid for the lifetime `'ww_class` captured by `Self`.
                unsafe { bindings::ww_acquire_init(slot, class) }
            }),
            _p: PhantomData
        })
    }

    /// Marks the end of the acquire phase.
    ///
    /// After calling this function, no more mutexes can be acquired with this context.
    pub fn done(&self) {
        // SAFETY: The context is pinned and valid.
        unsafe { bindings::ww_acquire_done(self.inner.get()) };
    }

    /// Locks the given mutex on this acquire context ([`WwAcquireCtx`]).
    pub fn lock<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        lock_common(ww_mutex, Some(self), LockKind::Regular)
    }

    /// Similar to `lock`, but can be interrupted by signals.
    pub fn lock_interruptible<'a, T>(
        &'a self,
        ww_mutex: &'a WwMutex<'a, T>,
    ) -> Result<WwMutexGuard<'a, T>> {
        lock_common(ww_mutex, Some(self), LockKind::Interruptible)
    }

    /// Locks the given mutex on this acquire context ([`WwAcquireCtx`]) using the slow path.
    ///
    /// This function should be used when `lock` fails (typically due to a potential deadlock).
    pub fn lock_slow<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        lock_common(ww_mutex, Some(self), LockKind::Slow)
    }

    /// Similar to `lock_slow`, but can be interrupted by signals.
    pub fn lock_slow_interruptible<'a, T>(
        &'a self,
        ww_mutex: &'a WwMutex<'a, T>,
    ) -> Result<WwMutexGuard<'a, T>> {
        lock_common(ww_mutex, Some(self), LockKind::SlowInterruptible)
    }

    /// Tries to lock the mutex on this acquire context ([`WwAcquireCtx`]) without blocking.
    ///
    /// Unlike `lock`, no deadlock handling is performed.
    pub fn try_lock<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        lock_common(ww_mutex, Some(self), LockKind::Try)
    }
}

#[pinned_drop]
impl PinnedDrop for WwAcquireCtx<'_> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: The context is being dropped and is pinned.
        unsafe { bindings::ww_acquire_fini(self.inner.get()) };
    }
}

/// A wound/wait mutex backed with C side `ww_mutex`.
///
/// This is a mutual exclusion primitive that provides deadlock avoidance when
/// acquiring multiple locks of the same class.
///
/// # Examples
///
/// ## Basic Usage
///
/// ```
/// use kernel::c_str;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::ww_mutex::{WwClass, WwAcquireCtx, WwMutex };
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("buffer_class")));
/// let mutex = Arc::pin_init(WwMutex::new(42, &class), GFP_KERNEL)?;
///
/// let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;
///
/// let guard = ctx.lock(&mutex)?;
/// assert_eq!(*guard, 42);
///
/// # Ok::<(), Error>(())
/// ```
///
/// ## Multiple Locks
///
/// ```
/// use kernel::c_str;
/// use kernel::prelude::*;
/// use kernel::sync::Arc;
/// use kernel::sync::lock::ww_mutex::{WwClass, WwAcquireCtx, WwMutex};
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = WwClass::new_wait_die(c_str!("resource_class")));
/// let mutex_a = Arc::pin_init(WwMutex::new("Resource A", &class), GFP_KERNEL)?;
/// let mutex_b = Arc::pin_init(WwMutex::new("Resource B", &class), GFP_KERNEL)?;
///
/// let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;
///
/// // Try to acquire both locks.
/// let guard_a = match ctx.lock(&mutex_a) {
///     Ok(guard) => guard,
///     Err(e) if e == EDEADLK => {
///         // Deadlock detected, use slow path.
///         ctx.lock_slow(&mutex_a)?
///     }
///     Err(e) => return Err(e),
/// };
///
/// let guard_b = ctx.lock(&mutex_b)?;
/// ctx.done();
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
pub struct WwMutex<'a, T: ?Sized> {
    _p: PhantomData<&'a WwClass>,
    #[pin]
    mutex: Opaque<bindings::ww_mutex>,
    data: UnsafeCell<T>,
}

// SAFETY: [`WwMutex`] can be shared between threads.
unsafe impl<T: ?Sized + Send> Send for WwMutex<'_, T> {}
// SAFETY: [`WwMutex`] can be safely accessed from multiple threads concurrently.
unsafe impl<T: ?Sized + Send + Sync> Sync for WwMutex<'_, T> {}

impl<'ww_class, T> WwMutex<'ww_class, T> {
    /// Creates `Self` with calling `ww_mutex_init` inside.
    pub fn new(t: T, ww_class: &'ww_class WwClass) -> impl PinInit<Self> {
        let class = ww_class.inner.get();
        pin_init!(WwMutex {
            mutex <- Opaque::ffi_init(|slot: *mut bindings::ww_mutex| {
                // SAFETY: `ww_class` is valid for the lifetime `'ww_class` captured by `Self`.
                unsafe { bindings::ww_mutex_init(slot, class) }
            }),
            data: UnsafeCell::new(t),
            _p: PhantomData,
        })
    }
}

impl<'ww_class, T: ?Sized> WwMutex<'ww_class, T> {
    /// Returns a raw pointer to the inner mutex.
    fn as_ptr(&self) -> *mut bindings::ww_mutex {
        self.mutex.get()
    }

    /// Checks if the mutex is currently locked.
    ///
    /// Intended for internal tests only and should not be used
    /// anywhere else.
    #[cfg(CONFIG_KUNIT)]
    fn is_locked(&self) -> bool {
        // SAFETY: The mutex is pinned and valid.
        unsafe { bindings::ww_mutex_is_locked(self.mutex.get()) }
    }

    /// Locks the given mutex without acquire context ([`WwAcquireCtx`]).
    pub fn lock<'a>(&'a self) -> Result<WwMutexGuard<'a, T>> {
        lock_common(self, None, LockKind::Regular)
    }

    /// Similar to `lock`, but can be interrupted by signals.
    pub fn lock_interruptible<'a>(&'a self) -> Result<WwMutexGuard<'a, T>> {
        lock_common(self, None, LockKind::Interruptible)
    }

    /// Locks the given mutex without acquire context ([`WwAcquireCtx`]) using the slow path.
    ///
    /// This function should be used when `lock` fails (typically due to a potential deadlock).
    pub fn lock_slow<'a>(&'a self) -> Result<WwMutexGuard<'a, T>> {
        lock_common(self, None, LockKind::Slow)
    }

    /// Similar to `lock_slow`, but can be interrupted by signals.
    pub fn lock_slow_interruptible<'a>(&'a self) -> Result<WwMutexGuard<'a, T>> {
        lock_common(self, None, LockKind::SlowInterruptible)
    }

    /// Tries to lock the mutex without acquire context ([`WwAcquireCtx`]) and without blocking.
    ///
    /// Unlike `lock`, no deadlock handling is performed.
    pub fn try_lock<'a>(&'a self) -> Result<WwMutexGuard<'a, T>> {
        lock_common(self, None, LockKind::Try)
    }
}

/// A guard that provides exclusive access to the data protected
/// by a [`WwMutex`].
///
/// # Invariants
///
/// The guard holds an exclusive lock on the associated [`WwMutex`]. The lock is held
/// for the entire lifetime of this guard and is automatically released when the
/// guard is dropped.
#[must_use = "the lock unlocks immediately when the guard is unused"]
pub struct WwMutexGuard<'a, T: ?Sized> {
    mutex: &'a WwMutex<'a, T>,
    _not_send: NotThreadSafe,
}

// SAFETY: [`WwMutexGuard`] can be shared between threads if the data can.
unsafe impl<T: ?Sized + Sync> Sync for WwMutexGuard<'_, T> {}

impl<'a, T: ?Sized> WwMutexGuard<'a, T> {
    /// Creates a new guard for a locked mutex.
    fn new(mutex: &'a WwMutex<'a, T>) -> Self {
        Self {
            mutex,
            _not_send: NotThreadSafe,
        }
    }
}

impl<T: ?Sized> core::ops::Deref for WwMutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: We hold the lock, so we have exclusive access.
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T: ?Sized> core::ops::DerefMut for WwMutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: We hold the lock, so we have exclusive access.
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T: ?Sized> Drop for WwMutexGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: We hold the lock and are about to release it.
        unsafe { bindings::ww_mutex_unlock(self.mutex.as_ptr()) };
    }
}

#[kunit_tests(rust_kernel_ww_mutex)]
mod tests {
    use crate::c_str;
    use crate::prelude::*;
    use crate::sync::Arc;
    use pin_init::stack_pin_init;

    use super::*;

    // A simple coverage on `define_ww_class` macro.
    define_ww_class!(TEST_WOUND_WAIT_CLASS, wound_wait, c_str!("test_wound_wait"));
    define_ww_class!(TEST_WAIT_DIE_CLASS, wait_die, c_str!("test_wait_die"));

    #[test]
    fn test_ww_mutex_basic_lock_unlock() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("test_mutex_class")));

        let mutex = Arc::pin_init(WwMutex::new(42, &class), GFP_KERNEL)?;

        let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;

        // Lock.
        let guard = ctx.lock(&mutex)?;
        assert_eq!(*guard, 42);

        // Drop the lock.
        drop(guard);

        // Lock it again.
        let mut guard = ctx.lock(&mutex)?;
        *guard = 100;
        assert_eq!(*guard, 100);

        Ok(())
    }

    #[test]
    fn test_ww_mutex_trylock() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("trylock_class")));

        let mutex = Arc::pin_init(WwMutex::new(123, &class), GFP_KERNEL)?;

        let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;

        // `try_lock` on unlocked mutex should succeed.
        let guard = ctx.try_lock(&mutex)?;
        assert_eq!(*guard, 123);

        // Now it should fail immediately as it's already locked.
        assert!(ctx.try_lock(&mutex).is_err());

        Ok(())
    }

    #[test]
    fn test_ww_mutex_is_locked() -> Result {
        stack_pin_init!(let class = WwClass::new_wait_die(c_str!("locked_check_class")));

        let mutex = Arc::pin_init(WwMutex::new("hello", &class), GFP_KERNEL)?;

        let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;

        // Should not be locked initially.
        assert!(!mutex.is_locked());

        let guard = ctx.lock(&mutex)?;
        assert!(mutex.is_locked());

        drop(guard);
        assert!(!mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_ww_acquire_context() -> Result {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("ctx_class")));

        let mutex1 = Arc::pin_init(WwMutex::new(1, &class), GFP_KERNEL)?;
        let mutex2 = Arc::pin_init(WwMutex::new(2, &class), GFP_KERNEL)?;

        let ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL)?;

        // Acquire multiple mutexes with the same context.
        let guard1 = ctx.lock(&mutex1)?;
        let guard2 = ctx.lock(&mutex2)?;

        assert_eq!(*guard1, 1);
        assert_eq!(*guard2, 2);

        ctx.done();

        // We shouldn't be able to lock once it's `done`.
        assert!(ctx.lock(&mutex1).is_err());
        assert!(ctx.lock(&mutex2).is_err());

        Ok(())
    }

    #[test]
    fn test_with_global_classes() -> Result {
        let wound_wait_mutex =
            Arc::pin_init(WwMutex::new(100, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let wait_die_mutex = Arc::pin_init(WwMutex::new(200, &TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;

        let ww_ctx = KBox::pin_init(WwAcquireCtx::new(&TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let wd_ctx = KBox::pin_init(WwAcquireCtx::new(&TEST_WAIT_DIE_CLASS), GFP_KERNEL)?;

        let ww_guard = ww_ctx.lock(&wound_wait_mutex)?;
        let wd_guard = wd_ctx.lock(&wait_die_mutex)?;

        assert_eq!(*ww_guard, 100);
        assert_eq!(*wd_guard, 200);

        assert!(wound_wait_mutex.is_locked());
        assert!(wait_die_mutex.is_locked());

        drop(ww_guard);
        drop(wd_guard);

        assert!(!wound_wait_mutex.is_locked());
        assert!(!wait_die_mutex.is_locked());

        Ok(())
    }

    #[test]
    fn test_mutex_without_ctx() -> Result {
        let mutex = Arc::pin_init(WwMutex::new(100, &TEST_WOUND_WAIT_CLASS), GFP_KERNEL)?;
        let guard = mutex.lock()?;

        assert_eq!(*guard, 100);
        assert!(mutex.is_locked());

        drop(guard);

        assert!(!mutex.is_locked());

        Ok(())
    }
}
