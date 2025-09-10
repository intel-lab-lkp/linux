// SPDX-License-Identifier: GPL-2.0

//! Clock abstractions.
//!
//! C header: [`include/linux/clk.h`](srctree/include/linux/clk.h)
//!
//! Reference: <https://docs.kernel.org/driver-api/clk.html>

use crate::ffi::c_ulong;

/// The frequency unit.
///
/// Represents a frequency in hertz, wrapping a [`c_ulong`] value.
///
/// # Examples
///
/// ```
/// use kernel::clk::Hertz;
///
/// let hz = 1_000_000_000;
/// let rate = Hertz(hz);
///
/// assert_eq!(rate.as_hz(), hz);
/// assert_eq!(rate, Hertz(hz));
/// assert_eq!(rate, Hertz::from_khz(hz / 1_000));
/// assert_eq!(rate, Hertz::from_mhz(hz / 1_000_000));
/// assert_eq!(rate, Hertz::from_ghz(hz / 1_000_000_000));
/// ```
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Hertz(pub c_ulong);

impl Hertz {
    const KHZ_TO_HZ: c_ulong = 1_000;
    const MHZ_TO_HZ: c_ulong = 1_000_000;
    const GHZ_TO_HZ: c_ulong = 1_000_000_000;

    /// Create a new instance from kilohertz (kHz)
    pub const fn from_khz(khz: c_ulong) -> Self {
        Self(khz * Self::KHZ_TO_HZ)
    }

    /// Create a new instance from megahertz (MHz)
    pub const fn from_mhz(mhz: c_ulong) -> Self {
        Self(mhz * Self::MHZ_TO_HZ)
    }

    /// Create a new instance from gigahertz (GHz)
    pub const fn from_ghz(ghz: c_ulong) -> Self {
        Self(ghz * Self::GHZ_TO_HZ)
    }

    /// Get the frequency in hertz
    pub const fn as_hz(&self) -> c_ulong {
        self.0
    }

    /// Get the frequency in kilohertz
    pub const fn as_khz(&self) -> c_ulong {
        self.0 / Self::KHZ_TO_HZ
    }

    /// Get the frequency in megahertz
    pub const fn as_mhz(&self) -> c_ulong {
        self.0 / Self::MHZ_TO_HZ
    }

    /// Get the frequency in gigahertz
    pub const fn as_ghz(&self) -> c_ulong {
        self.0 / Self::GHZ_TO_HZ
    }
}

impl From<Hertz> for c_ulong {
    fn from(freq: Hertz) -> Self {
        freq.0
    }
}

#[cfg(CONFIG_COMMON_CLK)]
mod common_clk {
    use super::Hertz;
    use crate::{
        device::Device,
        error::{from_err_ptr, to_result, Result},
        prelude::*,
    };

    use core::{marker::PhantomData, mem::ManuallyDrop, ptr};

    mod private {
        pub trait Sealed {}

        impl Sealed for super::Unprepared {}
        impl Sealed for super::Prepared {}
        impl Sealed for super::Enabled {}
    }

    /// Obtains and enables a [`devres`]-managed [`Clk`] for a device.
    ///
    /// [`devres`]: crate::devres::Devres
    pub fn devm_enable(dev: &Device, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_ptr());

        // SAFETY: It is safe to call [`devm_clk_get_enabled`] with a valid
        // device pointer.
        from_err_ptr(unsafe { bindings::devm_clk_get_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Obtains and enables a [`devres`]-managed [`Clk`] for a device.
    ///
    /// This does not print any error messages if the clock is not found.
    ///
    /// [`devres`]: crate::devres::Devres
    pub fn devm_enable_optional(dev: &Device, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_ptr());

        // SAFETY: It is safe to call [`devm_clk_get_optional_enabled`] with a
        // valid device pointer.
        from_err_ptr(unsafe { bindings::devm_clk_get_optional_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Same as [`devm_enable_optional`], but also sets the rate.
    pub fn devm_enable_optional_with_rate(
        dev: &Device,
        name: Option<&CStr>,
        rate: Hertz,
    ) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_ptr());

        // SAFETY: It is safe to call
        // [`devm_clk_get_optional_enabled_with_rate`] with a valid device
        // pointer.
        from_err_ptr(unsafe {
            bindings::devm_clk_get_optional_enabled_with_rate(dev.as_raw(), name, rate.as_hz())
        })?;
        Ok(())
    }

    /// A trait representing the different states that a [`Clk`] can be in.
    pub trait ClkState: private::Sealed {
        /// Whether the clock should be disabled when dropped.
        const DISABLE_ON_DROP: bool;

        /// Whether the clock should be unprepared when dropped.
        const UNPREPARE_ON_DROP: bool;
    }

    /// A state where the [`Clk`] is not prepared and not enabled.
    pub struct Unprepared;

    /// A state where the [`Clk`] is prepared but not enabled.
    pub struct Prepared;

    /// A state where the [`Clk`] is both prepared and enabled.
    pub struct Enabled;

    impl ClkState for Unprepared {
        const DISABLE_ON_DROP: bool = false;
        const UNPREPARE_ON_DROP: bool = false;
    }

    impl ClkState for Prepared {
        const DISABLE_ON_DROP: bool = false;
        const UNPREPARE_ON_DROP: bool = true;
    }

    impl ClkState for Enabled {
        const DISABLE_ON_DROP: bool = true;
        const UNPREPARE_ON_DROP: bool = true;
    }

    /// An error that can occur when trying to convert a [`Clk`] between states.
    pub struct Error<State: ClkState> {
        /// The error that occurred.
        pub error: kernel::error::Error,

        /// The [`Clk`] that caused the error, so that the operation may be
        /// retried.
        pub clk: Clk<State>,
    }

    /// A reference-counted clock.
    ///
    /// Rust abstraction for the C [`struct clk`].
    ///
    /// A [`Clk`] instance represents a clock that can be in one of several
    /// states: [`Unprepared`], [`Prepared`], or [`Enabled`].
    ///
    /// No action needs to be taken when a [`Clk`] is dropped. The calls to
    /// `clk_unprepare()` and `clk_disable()` will be placed as applicable.
    ///
    /// An optional [`Clk`] is treated just like a regular [`Clk`], but its
    /// inner `struct clk` pointer is `NULL`. This interfaces correctly with the
    /// C API and also exposes all the methods of a regular [`Clk`] to users.
    ///
    /// # Invariants
    ///
    /// A [`Clk`] instance holds either a pointer to a valid [`struct clk`] created by the C
    /// portion of the kernel or a NULL pointer.
    ///
    /// Instances of this type are reference-counted. Calling [`Clk::get`] ensures that the
    /// allocation remains valid for the lifetime of the [`Clk`].
    ///
    /// The [`Prepared`] state is associated with a single count of
    /// `clk_prepare()`, and the [`Enabled`] state is associated with a single
    /// count of `clk_enable()`, and the [`Prepared`] state is associated with a
    /// single count of `clk_prepare()` and `clk_enable()`.
    ///
    /// All states are associated with a single count of `clk_get()`.
    ///
    /// # Examples
    ///
    /// The following example demonstrates how to obtain and configure a clock for a device.
    ///
    /// ```
    /// use kernel::c_str;
    /// use kernel::clk::{Clk, Enabled, Hertz, Unprepared, Prepared};
    /// use kernel::device::Device;
    /// use kernel::error::Result;
    ///
    /// fn configure_clk(dev: &Device) -> Result {
    ///     // The fastest way is to use a version of `Clk::get` for the desired
    ///     // state, i.e.:
    ///     let clk: Clk<Enabled> = Clk::<Enabled>::get(dev, Some(c_str!("apb_clk")))?;
    ///
    ///     // Any other state is also possible, e.g.:
    ///     let clk: Clk<Prepared> = Clk::<Prepared>::get(dev, Some(c_str!("apb_clk")))?;
    ///
    ///     // Later:
    ///     let clk: Clk<Enabled> = clk.enable().map_err(|error| {
    ///         error.error
    ///     })?;
    ///
    ///     // Note that error.clk is the original `clk` if the operation
    ///     // failed. It is provided as a convenience so that the operation may be
    ///     // retried in case of errors.
    ///
    ///     let expected_rate = Hertz::from_ghz(1);
    ///
    ///     if clk.rate() != expected_rate {
    ///         clk.set_rate(expected_rate)?;
    ///     }
    ///
    ///     // Nothing is needed here. The drop implementation will undo any
    ///     // operations as appropriate.
    ///     Ok(())
    /// }
    ///
    /// fn shutdown(clk: Clk<Enabled>) -> Result {
    ///     // The states can be traversed "in the reverse order" as well:
    ///     let clk: Clk<Prepared> = clk.disable().map_err(|error| {
    ///         error.error
    ///     })?;
    ///
    ///     // This is of type `Clk<Unprepared>`.
    ///     let clk = clk.unprepare();
    ///
    ///     Ok(())
    /// }
    /// ```
    ///
    /// [`struct clk`]: https://docs.kernel.org/driver-api/clk.html
    #[repr(transparent)]
    pub struct Clk<T: ClkState> {
        inner: *mut bindings::clk,
        _phantom: core::marker::PhantomData<T>,
    }

    impl Clk<Unprepared> {
        /// Gets [`Clk`] corresponding to a [`Device`] and a connection id.
        ///
        /// Equivalent to the kernel's [`clk_get`] API.
        ///
        /// [`clk_get`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get
        #[inline]
        pub fn get(dev: &Device, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_ptr());

            // SAFETY: It is safe to call [`clk_get`] for a valid device pointer.
            let inner = from_err_ptr(unsafe { bindings::clk_get(dev.as_raw(), con_id) })?;

            // INVARIANT: The reference-count is decremented when [`Clk`] goes out of scope.
            Ok(Self {
                inner,
                _phantom: PhantomData,
            })
        }

        /// Behaves the same as [`Self::get`], except when there is no clock
        /// producer. In this case, instead of returning [`ENOENT`], it returns
        /// a dummy [`Clk`].
        #[inline]
        pub fn get_optional(dev: &Device, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_ptr());

            // SAFETY: It is safe to call [`clk_get`] for a valid device pointer.
            let inner = from_err_ptr(unsafe { bindings::clk_get_optional(dev.as_raw(), con_id) })?;

            // INVARIANT: The reference-count is decremented when [`Clk`] goes out of scope.
            Ok(Self {
                inner,
                _phantom: PhantomData,
            })
        }

        /// Attempts to convert the [`Clk`] to a [`Prepared`] state.
        ///
        /// Equivalent to the kernel's [`clk_prepare`] API.
        ///
        /// [`clk_prepare`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_prepare
        #[inline]
        pub fn prepare(self) -> Result<Clk<Prepared>, Error<Unprepared>> {
            // We will be transferring the ownership of our `clk_get()` count to
            // `Clk<Prepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, self.0 is a valid argument for [`clk_prepare`].
            to_result(unsafe { bindings::clk_prepare(clk.as_raw()) })
                .map(|()| Clk {
                    inner: clk.inner,
                    _phantom: PhantomData,
                })
                .map_err(|error| Error {
                    error,
                    clk: ManuallyDrop::into_inner(clk),
                })
        }
    }

    impl Clk<Prepared> {
        /// Obtains a [`Clk`] from a [`Device`] and a connection id and prepares it.
        ///
        /// Equivalent to calling [`Clk::get`], followed by [`Clk::prepare`],
        #[inline]
        pub fn get(dev: &Device, name: Option<&CStr>) -> Result<Clk<Prepared>> {
            Clk::<Unprepared>::get(dev, name)?
                .prepare()
                .map_err(|error| error.error)
        }

        /// Attempts to convert the [`Clk`] to an [`Unprepared`] state.
        ///
        /// Equivalent to the kernel's [`clk_unprepare`] API.
        #[inline]
        pub fn unprepare(self) -> Clk<Unprepared> {
            // We will be transferring the ownership of our `clk_get()` count to
            // `Clk<Unprepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, self.0 is a valid argument for [`clk_unprepare`].
            unsafe { bindings::clk_unprepare(clk.as_raw()) }

            Clk {
                inner: clk.inner,
                _phantom: PhantomData,
            }
        }

        /// Attempts to convert the [`Clk`] to an [`Enabled`] state.
        ///
        /// Equivalent to the kernel's [`clk_enable`] API.
        ///
        /// [`clk_enable`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_enable
        #[inline]
        pub fn enable(self) -> Result<Clk<Enabled>, Error<Prepared>> {
            // We will be transferring the ownership of our `clk_get()` and
            // `clk_prepare()` counts to `Clk<Enabled>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, self.0 is a valid argument for [`clk_enable`].
            to_result(unsafe { bindings::clk_enable(clk.as_raw()) })
                .map(|()| Clk {
                    inner: clk.inner,
                    _phantom: PhantomData,
                })
                .map_err(|error| Error {
                    error,
                    clk: ManuallyDrop::into_inner(clk),
                })
        }
    }

    impl Clk<Enabled> {
        /// Gets [`Clk`] corresponding to a [`Device`] and a connection id and
        /// then prepares and enables it.
        ///
        /// Equivalent to calling [`Clk::get`], followed by [`Clk::prepare`],
        /// followed by [`Clk::enable`].
        #[inline]
        pub fn get(dev: &Device, name: Option<&CStr>) -> Result<Clk<Enabled>> {
            Clk::<Prepared>::get(dev, name)?
                .enable()
                .map_err(|error| error.error)
        }

        #[inline]
        /// Attempts to disable the [`Clk`] and convert it to the [`Prepared`]
        /// state.
        pub fn disable(self) -> Result<Clk<Prepared>, Error<Enabled>> {
            // We will be transferring the ownership of our `clk_get()` and
            // `clk_enable()` counts to `Clk<Prepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, self.0 is a valid argument for [`clk_disable`].
            unsafe { bindings::clk_disable(clk.as_raw()) };

            Ok(Clk {
                inner: clk.inner,
                _phantom: PhantomData,
            })
        }

        /// Get clock's rate.
        ///
        /// Equivalent to the kernel's [`clk_get_rate`] API.
        ///
        /// [`clk_get_rate`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get_rate
        #[inline]
        pub fn rate(&self) -> Hertz {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for
            // [`clk_get_rate`].
            Hertz(unsafe { bindings::clk_get_rate(self.as_raw()) })
        }

        /// Set clock's rate.
        ///
        /// Equivalent to the kernel's [`clk_set_rate`] API.
        ///
        /// [`clk_set_rate`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_set_rate
        #[inline]
        pub fn set_rate(&self, rate: Hertz) -> Result {
            // SAFETY: By the type invariants, self.as_raw() is a valid argument for
            // [`clk_set_rate`].
            to_result(unsafe { bindings::clk_set_rate(self.as_raw(), rate.as_hz()) })
        }
    }

    impl<T: ClkState> Clk<T> {
        /// Obtain the raw [`struct clk`] pointer.
        #[inline]
        pub fn as_raw(&self) -> *mut bindings::clk {
            self.inner
        }
    }

    impl<T: ClkState> Drop for Clk<T> {
        fn drop(&mut self) {
            if T::DISABLE_ON_DROP {
                // SAFETY: By the type invariants, self.as_raw() is a valid argument for
                // [`clk_disable`].
                unsafe { bindings::clk_disable(self.as_raw()) };
            }

            if T::UNPREPARE_ON_DROP {
                // SAFETY: By the type invariants, self.as_raw() is a valid argument for
                // [`clk_unprepare`].
                unsafe { bindings::clk_unprepare(self.as_raw()) };
            }

            // SAFETY: By the type invariants, self.as_raw() is a valid argument for
            // [`clk_put`].
            unsafe { bindings::clk_put(self.as_raw()) };
        }
    }

    // SAFETY: It is safe to call `clk_put` on another thread than where `clk_get` was called.
    unsafe impl<T: ClkState> Send for Clk<T> {}

    // SAFETY: It is safe to call any combination of the `&self` methods in parallel, as the
    // methods are synchronized internally.
    unsafe impl<T: ClkState> Sync for Clk<T> {}
}

#[cfg(CONFIG_COMMON_CLK)]
pub use common_clk::*;
