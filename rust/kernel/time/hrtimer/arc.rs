// SPDX-License-Identifier: GPL-2.0

use super::HasHrTimer;
use super::HrTimer;
use super::HrTimerCallback;
use super::HrTimerCallbackContext;
use super::HrTimerHandle;
use super::HrTimerMode;
use super::HrTimerPointer;
use super::RawHrTimerCallback;
use crate::alloc::Flags;
use crate::error::{Error, Result};
use crate::init::InPlaceInit;
use crate::sync::{Arc, ArcBorrow, UniqueArc};
use core::pin::Pin;
use pin_init::PinInit;

/// A wrapper around [`Arc`] that's guaranteed unique.
///
/// The `HrTimerArc` type can be thought of as a special reference to a refcounted object that owns
/// the permission to arm the [`HrTimer`] stored in the refcounted object. By ensuring that each
/// object has only one `HrTimerArc` reference, the owner of that reference is assured exclusive
/// access to the arming operation. When a timer is started, the returned [`ArcHrTimerHandle`] takes
/// ownership of the `HrTimerArc` reference.
///
/// While this `HrTimerArc` is unique, there still might exist normal [`Arc`] references to the
/// object. Use [`HrTimerArc::clone_arc`] to obtain one.
///
/// # Invariants
///
/// * Each reference counted object has at most one `HrTimerArc`.
pub struct HrTimerArc<T>
where
    T: HasHrTimer<T>,
{
    arc: Arc<T>,
}

impl<T> HrTimerArc<T>
where
    T: HasHrTimer<T>,
{
    /// Use the given pin-initializer to pin-initialize a `T` inside of a new `HrTimerArc`.
    #[inline]
    pub fn pin_init<E>(init: impl PinInit<T, E>, flags: Flags) -> Result<Self>
    where
        Error: From<E>,
    {
        Ok(Self::from(UniqueArc::pin_init(init, flags)?))
    }

    /// Clone an [`Arc`] from this `HrTimerArc`.
    ///
    /// The returned [`Arc`] can be used to access the object, but not to arm its timer.
    #[inline]
    pub fn clone_arc(&self) -> Arc<T> {
        self.arc.clone()
    }
}

impl<T> From<Pin<UniqueArc<T>>> for HrTimerArc<T>
where
    T: HasHrTimer<T>,
{
    /// Convert a pinned [`UniqueArc`] into a [`HrTimerArc`].
    #[inline]
    fn from(unique: Pin<UniqueArc<T>>) -> Self {
        // INVARIANT: We have a `UniqueArc`, so there is no `HrTimerArc` for this object.
        Self {
            arc: Arc::from(unique),
        }
    }
}

impl<T> HrTimerPointer for HrTimerArc<T>
where
    T: 'static,
    T: Send + Sync,
    T: HasHrTimer<T>,
    T: for<'a> HrTimerCallback<Pointer<'a> = Self>,
{
    type TimerMode = <T as HasHrTimer<T>>::TimerMode;
    type TimerHandle = ArcHrTimerHandle<T>;

    fn start(
        self,
        expires: <<T as HasHrTimer<T>>::TimerMode as HrTimerMode>::Expires,
    ) -> ArcHrTimerHandle<T> {
        // SAFETY:
        //  - We keep `self` alive by wrapping it in a handle below.
        //  - Since we generate the pointer passed to `start` from a valid
        //    reference, it is a valid pointer.
        unsafe { T::start(Arc::as_ptr(&self.arc), expires) };
        ArcHrTimerHandle { inner: self }
    }
}

/// A handle for a [`HrTimerArc`] returned by a call to [`HrTimerPointer::start`].
///
/// This handle owns the [`HrTimerArc`] reference for the object, so the timer cannot be armed
/// again while this handle exists.
pub struct ArcHrTimerHandle<T>
where
    T: HasHrTimer<T>,
{
    pub(crate) inner: HrTimerArc<T>,
}

// SAFETY: We implement drop below, and we cancel the timer in the drop
// implementation.
unsafe impl<T> HrTimerHandle for ArcHrTimerHandle<T>
where
    T: HasHrTimer<T>,
{
    fn cancel(&mut self) -> bool {
        let self_ptr = Arc::as_ptr(&self.inner.arc);

        // SAFETY: As we obtained `self_ptr` from a valid reference above, it
        // must point to a valid `T`.
        let timer_ptr = unsafe { <T as HasHrTimer<T>>::raw_get_timer(self_ptr) };

        // SAFETY: As `timer_ptr` points into `T` and `T` is valid, `timer_ptr`
        // must point to a valid `HrTimer` instance.
        unsafe { HrTimer::<T>::raw_cancel(timer_ptr) }
    }
}

impl<T> Drop for ArcHrTimerHandle<T>
where
    T: HasHrTimer<T>,
{
    fn drop(&mut self) {
        self.cancel();
    }
}

impl<T> RawHrTimerCallback for HrTimerArc<T>
where
    T: 'static,
    T: HasHrTimer<T>,
    T: for<'a> HrTimerCallback<Pointer<'a> = Self>,
{
    type CallbackTarget<'a> = ArcBorrow<'a, T>;

    unsafe extern "C" fn run(ptr: *mut bindings::hrtimer) -> bindings::hrtimer_restart {
        // `HrTimer` is `repr(C)`
        let timer_ptr = ptr.cast::<super::HrTimer<T>>();

        // SAFETY: By C API contract `ptr` is the pointer we passed when
        // queuing the timer, so it is a `HrTimer<T>` embedded in a `T`.
        let data_ptr = unsafe { T::timer_container_of(timer_ptr) };

        // SAFETY:
        //  - `data_ptr` is derived form the pointer to the `T` that was used to
        //    queue the timer.
        //  - As per the safety requirements of the trait `HrTimerHandle`, the
        //    `ArcHrTimerHandle` associated with this timer is guaranteed to
        //    be alive until this method returns. That handle borrows the `T`
        //    behind `data_ptr` thus guaranteeing the validity of
        //    the `ArcBorrow` created below.
        //  - We own one refcount in the `ArcTimerHandle` associated with this
        //    timer, so it is not possible to get a `UniqueArc` to this
        //    allocation from other `Arc` clones.
        let receiver = unsafe { ArcBorrow::from_raw(data_ptr) };

        // SAFETY:
        // - By C API contract `timer_ptr` is the pointer that we passed when queuing the timer, so
        //   it is a valid pointer to a `HrTimer<T>` embedded in a `T`.
        // - We are within `RawHrTimerCallback::run`
        let context = unsafe { HrTimerCallbackContext::from_raw(timer_ptr) };

        T::run(receiver, context).into_c()
    }
}
