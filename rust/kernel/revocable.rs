// SPDX-License-Identifier: GPL-2.0

//! Revocable objects.
//!
//! The [`Revocable`] type wraps other types and allows access to them to be revoked. The existence
//! of a [`RevocableGuard`] ensures that objects remain valid.

use crate::{bindings, prelude::*, sync::rcu, types::Opaque};
use core::{
    marker::PhantomData,
    ops::Deref,
    ptr::drop_in_place,
    sync::atomic::{AtomicBool, Ordering},
};

/// An object that can become inaccessible at runtime.
///
/// Once access is revoked and all concurrent users complete (i.e., all existing instances of
/// [`RevocableGuard`] are dropped), the wrapped object is also dropped.
///
/// # Examples
///
/// ```
/// # use kernel::revocable::Revocable;
///
/// struct Example {
///     a: u32,
///     b: u32,
/// }
///
/// fn add_two(v: &Revocable<Example>) -> Option<u32> {
///     let guard = v.try_access()?;
///     Some(guard.a + guard.b)
/// }
///
/// let v = KBox::pin_init(Revocable::new(Example { a: 10, b: 20 }), GFP_KERNEL).unwrap();
/// assert_eq!(add_two(&v), Some(30));
/// v.revoke();
/// assert_eq!(add_two(&v), None);
/// ```
///
/// Sample example as above, but explicitly using the rcu read side lock.
///
/// ```
/// # use kernel::revocable::Revocable;
/// use kernel::sync::rcu;
///
/// struct Example {
///     a: u32,
///     b: u32,
/// }
///
/// fn add_two(v: &Revocable<Example>) -> Option<u32> {
///     let guard = rcu::read_lock();
///     let e = v.try_access_with_guard(&guard)?;
///     Some(e.a + e.b)
/// }
///
/// let v = KBox::pin_init(Revocable::new(Example { a: 10, b: 20 }), GFP_KERNEL).unwrap();
/// assert_eq!(add_two(&v), Some(30));
/// v.revoke();
/// assert_eq!(add_two(&v), None);
/// ```
///
/// # Invariants
///
/// - `data` is valid for reads in two cases:
///   - while `is_available` is true, or
///   - while the RCU read-side lock is taken and it was acquired while `is_available` was `true`.
/// - `data` is valid for writes when `is_available` was atomically changed from `true` to `false`
///   and no thread that has access to `data` is holding an RCU read-side lock that was acquired
///   prior to the change in `is_available`.
#[pin_data(PinnedDrop)]
pub struct Revocable<T> {
    is_available: AtomicBool,
    #[pin]
    data: Opaque<T>,
}

// SAFETY: `Revocable` is `Send` if the wrapped object is also `Send`. This is because while the
// functionality exposed by `Revocable` can be accessed from any thread/CPU, it is possible that
// this isn't supported by the wrapped object.
unsafe impl<T: Send> Send for Revocable<T> {}

// SAFETY: `Revocable` is `Sync` if the wrapped object is both `Send` and `Sync`. We require `Send`
// from the wrapped object as well because  of `Revocable::revoke`, which can trigger the `Drop`
// implementation of the wrapped object from an arbitrary thread.
unsafe impl<T: Sync + Send> Sync for Revocable<T> {}

impl<T> Revocable<T> {
    /// Creates a new revocable instance of the given data.
    pub fn new(data: impl PinInit<T>) -> impl PinInit<Self> {
        pin_init!(Self {
            is_available: AtomicBool::new(true),
            data <- Opaque::pin_init(data),
        })
    }

    /// Tries to access the revocable wrapped object.
    ///
    /// Returns `None` if the object has been revoked and is therefore no longer accessible.
    ///
    /// Returns a guard that gives access to the object otherwise; the object is guaranteed to
    /// remain accessible while the guard is alive. In such cases, callers are not allowed to sleep
    /// because another CPU may be waiting to complete the revocation of this object.
    pub fn try_access(&self) -> Option<RevocableGuard<'_, T>> {
        let guard = rcu::read_lock();
        if self.is_available.load(Ordering::Relaxed) {
            // Since `self.is_available` is true, data is initialised and has to remain valid
            // because the RCU read side lock prevents it from being dropped.
            Some(RevocableGuard::new(self.data.get(), guard))
        } else {
            None
        }
    }

    /// Tries to access the revocable wrapped object.
    ///
    /// Returns `None` if the object has been revoked and is therefore no longer accessible.
    ///
    /// Returns a shared reference to the object otherwise; the object is guaranteed to
    /// remain accessible while the rcu read side guard is alive. In such cases, callers are not
    /// allowed to sleep because another CPU may be waiting to complete the revocation of this
    /// object.
    pub fn try_access_with_guard<'a>(&'a self, _guard: &'a rcu::Guard) -> Option<&'a T> {
        if self.is_available.load(Ordering::Relaxed) {
            // SAFETY: `self.data` is valid for reads because of `Self`'s type invariants,
            // as `self.is_available` is true and `_guard` holds the RCU read-side lock.
            Some(unsafe { &*self.data.get() })
        } else {
            None
        }
    }

    /// Tries to access the wrapped object and run a closure on it while the guard is held.
    ///
    /// This is a convenience method to run short non-sleepable code blocks while ensuring the
    /// guard is dropped afterwards. [`Self::try_access`] carries the risk that the caller will
    /// forget to explicitly drop that returned guard before calling sleepable code; this method
    /// adds an extra safety to make sure it doesn't happen.
    ///
    /// Returns [`None`] if the object has been revoked and is therefore no longer accessible, or
    /// the result of the closure wrapped in [`Some`]. If the closure returns a [`Result`] then the
    /// return type becomes `Option<Result<>>`, which can be inconvenient. Users are encouraged to
    /// define their own macro that turns the [`Option`] into a proper error code and flattens the
    /// inner result into it if it makes sense within their subsystem.
    pub fn try_access_with<R, F: FnOnce(&T) -> R>(&self, f: F) -> Option<R> {
        self.try_access().map(|t| f(&*t))
    }

    /// Directly access the revocable wrapped object.
    ///
    /// # Safety
    ///
    /// The caller must ensure this [`Revocable`] instance hasn't been revoked and won't be revoked
    /// as long as the returned `&T` lives.
    pub unsafe fn access(&self) -> &T {
        // SAFETY: By the safety requirement of this function it is guaranteed that
        // `self.data.get()` is a valid pointer to an instance of `T`.
        unsafe { &*self.data.get() }
    }

    /// Revokes access to and drops the wrapped object.
    ///
    /// Access to the object is revoked immediately to new callers of [`Revocable::try_access`],
    /// expecting that there are no concurrent users of the object.
    ///
    /// Returns `true` if `&self` has been revoked with this call, `false` if it was revoked
    /// already.
    ///
    /// # Safety
    ///
    /// Callers must ensure that there are no more concurrent users of the revocable object.
    pub unsafe fn revoke_nosync(&self) -> bool {
        let revoke = self.is_available.swap(false, Ordering::Relaxed);

        if revoke {
            // SAFETY: `self.data` is valid for writes because of `Self`'s type invariants,
            // as `self.is_available` is false due to the atomic swap, and by the safety
            // requirements of this function, no thread is accessing `data` anymore.
            unsafe { drop_in_place(self.data.get()) };
        }
        revoke
    }

    /// Revokes access to and drops the wrapped object.
    ///
    /// Access to the object is revoked immediately to new callers of [`Revocable::try_access`].
    ///
    /// If there are concurrent users of the object (i.e., ones that called
    /// [`Revocable::try_access`] beforehand and still haven't dropped the returned guard), this
    /// function waits for the concurrent access to complete before dropping the wrapped object.
    ///
    /// Returns `true` if `&self` has been revoked with this call, `false` if it was revoked
    /// already.
    pub fn revoke(&self) -> bool {
        let revoke = self.is_available.swap(false, Ordering::Relaxed);

        if revoke {
            // SAFETY: Just an FFI call, there are no further requirements.
            unsafe { bindings::synchronize_rcu() };

            // SAFETY: `self.data` is valid for writes because of `Self`'s type invariants,
            // as `self.is_available` is false due to the atomic swap, and `synchronize_rcu`
            // ensures all prior RCU read-side critical sections have completed.
            unsafe { drop_in_place(self.data.get()) };
        }
        revoke
    }
}

#[pinned_drop]
impl<T> PinnedDrop for Revocable<T> {
    fn drop(self: Pin<&mut Self>) {
        // Drop only if the data hasn't been revoked yet (in which case it has already been
        // dropped).
        // SAFETY: We are not moving out of `p`, only dropping in place
        let p = unsafe { self.get_unchecked_mut() };
        if *p.is_available.get_mut() {
            // SAFETY:
            // - `self.data` is valid for writes because of `Self`'s type invariants:
            //   `&mut Self` guarantees exclusive access, so no other thread can concurrently access `data`.
            // - this function is a drop function, thus this code is at most executed once.
            unsafe { drop_in_place(p.data.get()) };
        }
    }
}

/// A guard that allows access to a revocable object and keeps it alive.
///
/// CPUs may not sleep while holding on to [`RevocableGuard`] because it's in atomic context
/// holding the RCU read-side lock.
///
/// # Invariants
///
/// The RCU read-side lock is held while the guard is alive.
pub struct RevocableGuard<'a, T> {
    // This can't use the `&'a T` type because references that appear in function arguments must
    // not become dangling during the execution of the function, which can happen if the
    // `RevocableGuard` is passed as a function argument and then dropped during execution of the
    // function.
    data_ref: *const T,
    _rcu_guard: rcu::Guard,
    _p: PhantomData<&'a ()>,
}

impl<T> RevocableGuard<'_, T> {
    fn new(data_ref: *const T, rcu_guard: rcu::Guard) -> Self {
        Self {
            data_ref,
            _rcu_guard: rcu_guard,
            _p: PhantomData,
        }
    }
}

impl<T> Deref for RevocableGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        // SAFETY: By the type invariants, we hold the rcu read-side lock, so the object is
        // guaranteed to remain valid.
        unsafe { &*self.data_ref }
    }
}
