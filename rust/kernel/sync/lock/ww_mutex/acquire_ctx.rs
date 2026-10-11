// SPDX-License-Identifier: GPL-2.0

//! Provides [`AcquireCtx`] for managing multiple wound/wait
//! mutexes from the same [`Class`].

use super::{
    lock_common,
    Class,
    LockKind,
    Mutex,
    MutexGuard, //
};
use crate::{
    bindings,
    prelude::*,
    types::Opaque, //
};
use core::marker::PhantomData;

/// Groups multiple [`Mutex`]es for deadlock avoidance when acquired
/// with the same [`Class`].
///
/// # Examples
///
/// ```
/// use kernel::{
///     define_ww_class,
///     sync::{
///         lock::ww_mutex::{
///             AcquireCtx,
///             Class,
///             Mutex, //
///         },
///         Arc,
///     },
/// };
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
/// // SAFETY: The guard is dropped before `ctx`.
/// let guard1 = unsafe { ctx.lock(&mutex1) }?;
/// // SAFETY: The guard is dropped before `ctx`.
/// let guard2 = unsafe { ctx.lock(&mutex2) }?;
///
/// // Mark acquisition phase as complete.
/// ctx.done();
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
    pub fn done(&self) {
        // SAFETY: `self.inner` contains a valid, initialized acquire context.
        unsafe { bindings::ww_acquire_done(self.inner.get()) };
    }

    /// Locks the given [`Mutex`] on this [`AcquireCtx`].
    ///
    /// # Safety
    ///
    /// On success, the caller must keep this context valid until the mutex is unlocked,
    /// even if the returned guard is forgotten. The context must not be finalized or
    /// reinitialized while the mutex remains locked.
    pub unsafe fn lock<'a, T>(&'a self, mutex: &'a Mutex<'a, T>) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Regular)
    }

    /// Similar to [`Self::lock`], but can be interrupted by signals.
    ///
    /// # Safety
    ///
    /// The same requirements as [`Self::lock`] apply.
    pub unsafe fn lock_interruptible<'a, T>(
        &'a self,
        mutex: &'a Mutex<'a, T>,
    ) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Interruptible)
    }

    /// Tries to lock the [`Mutex`] on this [`AcquireCtx`] without blocking.
    ///
    /// Unlike [`Self::lock`], no deadlock handling is performed.
    ///
    /// # Safety
    ///
    /// The same requirements as [`Self::lock`] apply.
    pub unsafe fn try_lock<'a, T>(&'a self, mutex: &'a Mutex<'a, T>) -> Result<MutexGuard<'a, T>> {
        lock_common(mutex, Some(self), LockKind::Try)
    }
}

#[pinned_drop]
impl PinnedDrop for AcquireCtx<'_> {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: The locking methods require callers to release all acquired locks
        // before this context is dropped.
        unsafe { bindings::ww_acquire_fini(self.inner.get()) };
    }
}
