// SPDX-License-Identifier: GPL-2.0

//! A kernel spinlock.
//!
//! This module allows Rust code to use the kernel's `spinlock_t`.
use crate::interrupt::LocalInterruptDisabled;

/// Creates a [`SpinLock`] initialiser with the given name and a newly-created lock class.
///
/// It uses the name if one is given, otherwise it generates one based on the file name and line
/// number.
#[macro_export]
macro_rules! new_spinlock {
    ($inner:expr $(, $name:literal)? $(,)?) => {
        $crate::sync::SpinLock::new(
            $inner, $crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}
pub use new_spinlock;

/// A spinlock.
///
/// Exposes the kernel's [`spinlock_t`]. When multiple CPUs attempt to lock the same spinlock, only
/// one at a time is allowed to progress, the others will block (spinning) until the spinlock is
/// unlocked, at which point another CPU will be allowed to make progress.
///
/// Instances of [`SpinLock`] need a lock class and to be pinned. The recommended way to create such
/// instances is with the [`pin_init`](crate::pin_init) and [`new_spinlock`] macros.
///
/// # Examples
///
/// The following example shows how to declare, allocate and initialise a struct (`Example`) that
/// contains an inner struct (`Inner`) that is protected by a spinlock.
///
/// ```
/// use kernel::sync::{new_spinlock, SpinLock};
///
/// struct Inner {
///     a: u32,
///     b: u32,
/// }
///
/// #[pin_data]
/// struct Example {
///     c: u32,
///     #[pin]
///     d: SpinLock<Inner>,
/// }
///
/// impl Example {
///     fn new() -> impl PinInit<Self> {
///         pin_init!(Self {
///             c: 10,
///             d <- new_spinlock!(Inner { a: 20, b: 30 }),
///         })
///     }
/// }
///
/// // Allocate a boxed `Example`.
/// let e = KBox::pin_init(Example::new(), GFP_KERNEL)?;
/// assert_eq!(e.c, 10);
/// assert_eq!(e.d.lock().a, 20);
/// assert_eq!(e.d.lock().b, 30);
/// # Ok::<(), Error>(())
/// ```
///
/// The following example shows how to use interior mutability to modify the contents of a struct
/// protected by a spinlock despite only having a shared reference:
///
/// ```
/// use kernel::sync::SpinLock;
///
/// struct Example {
///     a: u32,
///     b: u32,
/// }
///
/// fn example(m: &SpinLock<Example>) {
///     let mut guard = m.lock();
///     guard.a += 10;
///     guard.b += 20;
/// }
/// ```
///
/// [`spinlock_t`]: srctree/include/linux/spinlock.h
pub type SpinLock<T> = super::Lock<T, SpinLockBackend>;

/// A kernel `spinlock_t` lock backend.
pub struct SpinLockBackend;

/// A [`Guard`] acquired from locking a [`SpinLock`].
///
/// This is simply a type alias for a [`Guard`] returned from locking a [`SpinLock`]. It will unlock
/// the [`SpinLock`] upon being dropped.
///
/// [`Guard`]: super::Guard
pub type SpinLockGuard<'a, T> = super::Guard<'a, T, SpinLockBackend>;

// SAFETY: The underlying kernel `spinlock_t` object ensures mutual exclusion. `relock` uses the
// default implementation that always calls the same locking method.
unsafe impl super::Backend for SpinLockBackend {
    type State = bindings::spinlock_t;
    type GuardState = ();
    type Context<'a> = ();

    unsafe fn init(
        ptr: *mut Self::State,
        name: *const crate::ffi::c_char,
        key: *mut bindings::lock_class_key,
    ) {
        // SAFETY: The safety requirements ensure that `ptr` is valid for writes, and `name` and
        // `key` are valid for read indefinitely.
        unsafe { bindings::__spin_lock_init(ptr, name, key) }
    }

    unsafe fn lock(ptr: *mut Self::State) -> Self::GuardState {
        // SAFETY: The safety requirements of this function ensure that `ptr` points to valid
        // memory, and that it has been initialised before.
        unsafe { bindings::spin_lock(ptr) }
    }

    unsafe fn unlock(ptr: *mut Self::State, _guard_state: &Self::GuardState) {
        // SAFETY: The safety requirements of this function ensure that `ptr` is valid and that the
        // caller is the owner of the spinlock.
        unsafe { bindings::spin_unlock(ptr) }
    }

    unsafe fn try_lock(ptr: *mut Self::State) -> Option<Self::GuardState> {
        // SAFETY: The `ptr` pointer is guaranteed to be valid and initialized before use.
        let result = unsafe { bindings::spin_trylock(ptr) };

        if result != 0 {
            Some(())
        } else {
            None
        }
    }

    unsafe fn assert_is_held(ptr: *mut Self::State) {
        // SAFETY: The `ptr` pointer is guaranteed to be valid and initialized before use.
        unsafe { bindings::spin_assert_is_held(ptr) }
    }
}

/// Creates a [`SpinLockIrq`] initialiser with the given name and a newly-created lock class.
///
/// It uses the name if one is given, otherwise it generates one based on the file name and line
/// number.
#[macro_export]
macro_rules! new_spinlock_irq {
    ($inner:expr $(, $name:literal)? $(,)?) => {
        $crate::sync::SpinLockIrq::new(
            $inner, $crate::optional_name!($($name)?), $crate::static_lock_class!())
    };
}
pub use new_spinlock_irq;

/// A spinlock that may be acquired when local processor interrupts are disabled.
///
/// This is a version of [`SpinLock`] that can only be used in contexts where interrupts for the
/// local CPU are disabled. It can be acquired in two ways:
///
/// - Using [`lock()`] like any other type of lock, in which case the bindings will ensure that
///   interrupts remain disabled for at least as long as the [`SpinLockIrqGuard`] exists.
/// - Using [`lock_with()`] in contexts where a [`LocalInterruptDisabled`] token is present and
///   local processor interrupts are already known to be disabled, in which case the local interrupt
///   state will not be touched. This method should be preferred if a [`LocalInterruptDisabled`]
///   token is present in the scope.
///
/// For more info on spinlocks, see [`SpinLock`]. For more information on interrupts,
/// [see the interrupt module](kernel::interrupt).
///
/// # Examples
///
/// The following example shows how to declare, allocate initialise and access a struct (`Example`)
/// that contains an inner struct (`Inner`) that is protected by a spinlock that requires local
/// processor interrupts to be disabled.
///
/// ```
/// use kernel::sync::{new_spinlock_irq, SpinLockIrq};
///
/// struct Inner {
///     a: u32,
///     b: u32,
/// }
///
/// #[pin_data]
/// struct Example {
///     #[pin]
///     c: SpinLockIrq<Inner>,
///     #[pin]
///     d: SpinLockIrq<Inner>,
/// }
///
/// impl Example {
///     fn new() -> impl PinInit<Self> {
///         pin_init!(Self {
///             c <- new_spinlock_irq!(Inner { a: 0, b: 10 }),
///             d <- new_spinlock_irq!(Inner { a: 20, b: 30 }),
///         })
///     }
/// }
///
/// // Allocate a boxed `Example`
/// let e = KBox::pin_init(Example::new(), GFP_KERNEL)?;
///
/// // Accessing an `Example` from a context where interrupts may not be disabled already.
/// let c_guard = e.c.lock(); // interrupts are disabled now, +1 interrupt disable refcount
/// let d_guard = e.d.lock(); // no interrupt state change, +1 interrupt disable refcount
///
/// assert_eq!(c_guard.a, 0);
/// assert_eq!(c_guard.b, 10);
/// assert_eq!(d_guard.a, 20);
/// assert_eq!(d_guard.b, 30);
///
/// drop(c_guard); // Dropping c_guard will not re-enable interrupts just yet, since d_guard is
///                // still in scope.
/// drop(d_guard); // Last interrupt disable reference dropped here, so interrupts are re-enabled
///                // now
/// # Ok::<(), Error>(())
/// ```
///
/// [`lock()`]: SpinLockIrq::lock
/// [`lock_with()`]: SpinLockIrq::lock_with
pub type SpinLockIrq<T> = super::Lock<T, SpinLockIrqBackend>;

/// A kernel `spinlock_t` lock backend that is acquired in interrupt disabled contexts.
pub struct SpinLockIrqBackend;

/// A [`Guard`] acquired from locking a [`SpinLockIrq`] using [`lock()`].
///
/// This is simply a type alias for a [`Guard`] returned from locking a [`SpinLockIrq`] using
/// [`lock_with()`]. It will unlock the [`SpinLockIrq`] and decrement the local processor's
/// interrupt disablement refcount upon being dropped.
///
/// [`Guard`]: super::Guard
/// [`lock()`]: SpinLockIrq::lock
/// [`lock_with()`]: SpinLockIrq::lock_with
pub type SpinLockIrqGuard<'a, T> = super::Guard<'a, T, SpinLockIrqBackend>;

// SAFETY: The underlying kernel `spinlock_t` object ensures mutual exclusion. `relock` uses the
// default implementation that always calls the same locking method.
unsafe impl super::Backend for SpinLockIrqBackend {
    type State = bindings::spinlock_t;
    type GuardState = ();
    type Context<'a> = &'a LocalInterruptDisabled;

    unsafe fn init(
        ptr: *mut Self::State,
        name: *const crate::ffi::c_char,
        key: *mut bindings::lock_class_key,
    ) {
        // SAFETY: The safety requirements ensure that `ptr` is valid for writes, and `name` and
        // `key` are valid for read indefinitely.
        unsafe { bindings::__spin_lock_init(ptr, name, key) }
    }

    unsafe fn lock(ptr: *mut Self::State) -> Self::GuardState {
        // SAFETY: The safety requirements of this function ensure that `ptr` points to valid
        // memory, and that it has been initialised before.
        unsafe { bindings::spin_lock_irq_disable(ptr) }
    }

    unsafe fn unlock(ptr: *mut Self::State, _guard_state: &Self::GuardState) {
        // SAFETY: The safety requirements of this function ensure that `ptr` is valid and that the
        // caller is the owner of the spinlock.
        unsafe { bindings::spin_unlock_irq_enable(ptr) }
    }

    unsafe fn try_lock(ptr: *mut Self::State) -> Option<Self::GuardState> {
        // SAFETY: The `ptr` pointer is guaranteed to be valid and initialized before use.
        let result = unsafe { bindings::spin_trylock_irq_disable(ptr) };

        if result != 0 {
            Some(())
        } else {
            None
        }
    }

    unsafe fn assert_is_held(ptr: *mut Self::State) {
        // SAFETY: The `ptr` pointer is guaranteed to be valid and initialized before use.
        unsafe { bindings::spin_assert_is_held(ptr) }
    }
}
