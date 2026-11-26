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
//! A driver can also get a single reset control for all the reset lines in the `resets`
//! property of its device. Its operations then act on all of those lines.
//!
//! An [`ExclusiveResetControl`] in the [`Unknown`] state can also assert its line for a limited
//! time, with [`ExclusiveResetControl::assert_guard`]. The [`AssertedGuard`] that it returns
//! deasserts the line when dropped, and the control stays in the [`Unknown`] state.
//!
//! If the reset line is shared with other devices, the driver asks for a [`SharedResetControl`]
//! instead.
//!
//! A driver that only needs its reset line deasserted while it is bound can call
//! [`ExclusiveResetControl::devm_deassert`] instead of keeping a reset control.
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
///
/// For a [`SharedResetControl`], the line may still be deasserted after that assert, as described
/// in [`SharedResetControl::assert`].
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

/// A trait representing the states a [`SharedResetControl`] can be in.
///
/// [`Asserted`] is not one of them, since the line may still be deasserted after a
/// [`SharedResetControl`] is asserted.
pub trait SharedResetState: ResetState {}

impl SharedResetState for Unknown {}
impl SharedResetState for Deasserted {}

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
/// A reset control obtained with [`ExclusiveResetControl::get_optional`] or
/// [`ExclusiveResetControl::get_array_optional`] may have no reset line, in which case every
/// operation on it succeeds and does nothing.
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
/// - `fwnode_reset_control_array_get()` with the `RESET_CONTROL_EXCLUSIVE` or
///   `RESET_CONTROL_OPTIONAL_EXCLUSIVE` flag
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

    /// Gets the reset lines of `dev` as one [`ExclusiveResetControl`], as [`Self::get_array`] does
    /// or, if `optional` is set, as [`Self::get_array_optional`] does.
    fn get_array_internal(dev: &'a Device<Bound>, optional: bool) -> Result<Self> {
        let flags = if optional {
            bindings::reset_control_flags_RESET_CONTROL_OPTIONAL_EXCLUSIVE
        } else {
            bindings::reset_control_flags_RESET_CONTROL_EXCLUSIVE
        };

        let fwnode = dev
            .fwnode()
            .map_or(ptr::null_mut(), |fwnode| fwnode.as_raw());

        // SAFETY: `fwnode` is the firmware node of `dev` or `NULL`, which
        // `fwnode_reset_control_array_get()` checks for.
        let inner = unsafe { bindings::fwnode_reset_control_array_get(fwnode, flags) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to `fwnode_reset_control_array_get()`
        // with one of the two flags above, and the returned value owns the reference from that
        // call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }

    /// Gets the reset lines listed in the `resets` property of `dev` as one
    /// [`ExclusiveResetControl`].
    ///
    /// Its operations act on all the lines, in no guaranteed order. If one of them fails on a
    /// line, the opposite operation is run on the lines it had already succeeded on, whatever
    /// state they were in before. This includes operations that are done on drop. The caller only
    /// gets the error from the line that failed, while errors from the opposite operation are
    /// discarded.
    ///
    /// Equivalent to the kernel's `fwnode_reset_control_array_get()` on the firmware node of
    /// `dev`, as `devm_reset_control_array_get_exclusive()` does.
    ///
    /// # Examples
    ///
    /// Keeping all the reset lines of a device deasserted:
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
    ///     // Dropping `Data` tries to assert the lines again.
    ///     resets: ExclusiveResetControl<'a, Deasserted>,
    /// }
    ///
    /// fn probe<'a>(dev: &'a Device<Bound>) -> Result<Data<'a>> {
    ///     let resets = ExclusiveResetControl::get_array(dev)?.deassert()?;
    ///
    ///     Ok(Data { resets })
    /// }
    /// ```
    #[inline]
    pub fn get_array(dev: &'a Device<Bound>) -> Result<Self> {
        Self::get_array_internal(dev, false)
    }

    /// Gets the reset lines listed in the `resets` property of `dev` as one
    /// [`ExclusiveResetControl`], if there are any.
    ///
    /// Behaves the same as [`Self::get_array`], except when `dev` has no firmware node, when its
    /// `resets` property has no entry or cannot be read, or when `CONFIG_RESET_CONTROLLER` is
    /// disabled. In those cases, it returns an [`ExclusiveResetControl`] on which every operation
    /// succeeds and does nothing.
    ///
    /// Equivalent to the kernel's `fwnode_reset_control_array_get()` on the firmware node of
    /// `dev`, as `devm_reset_control_array_get_optional_exclusive()` does.
    #[inline]
    pub fn get_array_optional(dev: &'a Device<Bound>) -> Result<Self> {
        Self::get_array_internal(dev, true)
    }

    /// Deasserts the reset line `name` of `dev`, or its first reset line if `name` is `None`, until
    /// `dev` is unbound.
    ///
    /// The reset control is managed by devres, which asserts the line and releases the control
    /// when `dev` is unbound. An error from that assert is discarded. Until then, devres holds
    /// the line as an [`ExclusiveResetControl`] does.
    ///
    /// Equivalent to the kernel's [`devm_reset_control_get_exclusive_deasserted()`].
    ///
    /// # Examples
    ///
    /// Keeping the reset line of a device deasserted until the device is unbound:
    ///
    /// ```
    /// use kernel::{
    ///     device::{
    ///         Bound,
    ///         Device, //
    ///     },
    ///     reset::ExclusiveResetControl,
    /// };
    ///
    /// fn probe(dev: &Device<Bound>) -> Result {
    ///     ExclusiveResetControl::devm_deassert(dev, None)
    /// }
    /// ```
    ///
    /// [`devm_reset_control_get_exclusive_deasserted()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.devm_reset_control_get_exclusive_deasserted
    #[inline]
    pub fn devm_deassert(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid and bound device, and `id` is `NULL` or a valid C string.
        let inner =
            unsafe { bindings::devm_reset_control_get_exclusive_deasserted(dev.as_raw(), id) };
        from_err_ptr(inner)?;

        Ok(())
    }

    /// Deasserts the reset line `name` of `dev` until `dev` is unbound, if there is such a line.
    ///
    /// Behaves the same as [`Self::devm_deassert`], except when the device has no such reset line
    /// or `CONFIG_RESET_CONTROLLER` is disabled. In those cases, it succeeds and does nothing.
    ///
    /// Equivalent to the kernel's [`devm_reset_control_get_optional_exclusive_deasserted()`].
    ///
    /// [`devm_reset_control_get_optional_exclusive_deasserted()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.devm_reset_control_get_optional_exclusive_deasserted
    #[inline]
    pub fn devm_deassert_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid and bound device, and `id` is `NULL` or a valid C string.
        let inner = unsafe {
            bindings::devm_reset_control_get_optional_exclusive_deasserted(dev.as_raw(), id)
        };
        from_err_ptr(inner)?;

        Ok(())
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

    /// Asserts the reset line and returns an [`AssertedGuard`], which deasserts it when dropped.
    ///
    /// The [`ExclusiveResetControl`] stays in the [`Unknown`] state, and it cannot be used while
    /// the guard exists.
    ///
    /// The line is asserted with the kernel's [`reset_control_assert()`].
    ///
    /// [`reset_control_assert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_assert
    #[inline]
    pub fn assert_guard(&mut self) -> Result<AssertedGuard<'_, 'a>> {
        // SAFETY: By the type invariants, `self.inner` is a pointer that a C getter returned, and
        // `self` owns its reference.
        to_result(unsafe { bindings::reset_control_assert(self.inner) })?;

        Ok(AssertedGuard { reset: self })
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

/// A guard returned by [`ExclusiveResetControl::assert_guard`], which deasserts the reset line when
/// dropped.
///
/// An error from the deassert done on drop is discarded. [`AssertedGuard::deassert`] deasserts the
/// line and returns the error.
///
/// # Examples
///
/// Holding a device in reset for a limited time:
///
/// ```
/// use kernel::reset::{
///     ExclusiveResetControl,
///     Unknown, //
/// };
///
/// # fn restart_clocks() -> Result { Ok(()) }
/// fn reset_device(reset: &mut ExclusiveResetControl<'_, Unknown>) -> Result {
///     let asserted = reset.assert_guard()?;
///
///     // If this fails, dropping `asserted` deasserts the line.
///     restart_clocks()?;
///
///     asserted.deassert()
/// }
/// ```
#[must_use = "the reset line is deasserted immediately when the guard is unused"]
pub struct AssertedGuard<'r, 'a> {
    reset: &'r mut ExclusiveResetControl<'a, Unknown>,
}

impl<'r, 'a> AssertedGuard<'r, 'a> {
    /// Deasserts the reset line and consumes the guard.
    ///
    /// The only difference from dropping the guard is that this returns an error from the
    /// deassert, which a drop discards.
    ///
    /// Equivalent to the kernel's [`reset_control_deassert()`].
    ///
    /// [`reset_control_deassert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_deassert
    #[inline]
    pub fn deassert(self) -> Result {
        // Skip `drop()`, which would deassert the line a second time.
        let guard = ManuallyDrop::new(self);

        // SAFETY: By the type invariants of `ExclusiveResetControl`, `guard.reset.inner` is a
        // pointer that a C getter returned, and `guard.reset` owns its reference.
        to_result(unsafe { bindings::reset_control_deassert(guard.reset.inner) })
    }
}

impl<'r, 'a> Drop for AssertedGuard<'r, 'a> {
    fn drop(&mut self) {
        // SAFETY: By the type invariants of `ExclusiveResetControl`, `self.reset.inner` is a
        // pointer that a C getter returned, and `self.reset` owns its reference.
        unsafe { bindings::reset_control_deassert(self.reset.inner) };
    }
}

/// A shared reset control of a device.
///
/// Shared means that other devices may use the same reset line. The deasserts on the line are
/// counted, and only the first deassert and the last assert act on it. A [`SharedResetControl`]
/// can only be asserted after it was deasserted, as the C API requires.
///
/// A reset line cannot be held by a [`SharedResetControl`] and by an [`ExclusiveResetControl`] at
/// the same time.
///
/// The reset control is released when the [`SharedResetControl`] is dropped. The lifetime `'a` is
/// that of the reference to the bound device it was obtained from, so a [`SharedResetControl`]
/// cannot outlive the binding of that device to its driver.
///
/// # Examples
///
/// Keeping a shared reset line deasserted:
///
/// ```
/// use kernel::{
///     device::{
///         Bound,
///         Device, //
///     },
///     reset::{
///         Deasserted,
///         SharedResetControl, //
///     },
/// };
///
/// struct Data<'a> {
///     // Dropping `Data` asserts the reset control again.
///     reset: SharedResetControl<'a, Deasserted>,
/// }
///
/// fn probe<'a>(dev: &'a Device<Bound>) -> Result<Data<'a>> {
///     let reset = SharedResetControl::get(dev, None)?.deassert()?;
///
///     Ok(Data { reset })
/// }
/// ```
///
/// # Invariants
///
/// `inner` is a pointer obtained from a successful call to one of the following:
///
/// - [`reset_control_get_shared()`]
/// - [`reset_control_get_optional_shared()`]
/// - `fwnode_reset_control_array_get()` with the `RESET_CONTROL_SHARED` or
///   `RESET_CONTROL_OPTIONAL_SHARED` flag
///
/// It is treated as an opaque token that may only be accessed using C API methods (e.g., it may be
/// `NULL` if the C API returns `NULL`). The [`SharedResetControl`] owns the reference that the
/// call returned.
///
/// [`reset_control_get_shared()`]:
/// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_shared
/// [`reset_control_get_optional_shared()`]:
/// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_optional_shared
pub struct SharedResetControl<'a, State: SharedResetState> {
    inner: *mut bindings::reset_control,
    _phantom: PhantomData<(&'a Device<Bound>, State)>,
}

// SAFETY: A reset control can be used and released from any thread.
unsafe impl<State: SharedResetState> Send for SharedResetControl<'_, State> {}

// SAFETY: A `SharedResetControl` has no `&self` methods, so references to it can be shared between
// threads.
unsafe impl<State: SharedResetState> Sync for SharedResetControl<'_, State> {}

impl<'a> SharedResetControl<'a, Unknown> {
    /// Gets the reset line `name` of `dev`, or its first reset line if `name` is `None`.
    ///
    /// Equivalent to the kernel's [`reset_control_get_shared()`].
    ///
    /// [`reset_control_get_shared()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_shared
    #[inline]
    pub fn get(dev: &'a Device<Bound>, name: Option<&CStr>) -> Result<Self> {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid device, and `id` is `NULL` or a valid C string.
        let inner = unsafe { bindings::reset_control_get_shared(dev.as_raw(), id) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to `reset_control_get_shared()`, and the
        // returned value owns the reference from that call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }

    /// Gets the reset line `name` of `dev`, if there is one.
    ///
    /// Behaves the same as [`Self::get`], except when the device has no such reset line or
    /// `CONFIG_RESET_CONTROLLER` is disabled. In those cases, it returns a [`SharedResetControl`]
    /// on which every operation succeeds and does nothing.
    ///
    /// Equivalent to the kernel's [`reset_control_get_optional_shared()`].
    ///
    /// [`reset_control_get_optional_shared()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_get_optional_shared
    #[inline]
    pub fn get_optional(dev: &'a Device<Bound>, name: Option<&CStr>) -> Result<Self> {
        let id = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: `dev` is a valid device, and `id` is `NULL` or a valid C string.
        let inner = unsafe { bindings::reset_control_get_optional_shared(dev.as_raw(), id) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to
        // `reset_control_get_optional_shared()`, and the returned value owns the reference from
        // that call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }

    /// Gets the reset lines of `dev` as one [`SharedResetControl`], as [`Self::get_array`] does
    /// or, if `optional` is set, as [`Self::get_array_optional`] does.
    fn get_array_internal(dev: &'a Device<Bound>, optional: bool) -> Result<Self> {
        let flags = if optional {
            bindings::reset_control_flags_RESET_CONTROL_OPTIONAL_SHARED
        } else {
            bindings::reset_control_flags_RESET_CONTROL_SHARED
        };

        let fwnode = dev
            .fwnode()
            .map_or(ptr::null_mut(), |fwnode| fwnode.as_raw());

        // SAFETY: `fwnode` is the firmware node of `dev` or `NULL`, which
        // `fwnode_reset_control_array_get()` checks for.
        let inner = unsafe { bindings::fwnode_reset_control_array_get(fwnode, flags) };
        let inner = from_err_ptr(inner)?;

        // INVARIANT: `inner` comes from a successful call to `fwnode_reset_control_array_get()`
        // with one of the two flags above, and the returned value owns the reference from that
        // call.
        Ok(Self {
            inner,
            _phantom: PhantomData,
        })
    }

    /// Gets the reset lines listed in the `resets` property of `dev` as one
    /// [`SharedResetControl`].
    ///
    /// Its operations act on all the lines, in no guaranteed order. If one of them fails on a
    /// line, the opposite operation is run on the lines it had already succeeded on. This
    /// includes the assert that is done on drop. The caller only gets the error from the line
    /// that failed, while errors from the opposite operation are discarded.
    ///
    /// A [`SharedResetControl::assert`] that fails on one line therefore leaves the control
    /// holding its deasserts on the other lines. The control is in the [`Unknown`] state then,
    /// and does not give them back.
    ///
    /// Equivalent to the kernel's `fwnode_reset_control_array_get()` on the firmware node of
    /// `dev`, as `devm_reset_control_array_get_shared()` does.
    ///
    /// # Examples
    ///
    /// Keeping all the shared reset lines of a device deasserted:
    ///
    /// ```
    /// use kernel::{
    ///     device::{
    ///         Bound,
    ///         Device, //
    ///     },
    ///     reset::{
    ///         Deasserted,
    ///         SharedResetControl, //
    ///     },
    /// };
    ///
    /// struct Data<'a> {
    ///     // Dropping `Data` asserts the reset control again.
    ///     resets: SharedResetControl<'a, Deasserted>,
    /// }
    ///
    /// fn probe<'a>(dev: &'a Device<Bound>) -> Result<Data<'a>> {
    ///     let resets = SharedResetControl::get_array(dev)?.deassert()?;
    ///
    ///     Ok(Data { resets })
    /// }
    /// ```
    #[inline]
    pub fn get_array(dev: &'a Device<Bound>) -> Result<Self> {
        Self::get_array_internal(dev, false)
    }

    /// Gets the reset lines listed in the `resets` property of `dev` as one
    /// [`SharedResetControl`], if there are any.
    ///
    /// Behaves the same as [`Self::get_array`], except when `dev` has no firmware node, when its
    /// `resets` property has no entry or cannot be read, or when `CONFIG_RESET_CONTROLLER` is
    /// disabled. In those cases, it returns a [`SharedResetControl`] on which every operation
    /// succeeds and does nothing.
    ///
    /// Equivalent to the kernel's `fwnode_reset_control_array_get()` on the firmware node of
    /// `dev`, as `devm_reset_control_array_get_optional_shared()` does.
    #[inline]
    pub fn get_array_optional(dev: &'a Device<Bound>) -> Result<Self> {
        Self::get_array_internal(dev, true)
    }

    /// Deasserts the reset line and moves the [`SharedResetControl`] to the [`Deasserted`] state.
    ///
    /// Equivalent to the kernel's [`reset_control_deassert()`].
    ///
    /// [`reset_control_deassert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_deassert
    #[inline]
    pub fn deassert(self) -> Result<SharedResetControl<'a, Deasserted>, Error<Self>> {
        // INVARIANT: The reference that `self` owns moves to the value that this returns.
        let reset = ManuallyDrop::new(self);

        // SAFETY: By the type invariants, `reset.inner` is a pointer that a C getter returned, and
        // `reset` owns its reference.
        to_result(unsafe { bindings::reset_control_deassert(reset.inner) })
            .map(|()| SharedResetControl {
                inner: reset.inner,
                _phantom: PhantomData,
            })
            .map_err(|error| Error {
                error,
                reset: ManuallyDrop::into_inner(reset),
            })
    }
}

impl<'a> SharedResetControl<'a, Deasserted> {
    /// Asserts the reset line and moves the [`SharedResetControl`] to the [`Unknown`] state.
    ///
    /// The line may still be deasserted afterwards, for as long as other users keep it so.
    ///
    /// If this fails, the [`SharedResetControl`] is returned in the [`Unknown`] state as well.
    ///
    /// Equivalent to the kernel's [`reset_control_assert()`].
    ///
    /// [`reset_control_assert()`]:
    /// https://docs.kernel.org/driver-api/reset.html#c.reset_control_assert
    #[inline]
    pub fn assert(
        self,
    ) -> Result<SharedResetControl<'a, Unknown>, Error<SharedResetControl<'a, Unknown>>> {
        // INVARIANT: The reference that `self` owns moves to `reset`.
        let this = ManuallyDrop::new(self);
        let reset = SharedResetControl {
            inner: this.inner,
            _phantom: PhantomData,
        };

        // An assert that fails may already have given the deassert back, so `reset` is `Unknown`
        // in both cases.
        //
        // SAFETY: By the type invariants, `reset.inner` is a pointer that a C getter returned, and
        // `reset` owns its reference.
        match to_result(unsafe { bindings::reset_control_assert(reset.inner) }) {
            Ok(()) => Ok(reset),
            Err(error) => Err(Error { error, reset }),
        }
    }
}

impl<State: SharedResetState> Drop for SharedResetControl<'_, State> {
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
