// SPDX-License-Identifier: GPL-2.0

//! Provides [`AcquireCtx`] for managing multiple wound/wait
//! mutexes from the same [`Class`].

use crate::bindings;
use crate::prelude::*;
use crate::types::Opaque;

use core::marker::PhantomData;

use super::{lock_common, Class, LockKind, Mutex, MutexGuard};

/// Groups multiple [`Mutex`]es for deadlock avoidance when acquired
/// with the same [`Class`].
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::{Class, AcquireCtx, Mutex};
/// use kernel::define_ww_class;
/// use kernel::sync::Arc;
/// use pin_init::stack_pin_init;
///
/// define_ww_class!(SOME_WW_CLASS);
///
/// // Create mutexes.
/// let mutex1 = Arc::pin_init(Mutex::new(1, &SOME_WW_CLASS), GFP_KERNEL)?;
/// let mutex2 = Arc::pin_init(Mutex::new(2, &SOME_WW_CLASS), GFP_KERNEL)?;
///
/// // Create acquire context for deadlock avoidance.
/// let ctx = KBox::pin_init(AcquireCtx::new(&SOME_WW_CLASS), GFP_KERNEL)?;
///
/// let guard1 = ctx.lock(&mutex1)?;
/// let guard2 = ctx.lock(&mutex2)?;
///
/// // Mark acquisition phase as complete.
/// // SAFETY: It's called exactly once here and nowhere else.
/// unsafe { ctx.done() };
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data(PinnedDrop)]
#[repr(transparent)]
pub struct AcquireCtx<'a> {
    #[pin]
    pub(super) inner: Opaque<bindings::ww_acquire_ctx>,
    _p: PhantomData<&'a Class>,
}

impl<'class> AcquireCtx<'class> {
    /// Initializes a new [`AcquireCtx`] with the given [`Class`].
    pub fn new(class: &'class Class) -> impl PinInit<Self> {
        let class_ptr = class.inner.get();
        pin_init!(AcquireCtx {
            inner <- Opaque::ffi_init(|slot: *mut bindings::ww_acquire_ctx| {
                // SAFETY: `class` is valid for the lifetime `'class` captured
                // by `AcquireCtx`.
                unsafe { bindings::ww_acquire_init(slot, class_ptr) }
            }),
            _p: PhantomData
        })
    }

    /// Creates a [`AcquireCtx`] from a raw pointer.
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is a valid pointer to the `inner` field
    /// of [`AcquireCtx`] and that it remains valid for the lifetime `'a`.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::ww_acquire_ctx) -> &'a Self {
        // SAFETY: By the safety contract, `ptr` is valid to construct `AcquireCtx`.
        unsafe { &*ptr.cast() }
    }

    /// Marks the end of the acquire phase.
    ///
    /// Calling this function is optional. It is just useful to document
    /// the code and clearly designated the acquire phase from actually
    /// using the locked data structures.
    ///
    /// After calling this function, no more mutexes can be acquired with
    /// this context.
    ///
    /// # Safety
    ///
    /// The caller must ensure that this function is called only once
    /// and after calling it, no further mutexes are acquired using
    /// this context.
    pub unsafe fn done(&self) {
        // SAFETY: By the safety contract, the caller guarantees that this
        // function is called only once.
        unsafe { bindings::ww_acquire_done(self.inner.get()) };
    }

    /// Re-initializes the [`AcquireCtx`].
    ///
    /// Must be called after releasing all locks when [`EDEADLK`] occurs.
    ///
    /// # Safety
    ///
    /// The caller must ensure no locks are held in this [`AcquireCtx`].
    pub unsafe fn reinit(self: Pin<&mut Self>) {
        let ctx = self.inner.get();

        // SAFETY: `ww_class` is always a valid pointer in properly initialized
        // `AcquireCtx`.
        let class_ptr = unsafe { (*ctx).ww_class };

        // SAFETY:
        //  - Lifetime of any guard (which hold an immutable borrow of `self`) cannot overlap
        //    with the execution of this function. This enforces that all locks acquired via
        //    this context have been released.
        //
        //  - `ctx` is guaranteed to be initialized because `ww_acquire_fini`
        //    can only be called from the `Drop` implementation.
        //
        //  - `ww_acquire_fini` is safe to call on an initialized context.
        unsafe { bindings::ww_acquire_fini(ctx) };

        // SAFETY: `ww_acquire_init` is safe to call with valid pointers
        // to initialize an uninitialized context.
        unsafe { bindings::ww_acquire_init(ctx, class_ptr) };
    }

    /// Locks the given [`Mutex`] on this [`AcquireCtx`].
    pub fn lock<'a, T>(&'a self, mutex: &'a Mutex<'a, T>) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Regular)
    }

    /// Similar to [`Self::lock`], but can be interrupted by signals.
    pub fn lock_interruptible<'a, T>(
        &'a self,
        mutex: &'a Mutex<'a, T>,
    ) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Interruptible)
    }

    /// Locks the given [`Mutex`] on this [`AcquireCtx`] using the slow path.
    ///
    /// This function should be used when [`Self::lock`] fails (typically due
    /// to a potential deadlock).
    pub fn lock_slow<'a, T>(&'a self, mutex: &'a Mutex<'a, T>) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Slow)
    }

    /// Similar to [`Self::lock_slow`], but can be interrupted by signals.
    pub fn lock_slow_interruptible<'a, T>(
        &'a self,
        mutex: &'a Mutex<'a, T>,
    ) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::SlowInterruptible)
    }

    /// Tries to lock the [`Mutex`] on this [`AcquireCtx`] without blocking.
    ///
    /// Unlike [`Self::lock`], no deadlock handling is performed.
    pub fn try_lock<'a, T>(&'a self, mutex: &'a Mutex<'a, T>) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Try)
    }
}

#[pinned_drop]
impl PinnedDrop for AcquireCtx<'_> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: Given the lifetime bounds we know no locks are held,
        // so calling `ww_acquire_fini` is safe.
        unsafe { bindings::ww_acquire_fini(self.inner.get()) };
    }
}
