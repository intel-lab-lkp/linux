// SPDX-License-Identifier: GPL-2.0

//! A kernel Wound/Wait Mutex.
//!
//! This module provides Rust abstractions for the Linux kernel's `ww_mutex` implementation,
//! which provides deadlock avoidance through a wait-wound or wait-die algorithm.

use crate::error::{to_result, Result};
use crate::prelude::EBUSY;
use crate::types::{NotThreadSafe, Opaque};
use crate::{bindings, str::CStr};
use core::marker::PhantomData;
use core::{cell::UnsafeCell, pin::Pin};
use macros::kunit_tests;
use pin_init::{pin_data, pin_init, pinned_drop, PinInit};

/// Create static `WwClass` instances.
///
/// # Examples
///
/// ```
/// use kernel::c_str;
/// use kernel::define_ww_class;
///
/// define_ww_class!(WOUND_WAIT_GLOBAL_CLASS, wound_wait, c_str!("wound_wait_global_class"));
/// define_ww_class!(WAIT_DIE_GLOBAL_CLASS, wait_die, c_str!("wait_die_global_class"));
/// ```
#[macro_export]
macro_rules! define_ww_class {
    ($name:ident, wound_wait, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            { $crate::sync::lock::ww_mutex::WwClass::new($class_name, false) };
    };
    ($name:ident, wait_die, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            { $crate::sync::lock::ww_mutex::WwClass::new($class_name, true) };
    };
}

/// Represents a group of mutexes that can participate in deadlock avoidance together.
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

// SAFETY: `WwClass` is set up once and never modified. It's fine to share it across threads.
unsafe impl Sync for WwClass {}
// SAFETY: Doesn't hold anything thread-specific. It's safe to send to other threads.
unsafe impl Send for WwClass {}

macro_rules! ww_class_init_helper {
    ($name:expr, $is_wait_die:expr) => {
        Opaque::new(bindings::ww_class {
            stamp: bindings::atomic_long_t { counter: 0 },
            acquire_name: $name.as_char_ptr(),
            mutex_name: $name.as_char_ptr(),
            is_wait_die: $is_wait_die as u32,
            // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
            //
            // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
            // globally on C side.
            //
            // Ref: https://github.com/torvalds/linux/blob/master/include/linux/ww_mutex.h#L85-L89
            acquire_key: unsafe { core::mem::zeroed() },
            // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
            //
            // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
            // globally on C side.
            //
            // Ref: https://github.com/torvalds/linux/blob/master/include/linux/ww_mutex.h#L85-L89
            mutex_key: unsafe { core::mem::zeroed() },
        })
    };
}

impl WwClass {
    /// Creates a `WwClass`.
    ///
    /// It's `pub` only so it can be used by the `define_ww_class!` macro.
    ///
    /// You should not use this function directly. Use the `define_ww_class!`
    /// macro or call `WwClass::new_wait_die` or `WwClass::new_wound_wait` instead.
    pub const fn new(name: &'static CStr, is_wait_die: bool) -> Self {
        WwClass {
            inner: ww_class_init_helper!(name, is_wait_die),
        }
    }

    /// Creates wait-die `WwClass`.
    pub fn new_wait_die(name: &'static CStr) -> impl PinInit<Self> {
        pin_init!(WwClass {
            inner: ww_class_init_helper!(name, true),
        })
    }

    /// Creates wound-wait `WwClass`.
    pub fn new_wound_wait(name: &'static CStr) -> impl PinInit<Self> {
        pin_init!(WwClass {
            inner: ww_class_init_helper!(name, false),
        })
    }
}

/// An acquire context is used to group multiple mutex acquisitions together
/// for deadlock avoidance. It must be used when acquiring multiple mutexes
/// of the same class.
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::{WwClass, WwAcquireCtx, WwMutex};
/// use kernel::c_str;
/// use pin_init::stack_pin_init;
/// use kernel::alloc::KBox;
///
/// stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("my_class")));
///
/// // Create mutexes
/// stack_pin_init!(let mutex1 = WwMutex::new(1, &class));
/// stack_pin_init!(let mutex2 = WwMutex::new(2, &class));
///
/// // Create acquire context for deadlock avoidance
/// let mut ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL).unwrap();
///
/// // Acquire multiple locks safely
/// let guard1 = mutex1.lock(Some(&ctx)).unwrap();
/// let guard2 = mutex2.lock(Some(&ctx)).unwrap();
///
/// // Mark acquisition phase as complete
/// ctx.as_mut().done();
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data(PinnedDrop)]
pub struct WwAcquireCtx<'a> {
    #[pin]
    inner: Opaque<bindings::ww_acquire_ctx>,
    _p: PhantomData<&'a WwClass>,
}

// SAFETY: Used in controlled ways during lock acquisition. No race risk.
unsafe impl Sync for WwAcquireCtx<'_> {}
// SAFETY: Doesn't rely on thread-local state. Safe to move between threads.
unsafe impl Send for WwAcquireCtx<'_> {}

impl<'ctx> WwAcquireCtx<'ctx> {
    /// Initializes `Self` with calling C side `ww_acquire_init` inside.
    pub fn new<'class: 'ctx>(ww_class: &'class WwClass) -> impl PinInit<Self> {
        let raw_ptr = ww_class.inner.get();
        pin_init!(WwAcquireCtx {
            inner <- Opaque::ffi_init(|slot: *mut bindings::ww_acquire_ctx| {
                // SAFETY: The caller guarantees that `ww_class` remains valid.
                unsafe { bindings::ww_acquire_init(slot, raw_ptr) }
            }),
            _p: PhantomData
        })
    }

    /// Marks the end of the acquire phase with C side `ww_acquire_done`.
    ///
    /// After calling this function, no more mutexes can be acquired with this context.
    pub fn done(self: Pin<&mut Self>) {
        // SAFETY: The context is pinned and valid.
        unsafe { bindings::ww_acquire_done(self.inner.get()) };
    }

    /// Returns a raw pointer to the inner `ww_acquire_ctx`.
    fn as_ptr(&self) -> *mut bindings::ww_acquire_ctx {
        self.inner.get()
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
/// use kernel::sync::lock::ww_mutex::{WwClass, WwMutex};
/// use kernel::c_str;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("buffer_class")));
/// stack_pin_init!(let mutex = WwMutex::new(42, &class));
///
/// // Simple lock without context
/// let guard = mutex.lock(None).unwrap();
/// assert_eq!(*guard, 42);
///
/// # Ok::<(), Error>(())
/// ```
///
/// ## Multiple Locks
///
/// ```
/// use kernel::sync::lock::ww_mutex::{WwClass, WwAcquireCtx, WwMutex};
/// use kernel::alloc::KBox;
/// use kernel::c_str;
/// use kernel::error::code::EDEADLK;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let class = WwClass::new_wait_die(c_str!("resource_class")));
/// stack_pin_init!(let mutex_a = WwMutex::new("Resource A", &class));
/// stack_pin_init!(let mutex_b = WwMutex::new("Resource B", &class));
///
/// let mut ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL).unwrap();
///
/// // Try to acquire both locks
/// let guard_a = match mutex_a.lock(Some(&ctx)) {
///     Ok(guard) => guard,
///     Err(e) if e == EDEADLK => {
///         // Deadlock detected, use slow path
///         mutex_a.lock_slow(&ctx).unwrap()
///     }
///     Err(e) => return Err(e),
/// };
///
/// let guard_b = mutex_b.lock(Some(&ctx)).unwrap();
/// ctx.as_mut().done();
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

// SAFETY: `WwMutex` can be shared between threads.
unsafe impl<T: ?Sized + Send> Send for WwMutex<'_, T> {}
// SAFETY: `WwMutex` can be safely accessed from multiple threads concurrently.
unsafe impl<T: ?Sized + Sync> Sync for WwMutex<'_, T> {}

impl<'ww_class, T> WwMutex<'ww_class, T> {
    /// Creates `Self` with calling `ww_mutex_init` inside.
    pub fn new(t: T, ww_class: &'ww_class WwClass) -> impl PinInit<Self> {
        let raw_ptr = ww_class.inner.get();
        pin_init!(WwMutex {
            mutex <- Opaque::ffi_init(|slot: *mut bindings::ww_mutex| {
                // SAFETY: The caller guarantees that `ww_class` remains valid.
                unsafe { bindings::ww_mutex_init(slot, raw_ptr) }
            }),
            data: UnsafeCell::new(t),
            _p: PhantomData,
        })
    }
}

impl<T: ?Sized> WwMutex<'_, T> {
    /// Locks the mutex with the given acquire context.
    pub fn lock<'a>(&'a self, ctx: Option<&WwAcquireCtx<'_>>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe {
            bindings::ww_mutex_lock(
                self.mutex.get(),
                ctx.map_or(core::ptr::null_mut(), |c| c.as_ptr()),
            )
        };

        to_result(ret)?;

        Ok(WwMutexGuard::new(self))
    }

    /// Locks the mutex with the given acquire context, interruptible.
    ///
    /// Similar to `lock`, but can be interrupted by signals.
    pub fn lock_interruptible<'a>(
        &'a self,
        ctx: Option<&WwAcquireCtx<'_>>,
    ) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe {
            bindings::ww_mutex_lock_interruptible(
                self.mutex.get(),
                ctx.map_or(core::ptr::null_mut(), |c| c.as_ptr()),
            )
        };

        to_result(ret)?;

        Ok(WwMutexGuard::new(self))
    }

    /// Locks the mutex in the slow path after a die case.
    ///
    /// This should be called after releasing all held mutexes when `lock` returns `EDEADLK`.
    pub fn lock_slow<'a>(&'a self, ctx: &WwAcquireCtx<'_>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid, and we're in the slow path.
        unsafe { bindings::ww_mutex_lock_slow(self.mutex.get(), ctx.as_ptr()) };

        Ok(WwMutexGuard::new(self))
    }

    /// Locks the mutex in the slow path after a die case, interruptible.
    pub fn lock_slow_interruptible<'a>(
        &'a self,
        ctx: &WwAcquireCtx<'_>,
    ) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid, and we are in the slow path.
        let ret =
            unsafe { bindings::ww_mutex_lock_slow_interruptible(self.mutex.get(), ctx.as_ptr()) };

        to_result(ret)?;

        Ok(WwMutexGuard::new(self))
    }

    /// Tries to lock the mutex without blocking.
    pub fn try_lock<'a>(&'a self, ctx: Option<&WwAcquireCtx<'_>>) -> Result<WwMutexGuard<'a, T>> {
        // SAFETY: The mutex is pinned and valid.
        let ret = unsafe {
            bindings::ww_mutex_trylock(
                self.mutex.get(),
                ctx.map_or(core::ptr::null_mut(), |c| c.as_ptr()),
            )
        };

        if ret == 0 {
            return Err(EBUSY);
        }

        to_result(if ret < 0 { ret } else { 0 })?;

        Ok(WwMutexGuard::new(self))
    }

    /// Checks if the mutex is currently locked.
    pub fn is_locked(&self) -> bool {
        // SAFETY: The mutex is pinned and valid.
        unsafe { bindings::ww_mutex_is_locked(self.mutex.get()) }
    }

    /// Returns a raw pointer to the inner mutex.
    fn as_ptr(&self) -> *mut bindings::ww_mutex {
        self.mutex.get()
    }
}

/// A guard that provides exclusive access to the data protected
/// by a [`WwMutex`].
///
/// # Invariants
///
/// The guard holds an exclusive lock on the associated `WwMutex`. The lock is held
/// for the entire lifetime of this guard and is automatically released when the
/// guard is dropped.
#[must_use = "the lock unlocks immediately when the guard is unused"]
pub struct WwMutexGuard<'a, T: ?Sized> {
    mutex: &'a WwMutex<'a, T>,
    _not_send: NotThreadSafe,
}

// SAFETY: `WwMutexGuard` can be transferred across thread boundaries if the data can.
unsafe impl<T: ?Sized + Send> Send for WwMutexGuard<'_, T> {}

// SAFETY: `WwMutexGuard` can be shared between threads if the data can.
unsafe impl<T: ?Sized + Send + Sync> Sync for WwMutexGuard<'_, T> {}

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
    use crate::alloc::KBox;
    use crate::c_str;
    use crate::prelude::*;
    use pin_init::stack_pin_init;

    use super::*;

    // A simple coverage on `define_ww_class` macro.
    define_ww_class!(TEST_WOUND_WAIT_CLASS, wound_wait, c_str!("test_wound_wait"));
    define_ww_class!(TEST_WAIT_DIE_CLASS, wait_die, c_str!("test_wait_die"));

    #[test]
    fn test_ww_mutex_basic_lock_unlock() {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("test_mutex_class")));

        stack_pin_init!(let mutex = WwMutex::new(42, &class));

        // Lock without context
        let guard = mutex.lock(None).unwrap();
        assert_eq!(*guard, 42);

        // Drop the lock
        drop(guard);

        // Lock it again
        let mut guard = mutex.lock(None).unwrap();
        *guard = 100;
        assert_eq!(*guard, 100);
    }

    #[test]
    fn test_ww_mutex_trylock() {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("trylock_class")));

        stack_pin_init!(let mutex = WwMutex::new(123, &class));

        // trylock on unlocked mutex should succeed
        let guard = mutex.try_lock(None).unwrap();
        assert_eq!(*guard, 123);
        drop(guard);

        // lock it first
        let _guard1 = mutex.lock(None).unwrap();

        // trylock should fail with EBUSY when already locked
        let result = mutex.try_lock(None);
        match result {
            Err(e) => assert_eq!(e, EBUSY),
            Ok(_) => panic!("Expected `EBUSY` but got success"),
        }
    }

    #[test]
    fn test_ww_mutex_is_locked() {
        stack_pin_init!(let class = WwClass::new_wait_die(c_str!("locked_check_class")));

        stack_pin_init!(let mutex = WwMutex::new("hello", &class));

        // should not be locked initially
        assert!(!mutex.is_locked());

        let guard = mutex.lock(None).unwrap();
        assert!(mutex.is_locked());

        drop(guard);
        assert!(!mutex.is_locked());
    }

    #[test]
    fn test_ww_acquire_context() {
        stack_pin_init!(let class = WwClass::new_wound_wait(c_str!("ctx_class")));

        stack_pin_init!(let mutex1 = WwMutex::new(1, &class));
        stack_pin_init!(let mutex2 = WwMutex::new(2, &class));

        let mut ctx = KBox::pin_init(WwAcquireCtx::new(&class), GFP_KERNEL).unwrap();

        // acquire multiple mutexes with same context
        let guard1 = mutex1.lock(Some(&ctx)).unwrap();
        let guard2 = mutex2.lock(Some(&ctx)).unwrap();

        assert_eq!(*guard1, 1);
        assert_eq!(*guard2, 2);

        ctx.as_mut().done();

        // we shouldn't be able to lock once it's `done`.
        assert!(mutex1.lock(Some(&ctx)).is_err());
        assert!(mutex2.lock(Some(&ctx)).is_err());
    }

    #[test]
    fn test_with_global_classes() {
        stack_pin_init!(let wound_wait_mutex = WwMutex::new(100, &TEST_WOUND_WAIT_CLASS));
        stack_pin_init!(let wait_die_mutex = WwMutex::new(200, &TEST_WAIT_DIE_CLASS));

        let ww_guard = wound_wait_mutex.lock(None).unwrap();
        let wd_guard = wait_die_mutex.lock(None).unwrap();

        assert_eq!(*ww_guard, 100);
        assert_eq!(*wd_guard, 200);

        assert!(wound_wait_mutex.is_locked());
        assert!(wait_die_mutex.is_locked());

        drop(ww_guard);
        drop(wd_guard);

        assert!(!wound_wait_mutex.is_locked());
        assert!(!wait_die_mutex.is_locked());
    }
}
