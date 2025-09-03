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

    /// Locks the given mutex.
    pub fn lock<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe { bindings::ww_mutex_lock(ww_mutex.mutex.get(), self.inner.get()) };

        to_result(ret)?;

        Ok(WwMutexGuard::new(ww_mutex))
    }

    /// Similar to `lock`, but can be interrupted by signals.
    pub fn lock_interruptible<'a, T>(
        &'a self,
        ww_mutex: &'a WwMutex<'a, T>,
    ) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe {
            bindings::ww_mutex_lock_interruptible(ww_mutex.mutex.get(), self.inner.get())
        };

        to_result(ret)?;

        Ok(WwMutexGuard::new(ww_mutex))
    }

    /// Locks the given mutex using the slow path.
    ///
    /// This function should be used when `lock` fails (typically due to a potential deadlock).
    pub fn lock_slow<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid, and we're in the slow path.
        unsafe { bindings::ww_mutex_lock_slow(ww_mutex.mutex.get(), self.inner.get()) };

        Ok(WwMutexGuard::new(ww_mutex))
    }

    /// Similar to `lock_slow`, but can be interrupted by signals.
    pub fn lock_slow_interruptible<'a, T>(
        &'a self,
        ww_mutex: &'a WwMutex<'a, T>,
    ) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid, and we are in the slow path.
        let ret = unsafe {
            bindings::ww_mutex_lock_slow_interruptible(ww_mutex.mutex.get(), self.inner.get())
        };

        to_result(ret)?;

        Ok(WwMutexGuard::new(ww_mutex))
    }

    /// Tries to lock the mutex without blocking.
    ///
    /// Unlike `lock`, no deadlock handling is performed.
    pub fn try_lock<'a, T>(&'a self, ww_mutex: &'a WwMutex<'a, T>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe { bindings::ww_mutex_trylock(ww_mutex.mutex.get(), self.inner.get()) };

        if ret == 0 {
            return Err(EBUSY);
        } else {
            to_result(ret)?;
        }

        Ok(WwMutexGuard::new(ww_mutex))
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

impl<T: ?Sized> WwMutex<'_, T> {
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
