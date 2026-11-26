// SPDX-License-Identifier: GPL-2.0

//! Reset control abstractions, the consumer side of the reset controller API.
//!
//! A reset controller controls a number of reset lines. A device is held in reset while its reset
//! line is asserted.
//!
//! A driver gets an [`ExclusiveResetControl`] for a reset line of its device, and uses it to
//! assert and deassert the line. A new control is in the [`Unknown`] state. Its `assert()` and
//! `deassert()` methods, when they succeed, move it to the [`Asserted`] or [`Deasserted`] state.
//!
//! C header: [`include/linux/reset.h`](srctree/include/linux/reset.h)
//!
//! See [Reset controller API](https://docs.kernel.org/driver-api/reset.html) for more information.

use crate::{
    bindings,
    device::{
        Bound,
        Device, //
    },
    error::{
        from_err_ptr,
        to_result, //
    },
    prelude::*, //
};

use core::{
    marker::PhantomData,
    mem::ManuallyDrop,
    ptr, //
};

mod private {
    pub trait Sealed {}

    impl Sealed for super::Unknown {}
    impl Sealed for super::Asserted {}
    impl Sealed for super::Deasserted {}
}

/// A trait representing the different states a reset control can be in.
pub trait ResetState: private::Sealed {
    /// Whether dropping a reset control in this state asserts the line.
    const ASSERT_ON_DROP: bool;
}

/// A state that records nothing about the reset line, which may be asserted or deasserted.
///
/// Dropping a reset control in this state does not change the line.
pub struct Unknown;

/// A state where the last successful operation on the reset control was an assert.
///
/// Dropping a reset control in this state does not change the line.
pub struct Asserted;

/// A state where the last successful operation on the reset control was a deassert.
///
/// Dropping a reset control in this state asserts the line. An error from that assert is
/// discarded.
pub struct Deasserted;

impl ResetState for Unknown {
    const ASSERT_ON_DROP: bool = false;
}

impl ResetState for Asserted {
    const ASSERT_ON_DROP: bool = false;
}

impl ResetState for Deasserted {
    const ASSERT_ON_DROP: bool = true;
}

/// An error that can occur when trying to move a reset control between states.
pub struct Error<T> {
    /// The error that occurred.
    pub error: kernel::error::Error,

    /// The reset control the operation failed on.
    pub reset: T,
}

impl<T> From<Error<T>> for kernel::error::Error {
    /// Drops the reset control and keeps the error, so that `?` works on the state transitions.
    #[inline]
    fn from(err: Error<T>) -> Self {
        err.error
    }
}

/// An exclusive reset control of a device.
///
/// Exclusive means that the driver has direct control over the reset line. A reset line can be
/// held by one [`ExclusiveResetControl`] at a time.
///
/// A reset control obtained with [`ExclusiveResetControl::get_optional`] may have no reset line,
/// in which case every operation on it succeeds and does nothing.
///
/// The reset control is released when the [`ExclusiveResetControl`] is dropped. The lifetime `'a`
/// is that of the reference to the bound device it was obtained from, so an
/// [`ExclusiveResetControl`] cannot outlive the binding of that device to its driver.
///
/// # Examples
///
/// Holding a device in reset while its clocks are turned on, then releasing it:
///
/// ```
/// use kernel::{
///     device::{
///         Bound,
///         Device, //
///     },
///     reset::{
///         Deasserted,
///         ExclusiveResetControl, //
///     },
/// };
///
/// struct Data<'a> {
///     // Dropping `Data` asserts the line again.
///     reset: ExclusiveResetControl<'a, Deasserted>,
/// }
///
/// fn probe<'a>(dev: &'a Device<Bound>) -> Result<Data<'a>> {
///     // Put the device in reset.
///     let reset = ExclusiveResetControl::get(dev, None)?.assert()?;
///
///     // For example, turn on the clocks of the device here, while it is held in reset.
///
///     // Release the device from reset.
///     let reset = reset.deassert()?;
///
///     Ok(Data { reset })
/// }
/// ```
///
/// # Invariants
///
/// `inner` is a pointer obtained from a successful call to one of the following:
///
/// - [`reset_control_get_exclusive()`]
/// - [`reset_control_get_optional_exclusive()`]
///
/// It is treated as an opaque token that may only be accessed using C API methods (e.g., it may be
/// `NULL` if the C API returns `NULL`). The [`ExclusiveResetControl`] owns the reference that the
/// call returned.
///
/// [`reset_control_get_exclusive()`]:
/// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_exclusive
/// [`reset_control_get_optional_exclusive()`]:
/// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_optional_exclusive
pub struct ExclusiveResetControl<'a, State: ResetState> {
    inner: *mut bindings::reset_control,
    _phantom: PhantomData<(&'a Device<Bound>, State)>,
}

// SAFETY: A reset control can be used and released from any thread.
unsafe impl<State: ResetState> Send for ExclusiveResetControl<'_, State> {}

// SAFETY: An `ExclusiveResetControl` has no `&self` methods, so references to it can be shared
// between threads.
unsafe impl<State: ResetState> Sync for ExclusiveResetControl<'_, State> {}

impl<'a> ExclusiveResetControl<'a, Unknown> {
    /// Gets the reset line `name` of `dev`, or its first reset line if `name` is `None`.
    ///
    /// Equivalent to the kernel's [`reset_control_get_exclusive()`].
    ///
    /// [`reset_control_get_exclusive()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_exclusive
    #[inline]
    pub fn get(dev: &'a Device<Bound>, name: Option<&CStr>) -> Result<Self> {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid device, and `id` is `NULL` or a valid C string.
        let inner = unsafe { bindings::reset_control_get_exclusive(dev.as_raw(), id) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to `reset_control_get_exclusive()`, and
        // the returned value owns the reference from that call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }

    /// Gets the reset line `name` of `dev`, if there is one.
    ///
    /// Behaves the same as [`Self::get`], except when the device has no such reset line or
    /// `CONFIG_RESET_CONTROLLER` is disabled. In those cases, it returns an
    /// [`ExclusiveResetControl`] on which every operation succeeds and does nothing.
    ///
    /// Equivalent to the kernel's [`reset_control_get_optional_exclusive()`].
    ///
    /// [`reset_control_get_optional_exclusive()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_optional_exclusive
    #[inline]
    pub fn get_optional(dev: &'a Device<Bound>, name: Option<&CStr>) -> Result<Self> {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid device, and `id` is `NULL` or a valid C string.
        let inner = unsafe { bindings::reset_control_get_optional_exclusive(dev.as_raw(), id) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to
        // `reset_control_get_optional_exclusive()`, and the returned value owns the reference from
        // that call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }
}

impl<'a, State: ResetState> ExclusiveResetControl<'a, State> {
    /// Asserts the reset line and moves the [`ExclusiveResetControl`] to the [`Asserted`] state.
    ///
    /// Equivalent to the kernel's [`reset_control_assert()`].
    ///
    /// [`reset_control_assert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_assert
    #[inline]
    fn assert_internal(self) -> Result<ExclusiveResetControl<'a, Asserted>, Error<Self>> {
        // INVARIANT: The reference that `self` owns moves to the value that this returns.
        let reset = ManuallyDrop::new(self);

        // SAFETY: By the type invariants, `reset.inner` is a pointer that a C getter returned, and
        // `reset` owns its reference.
        to_result(unsafe { bindings::reset_control_assert(reset.inner) })
            .map(|()| ExclusiveResetControl {
                inner: reset.inner,
                _phantom: PhantomData,
            })
            .map_err(|error| Error {
                error,
                reset: ManuallyDrop::into_inner(reset),
            })
    }

    /// Deasserts the reset line and moves the [`ExclusiveResetControl`] to the [`Deasserted`]
    /// state.
    ///
    /// Equivalent to the kernel's [`reset_control_deassert()`].
    ///
    /// [`reset_control_deassert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_deassert
    #[inline]
    fn deassert_internal(self) -> Result<ExclusiveResetControl<'a, Deasserted>, Error<Self>> {
        // INVARIANT: The reference that `self` owns moves to the value that this returns.
        let reset = ManuallyDrop::new(self);

        // SAFETY: By the type invariants, `reset.inner` is a pointer that a C getter returned, and
        // `reset` owns its reference.
        to_result(unsafe { bindings::reset_control_deassert(reset.inner) })
            .map(|()| ExclusiveResetControl {
                inner: reset.inner,
                _phantom: PhantomData,
            })
            .map_err(|error| Error {
                error,
                reset: ManuallyDrop::into_inner(reset),
            })
    }
}

impl<'a> ExclusiveResetControl<'a, Unknown> {
    /// Asserts the reset line and moves the [`ExclusiveResetControl`] to the [`Asserted`] state.
    ///
    /// Equivalent to the kernel's [`reset_control_assert()`].
    ///
    /// [`reset_control_assert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_assert
    #[inline]
    pub fn assert(self) -> Result<ExclusiveResetControl<'a, Asserted>, Error<Self>> {
        self.assert_internal()
    }

    /// Deasserts the reset line and moves the [`ExclusiveResetControl`] to the [`Deasserted`]
    /// state.
    ///
    /// Equivalent to the kernel's [`reset_control_deassert()`].
    ///
    /// [`reset_control_deassert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_deassert
    #[inline]
    pub fn deassert(self) -> Result<ExclusiveResetControl<'a, Deasserted>, Error<Self>> {
        self.deassert_internal()
    }
}

impl<'a> ExclusiveResetControl<'a, Asserted> {
    /// Deasserts the reset line and moves the [`ExclusiveResetControl`] to the [`Deasserted`]
    /// state.
    ///
    /// Equivalent to the kernel's [`reset_control_deassert()`].
    ///
    /// [`reset_control_deassert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_deassert
    #[inline]
    pub fn deassert(self) -> Result<ExclusiveResetControl<'a, Deasserted>, Error<Self>> {
        self.deassert_internal()
    }
}

impl<'a> ExclusiveResetControl<'a, Deasserted> {
    /// Asserts the reset line and moves the [`ExclusiveResetControl`] to the [`Asserted`] state.
    ///
    /// Equivalent to the kernel's [`reset_control_assert()`].
    ///
    /// [`reset_control_assert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_assert
    #[inline]
    pub fn assert(self) -> Result<ExclusiveResetControl<'a, Asserted>, Error<Self>> {
        self.assert_internal()
    }
}

impl<State: ResetState> Drop for ExclusiveResetControl<'_, State> {
    fn drop(&mut self) {
        if State::ASSERT_ON_DROP {
            // SAFETY: By the type invariants, `self.inner` is a pointer that a C getter returned,
            // and `self` owns its reference.
            unsafe { bindings::reset_control_assert(self.inner) };
        }

        // SAFETY: By the type invariants, `self` owns the reference that the C getter returned,
        // so it is safe to relinquish it now.
        unsafe { bindings::reset_control_put(self.inner) };
    }
}
