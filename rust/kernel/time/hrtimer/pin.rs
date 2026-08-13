// SPDX-License-Identifier: GPL-2.0

use super::HasHrTimer;
use super::HrTimer;
use super::HrTimerCallback;
use super::HrTimerCallbackContext;
use super::HrTimerHandle;
use super::HrTimerMode;
use super::RawHrTimerCallback;
use super::UnsafeHrTimerPointer;
use core::pin::Pin;

/// A wrapper around a pinned shared reference that's guaranteed unique.
///
/// The `HrTimerPin` type can be thought of as a special pinned reference to an object that
/// owns the permission to arm the [`HrTimer`] stored in the object. By ensuring that each
/// object has only one `HrTimerPin`, the owner of it is assured exclusive access to the arming
/// operation. Starting a timer consumes the `HrTimerPin`, and the returned
/// [`PinHrTimerHandle`] keeps the object borrowed, so the timer cannot be armed again until the
/// handle is dropped.
///
/// While this `HrTimerPin` is unique, shared pinned references to the object can still be
/// obtained with [`HrTimerPin::as_ref`].
///
/// # Invariants
///
/// * Each object has at most one `HrTimerPin`.
pub struct HrTimerPin<'a, T>
where
    T: HasHrTimer<T>,
{
    pin: Pin<&'a T>,
}

impl<'a, T> HrTimerPin<'a, T>
where
    T: HasHrTimer<T>,
{
    /// Create a `HrTimerPin` from an exclusive pinned reference to a `T`.
    #[inline]
    pub fn new(inner: Pin<&'a mut T>) -> Self {
        // INVARIANT: We have an exclusive reference, so there is no `HrTimerPin` for this
        // object.
        Self {
            pin: inner.into_ref(),
        }
    }

    /// Get a shared pinned reference to the object.
    ///
    /// The returned reference can be used to access the object, but not to arm its timer.
    #[inline]
    pub fn as_ref(&self) -> Pin<&'a T> {
        self.pin
    }
}

// SAFETY: We capture the lifetime of `Self` when we create a `PinHrTimerHandle`,
// so `Self` will outlive the handle.
unsafe impl<'a, T> UnsafeHrTimerPointer for HrTimerPin<'a, T>
where
    T: Send + Sync,
    T: HasHrTimer<T>,
    T: HrTimerCallback<Pointer<'a> = HrTimerPin<'a, T>>,
{
    type TimerMode = <T as HasHrTimer<T>>::TimerMode;
    type TimerHandle = PinHrTimerHandle<'a, T>;

    unsafe fn start(
        self,
        expires: <<T as HasHrTimer<T>>::TimerMode as HrTimerMode>::Expires,
    ) -> Self::TimerHandle {
        // Cast to pointer
        let self_ptr: *const T = self.pin.get_ref();

        // SAFETY:
        //  - As we derive `self_ptr` from a reference above, it must point to a
        //    valid `T`.
        //  - We keep `self` alive by wrapping it in a handle below.
        unsafe { T::start(self_ptr, expires) };

        PinHrTimerHandle { inner: self.pin }
    }
}

/// A handle for a `Pin<&HasHrTimer>`. When the handle exists, the timer might be
/// running.
pub struct PinHrTimerHandle<'a, T>
where
    T: HasHrTimer<T>,
{
    pub(crate) inner: Pin<&'a T>,
}

// SAFETY: We cancel the timer when the handle is dropped. The implementation of
// the `cancel` method will block if the timer handler is running.
unsafe impl<'a, T> HrTimerHandle for PinHrTimerHandle<'a, T>
where
    T: HasHrTimer<T>,
{
    fn cancel(&mut self) -> bool {
        let self_ptr: *const T = self.inner.get_ref();

        // SAFETY: As we got `self_ptr` from a reference above, it must point to
        // a valid `T`.
        let timer_ptr = unsafe { <T as HasHrTimer<T>>::raw_get_timer(self_ptr) };

        // SAFETY: As `timer_ptr` is derived from a reference, it must point to
        // a valid and initialized `HrTimer`.
        unsafe { HrTimer::<T>::raw_cancel(timer_ptr) }
    }
}

impl<'a, T> Drop for PinHrTimerHandle<'a, T>
where
    T: HasHrTimer<T>,
{
    fn drop(&mut self) {
        self.cancel();
    }
}

impl<'a, T> RawHrTimerCallback for HrTimerPin<'a, T>
where
    T: HasHrTimer<T>,
    T: HrTimerCallback<Pointer<'a> = Self>,
{
    type CallbackTarget<'b> = Pin<&'a T>;

    unsafe extern "C" fn run(ptr: *mut bindings::hrtimer) -> bindings::hrtimer_restart {
        // `HrTimer` is `repr(C)`
        let timer_ptr = ptr.cast::<HrTimer<T>>();

        // SAFETY: By the safety requirement of this function, `timer_ptr`
        // points to a `HrTimer<T>` contained in an `T`.
        let receiver_ptr = unsafe { T::timer_container_of(timer_ptr) };

        // SAFETY:
        //  - By the safety requirement of this function, `timer_ptr`
        //    points to a `HrTimer<T>` contained in an `T`.
        //  - As per the safety requirements of the trait `HrTimerHandle`, the
        //    `PinHrTimerHandle` associated with this timer is guaranteed to
        //    be alive until this method returns. That handle borrows the `T`
        //    behind `receiver_ptr`, thus guaranteeing the validity of
        //    the reference created below.
        let receiver_ref = unsafe { &*receiver_ptr };

        // SAFETY: `receiver_ref` only exists as pinned, so it is safe to pin it
        // here.
        let receiver_pin = unsafe { Pin::new_unchecked(receiver_ref) };

        // SAFETY:
        // - By C API contract `timer_ptr` is the pointer that we passed when queuing the timer, so
        //   it is a valid pointer to a `HrTimer<T>` embedded in a `T`.
        // - We are within `RawHrTimerCallback::run`
        let context = unsafe { HrTimerCallbackContext::from_raw(timer_ptr) };

        T::run(receiver_pin, context).into_c()
    }
}
