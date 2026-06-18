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
        device::{Bound, Device},
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

    /// Obtains and enables a [`devres`]-managed [`Clk`] for a bound device.
    ///
    /// [`devres`]: crate::devres::Devres
    pub fn devm_enable(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call [`devm_clk_get_enabled`] with a valid
        // device pointer.
        from_err_ptr(unsafe { bindings::devm_clk_get_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Obtains and enables a [`devres`]-managed [`Clk`] for a bound device.
    ///
    /// This does not print any error messages if the clock is not found.
    ///
    /// [`devres`]: crate::devres::Devres
    pub fn devm_enable_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call [`devm_clk_get_optional_enabled`] with a
        // valid device pointer.
        from_err_ptr(unsafe { bindings::devm_clk_get_optional_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Same as [`devm_enable_optional`], but also sets the rate.
    pub fn devm_enable_optional_with_rate(
        dev: &Device<Bound>,
        name: Option<&CStr>,
        rate: Hertz,
    ) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

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

    impl<State: ClkState> From<Error<State>> for kernel::error::Error {
        /// Discards the [`Clk`] and keeps only the error code.
        ///
        /// This makes the fallible state transitions usable with the `?`
        /// operator when the caller does not need to retry the operation on the
        /// original [`Clk`], e.g.:
        ///
        /// ```
        /// use kernel::clk::{Clk, Enabled, Unprepared};
        /// use kernel::device::{Bound, Device};
        /// use kernel::error::Result;
        ///
        /// fn get_enabled(dev: &Device<Bound>) -> Result<Clk<Enabled>> {
        ///     let clk = Clk::<Unprepared>::get(dev, Some(c"apb_clk"))?
        ///         .prepare()?
        ///         .enable()?;
        ///     Ok(clk)
        /// }
        /// ```
        #[inline]
        fn from(err: Error<State>) -> Self {
            err.error
        }
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
    /// portion of the kernel or a `NULL` pointer.
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
    /// use kernel::clk::{Clk, Enabled, Hertz, Unprepared, Prepared};
    /// use kernel::device::{Bound, Device};
    /// use kernel::error::Result;
    ///
    /// fn configure_clk(dev: &Device<Bound>) -> Result {
    ///     // The fastest way is to use a version of `Clk::get` for the desired
    ///     // state, i.e.:
    ///     let clk: Clk<Enabled> = Clk::<Enabled>::get(dev, Some(c"apb_clk"))?;
    ///
    ///     // Any other state is also possible, e.g.:
    ///     let clk: Clk<Prepared> = Clk::<Prepared>::get(dev, Some(c"apb_clk"))?;
    ///
    ///     // Later:
    ///     //
    ///     // `?` works directly thanks to `From<Error<State>>`; the failed
    ///     // `Clk` is dropped on error. Match on the returned `Error<State>`
    ///     // instead (its `clk` field is the original `Clk`) if you want to
    ///     // retry the operation.
    ///     let clk: Clk<Enabled> = clk.enable()?;
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
    ///     let clk: Clk<Prepared> = clk.disable()?;
    ///
    ///     // This is of type `Clk<Unprepared>`.
    ///     let clk = clk.unprepare();
    ///
    ///     Ok(())
    /// }
    /// ```
    ///
    /// Drivers that need to change a clock's state at runtime (for example to
    /// enable it on resume and disable it on suspend) can keep it in an enum
    /// and move between the variants:
    ///
    /// ```
    /// use kernel::clk::{Clk, Enabled, Prepared};
    /// use kernel::error::Result;
    ///
    /// enum DeviceClk {
    ///     Suspended(Clk<Prepared>),
    ///     Resumed(Clk<Enabled>),
    /// }
    ///
    /// impl DeviceClk {
    ///     fn resume(self) -> Result<Self> {
    ///         Ok(match self {
    ///             DeviceClk::Suspended(clk) => DeviceClk::Resumed(clk.enable()?),
    ///             resumed => resumed,
    ///         })
    ///     }
    ///
    ///     fn suspend(self) -> Result<Self> {
    ///         Ok(match self {
    ///             DeviceClk::Resumed(clk) => DeviceClk::Suspended(clk.disable()?),
    ///             suspended => suspended,
    ///         })
    ///     }
    /// }
    /// ```
    ///
    /// [`struct clk`]: https://docs.kernel.org/driver-api/clk.html
    #[repr(transparent)]
    pub struct Clk<T: ClkState> {
        inner: *mut bindings::clk,
        _phantom: core::marker::PhantomData<T>,
    }

    // SAFETY: It is safe to call `clk_put` on another thread than where `clk_get` was called.
    unsafe impl<T: ClkState> Send for Clk<T> {}

    // SAFETY: It is safe to call any combination of the `&self` methods in parallel, as the
    // methods are synchronized internally.
    unsafe impl<T: ClkState> Sync for Clk<T> {}

    impl Clk<Unprepared> {
        /// Gets [`Clk`] corresponding to a bound [`Device`] and a connection
        /// id.
        ///
        /// Equivalent to the kernel's [`clk_get`] API.
        ///
        /// [`clk_get`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get
        #[inline]
        pub fn get(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            Self::get_unbound(dev, name)
        }

        /// Gets [`Clk`] corresponding to a [`Device`] and a connection id,
        /// without requiring the device to be bound.
        ///
        /// This is sound because [`clk_get`] and [`clk_put`] do not depend on the
        /// device being bound to a driver. It is `pub(crate)` because a driver
        /// should obtain its clocks through a bound device (see [`Clk::get`]); it
        /// is meant for the few in-tree abstractions that operate on a device
        /// outside a bind scope, such as the generic DT cpufreq driver.
        ///
        /// [`clk_get`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get
        #[inline]
        pub(crate) fn get_unbound(dev: &Device, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

            // SAFETY: It is safe to call [`clk_get`] for a valid device pointer
            // and any `con_id`, including NULL.
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
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

            // SAFETY: It is safe to call [`clk_get`] for a valid device pointer
            // and any `con_id`, including NULL.
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

            // SAFETY: By the type invariants, `self.0` is a valid argument for
            // [`clk_prepare`].
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
        /// Obtains a [`Clk`] from a bound [`Device`] and a connection id and
        /// prepares it.
        ///
        /// Equivalent to calling [`Clk::get`], followed by [`Clk::prepare`],
        #[inline]
        pub fn get(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Prepared>> {
            Clk::<Unprepared>::get(dev, name)?
                .prepare()
                .map_err(|error| error.error)
        }

        /// Behaves the same as [`Self::get`], except when there is no clock
        /// producer. In this case, instead of returning [`ENOENT`], it returns
        /// a dummy [`Clk`].
        #[inline]
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Prepared>> {
            Clk::<Unprepared>::get_optional(dev, name)?
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

            // SAFETY: By the type invariants, `self.0` is a valid argument for
            // [`clk_unprepare`].
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

            // SAFETY: By the type invariants, `self.0` is a valid argument for
            // [`clk_enable`].
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

        /// Runs `cb` with the clock temporarily enabled.
        ///
        /// The clock is enabled before `cb` runs and disabled again afterwards,
        /// so the [`Enabled`] state is scoped to the closure and the [`Clk`]
        /// remains [`Prepared`]. This is convenient for drivers that only need
        /// the clock running for a short, well-defined section (e.g. while
        /// touching registers) without giving up ownership of the prepared
        /// clock or threading it through an intermediate state, e.g.:
        ///
        /// ```
        /// use kernel::clk::{Clk, Enabled, Hertz, Prepared};
        /// use kernel::error::Result;
        ///
        /// fn read_rate(clk: &Clk<Prepared>) -> Result<Hertz> {
        ///     clk.with_enabled(|clk: &Clk<Enabled>| clk.rate())
        /// }
        /// ```
        ///
        /// Equivalent to a balanced [`clk_enable`]/[`clk_disable`] pair around
        /// `cb`.
        ///
        /// [`clk_enable`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_enable
        /// [`clk_disable`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_disable
        #[inline]
        pub fn with_enabled<R>(&self, cb: impl FnOnce(&Clk<Enabled>) -> R) -> Result<R> {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument for
            // [`clk_enable`].
            to_result(unsafe { bindings::clk_enable(self.as_raw()) })?;

            // Borrow the same clock as `Clk<Enabled>` for the duration of `cb`.
            // It must not be dropped, as that would run `clk_disable`/`clk_put`
            // against counts owned by `self`; the matching `clk_disable` below
            // balances the `clk_enable` above instead.
            //
            // INVARIANT: The clock is enabled for the lifetime of `enabled`.
            let enabled = ManuallyDrop::new(Clk::<Enabled> {
                inner: self.inner,
                _phantom: PhantomData,
            });

            let ret = cb(&enabled);

            // SAFETY: The `clk_enable` above succeeded, so this balances it.
            // `cb` only had a shared reference, so the enable count is unchanged.
            unsafe { bindings::clk_disable(self.as_raw()) };

            Ok(ret)
        }
    }

    impl Clk<Enabled> {
        /// Gets [`Clk`] corresponding to a bound [`Device`] and a connection id
        /// and then prepares and enables it.
        ///
        /// Equivalent to calling [`Clk::get`], followed by [`Clk::prepare`],
        /// followed by [`Clk::enable`].
        #[inline]
        pub fn get(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Enabled>> {
            Clk::<Prepared>::get(dev, name)?
                .enable()
                .map_err(|error| error.error)
        }

        /// Behaves the same as [`Self::get`], except when there is no clock
        /// producer. In this case, instead of returning [`ENOENT`], it returns
        /// a dummy [`Clk`].
        #[inline]
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Enabled>> {
            Clk::<Prepared>::get_optional(dev, name)?
                .enable()
                .map_err(|error| error.error)
        }

        /// Attempts to disable the [`Clk`] and convert it to the [`Prepared`]
        /// state.
        #[inline]
        pub fn disable(self) -> Result<Clk<Prepared>, Error<Enabled>> {
            // We will be transferring the ownership of our `clk_get()` and
            // `clk_enable()` counts to `Clk<Prepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, `self.0` is a valid argument for
            // [`clk_disable`].
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
}

#[cfg(CONFIG_COMMON_CLK)]
pub use common_clk::*;
