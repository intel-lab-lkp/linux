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
    ///
    /// # Panics
    ///
    /// Panics if `CONFIG_RUST_OVERFLOW_CHECKS` is enabled and `khz` is greater
    /// than `c_ulong::MAX / 1_000`.
    pub const fn from_khz(khz: c_ulong) -> Self {
        Self(khz * Self::KHZ_TO_HZ)
    }

    /// Create a new instance from megahertz (MHz)
    ///
    /// # Panics
    ///
    /// Panics if `CONFIG_RUST_OVERFLOW_CHECKS` is enabled and `mhz` is greater
    /// than `c_ulong::MAX / 1_000_000`.
    pub const fn from_mhz(mhz: c_ulong) -> Self {
        Self(mhz * Self::MHZ_TO_HZ)
    }

    /// Create a new instance from gigahertz (GHz)
    ///
    /// # Panics
    ///
    /// Panics if `CONFIG_RUST_OVERFLOW_CHECKS` is enabled and `ghz` is greater
    /// than `c_ulong::MAX / 1_000_000_000`.
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
        ops::Deref,
        ptr, //
    };

    mod private {
        pub trait Sealed {}

        impl Sealed for super::Unprepared {}
        impl Sealed for super::Prepared {}
        impl Sealed for super::Enabled {}
    }

    /// Obtains and enables a [`devres`]-managed [`Clk`] for a bound device.
    ///
    /// [`devres`]: crate::devres::Devres
    #[inline]
    pub fn devm_enable(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call `devm_clk_get_enabled()` for a valid device
        // pointer and any `name`, including NULL.
        from_err_ptr(unsafe { bindings::devm_clk_get_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Obtains and enables an optional [`devres`]-managed [`Clk`] for a bound
    /// device.
    ///
    /// If the clock does not exist, this succeeds and does nothing.
    ///
    /// [`devres`]: crate::devres::Devres
    #[inline]
    pub fn devm_enable_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call `devm_clk_get_optional_enabled()` for a
        // valid device pointer and any `name`, including NULL.
        from_err_ptr(unsafe { bindings::devm_clk_get_optional_enabled(dev.as_raw(), name) })?;
        Ok(())
    }

    /// Same as [`devm_enable_optional`], but also sets the rate.
    #[inline]
    pub fn devm_enable_optional_with_rate(
        dev: &Device<Bound>,
        name: Option<&CStr>,
        rate: Hertz,
    ) -> Result {
        let name = name.map_or(ptr::null(), |n| n.as_char_ptr());

        // SAFETY: It is safe to call `devm_clk_get_optional_enabled_with_rate()`
        // for a valid device pointer and any `name`, including NULL.
        from_err_ptr(unsafe {
            bindings::devm_clk_get_optional_enabled_with_rate(dev.as_raw(), name, rate.as_hz())
        })?;
        Ok(())
    }

    /// A trait representing the different states that a [`Clk`] can be in.
    pub trait ClkState: private::Sealed {
        /// Whether the clock is enabled in this state.
        const ENABLED: bool;

        /// Whether the clock is prepared in this state.
        const PREPARED: bool;
    }

    /// A state where the [`Clk`] is not prepared and not enabled.
    pub struct Unprepared;

    /// A state where the [`Clk`] is prepared but not enabled.
    pub struct Prepared;

    /// A state where the [`Clk`] is both prepared and enabled.
    pub struct Enabled;

    impl ClkState for Unprepared {
        const ENABLED: bool = false;
        const PREPARED: bool = false;
    }

    impl ClkState for Prepared {
        const ENABLED: bool = false;
        const PREPARED: bool = true;
    }

    impl ClkState for Enabled {
        const ENABLED: bool = true;
        const PREPARED: bool = true;
    }

    /// An error that can occur when trying to convert a [`Clk`] between states.
    pub struct Error<State: ClkState> {
        /// The error that occurred.
        pub error: kernel::error::Error,

        /// The [`Clk`] that caused the error, so that the operation may be
        /// retried.
        pub clk: Clk<State>,
    }

    impl<State: ClkState> core::fmt::Debug for Error<State> {
        fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
            self.error.fmt(f)
        }
    }

    impl<State: ClkState> From<Error<State>> for kernel::error::Error {
        /// Discards the [`Clk`] and keeps only the error code.
        ///
        /// This makes the fallible state transitions usable with the `?`
        /// operator when the caller does not need to retry the operation on the
        /// original [`Clk`], e.g.:
        ///
        /// ```
        /// use kernel::clk::{
        ///     Clk,
        ///     Enabled,
        ///     Unprepared,
        /// };
        /// use kernel::device::{
        ///     Bound,
        ///     Device,
        /// };
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
    /// `clk_disable()` and `clk_unprepare()` will be made as applicable.
    /// Since `clk_unprepare()` may sleep, a [`Clk<Prepared>`] or a
    /// [`Clk<Enabled>`] must not be dropped in atomic context.
    ///
    /// An optional [`Clk`] is treated like a regular [`Clk`], but its
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
    /// count of both `clk_prepare()` and `clk_enable()`.
    ///
    /// A shared reference to a [`Clk<Prepared>`] or a [`Clk<Enabled>`] may also
    /// be obtained from a [`PreparedGuard`], an [`EnabledGuard`] or a
    /// [`PrepareEnabledGuard`]. In that case, the guard owns the counts of that
    /// state for as long as the reference is alive.
    ///
    /// All states are associated with a single count of `clk_get()`.
    ///
    /// # Examples
    ///
    /// The following example demonstrates how to obtain and configure a clock for a device.
    ///
    /// ```
    /// use kernel::clk::{
    ///     Clk,
    ///     Enabled,
    ///     Hertz,
    ///     Prepared,
    ///     Unprepared,
    /// };
    /// use kernel::device::{
    ///     Bound,
    ///     Device,
    /// };
    ///
    /// fn configure_clk(dev: &Device<Bound>) -> Result {
    ///     // Each state has its own `Clk::get`, e.g.:
    ///     let clk: Clk<Enabled> = Clk::<Enabled>::get(dev, Some(c"apb_clk"))?;
    ///
    ///     // Any other state is also possible, e.g.:
    ///     let clk: Clk<Prepared> = Clk::<Prepared>::get(dev, Some(c"bus_clk"))?;
    ///
    ///     // Later:
    ///     //
    ///     // `?` works directly thanks to `From<Error<State>>`. The failed
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
    ///     // The states can also be traversed in reverse:
    ///     let clk: Clk<Prepared> = clk.disable();
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
    ///             DeviceClk::Resumed(clk) => DeviceClk::Suspended(clk.disable()),
    ///             suspended => suspended,
    ///         })
    ///     }
    /// }
    /// ```
    ///
    /// Drivers that keep a clock around in a lower state can take a guard
    /// instead of moving the clock through the states, e.g. to have it running
    /// for a short section:
    ///
    /// ```
    /// use kernel::clk::{
    ///     Clk,
    ///     Hertz,
    ///     Prepared,
    ///     Unprepared,
    /// };
    ///
    /// fn read_rate(clk: &Clk<Prepared>) -> Result<Hertz> {
    ///     let enabled = clk.enable_guard()?;
    ///
    ///     // The clock is enabled until `enabled` goes out of scope.
    ///     Ok(enabled.rate())
    /// }
    ///
    /// fn touch_registers(clk: &Clk<Unprepared>) -> Result {
    ///     {
    ///         let prepared = clk.prepare_guard()?;
    ///         let _enabled = prepared.enable_guard()?;
    ///
    ///         // `_enabled` borrows `prepared`, so the clock is disabled before
    ///         // it is unprepared.
    ///     }
    ///
    ///     // Or, alternatively:
    ///     {
    ///         let _enabled = clk.prepare_enable_guard()?;
    ///     }
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
        /// should obtain its clocks through a bound device (see [`Clk::get`]). It
        /// is meant for the few in-tree abstractions that operate on a device
        /// outside a bind scope, such as the generic DT cpufreq driver.
        ///
        /// [`clk_get`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get
        /// [`clk_put`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_put
        #[inline]
        pub(crate) fn get_unbound(dev: &Device, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

            // SAFETY: It is safe to call `clk_get()` for a valid device pointer
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
        ///
        /// Equivalent to the kernel's [`clk_get_optional`] API.
        ///
        /// [`clk_get_optional`]:
        /// https://docs.kernel.org/core-api/kernel-api.html#c.clk_get_optional
        #[inline]
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Unprepared>> {
            let con_id = name.map_or(ptr::null(), |n| n.as_char_ptr());

            // SAFETY: It is safe to call `clk_get_optional()` for a valid device
            // pointer and any `con_id`, including NULL.
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
            // The `clk_get()` count moves to the returned `Clk<Prepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, `clk.as_raw()` is a valid argument
            // for `clk_prepare()`.
            to_result(unsafe { bindings::clk_prepare(clk.as_raw()) })
                // INVARIANT: `clk_prepare()` succeeded, so the returned
                // `Clk<Prepared>` owns a single count of it, which is released
                // when it leaves the [`Prepared`] state.
                .map(|()| Clk {
                    inner: clk.inner,
                    _phantom: PhantomData,
                })
                .map_err(|error| Error {
                    error,
                    clk: ManuallyDrop::into_inner(clk),
                })
        }

        /// Prepares the clock for the lifetime of the returned guard.
        ///
        /// Unlike [`Clk::prepare`], this does not consume `self`. The guard
        /// dereferences to a [`Clk<Prepared>`].
        ///
        /// Equivalent to the kernel's [`clk_prepare`] API.
        ///
        /// [`clk_prepare`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_prepare
        #[inline]
        pub fn prepare_guard(&self) -> Result<PreparedGuard<'_>> {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument
            // for `clk_prepare()`.
            to_result(unsafe { bindings::clk_prepare(self.as_raw()) })?;

            // INVARIANT: `clk_prepare()` succeeded, so the guard owns a single
            // count of it.
            Ok(PreparedGuard { clk: self })
        }

        /// Prepares and enables the clock for the lifetime of the returned guard.
        ///
        /// Unlike [`Clk::prepare`] followed by [`Clk::enable`], this does not
        /// consume `self`. The guard dereferences to a [`Clk<Enabled>`].
        ///
        /// Equivalent to calling [`Clk::prepare_guard`] followed by
        /// [`Clk::enable_guard`], with a single guard holding both counts.
        #[inline]
        pub fn prepare_enable_guard(&self) -> Result<PrepareEnabledGuard<'_>> {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument
            // for `clk_prepare_enable()`.
            to_result(unsafe { bindings::clk_prepare_enable(self.as_raw()) })?;

            // INVARIANT: `clk_prepare_enable()` succeeded, so the guard owns a
            // single count of both `clk_prepare()` and `clk_enable()`.
            Ok(PrepareEnabledGuard { clk: self })
        }
    }

    impl Clk<Prepared> {
        /// Obtains a [`Clk`] from a bound [`Device`] and a connection id and
        /// prepares it.
        ///
        /// Equivalent to calling [`Clk::get`], followed by [`Clk::prepare`].
        #[inline]
        pub fn get(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Prepared>> {
            Ok(Clk::<Unprepared>::get(dev, name)?.prepare()?)
        }

        /// Behaves the same as [`Self::get`], except when there is no clock
        /// producer. In this case, instead of returning [`ENOENT`], it returns
        /// a dummy [`Clk`].
        #[inline]
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Prepared>> {
            Ok(Clk::<Unprepared>::get_optional(dev, name)?.prepare()?)
        }

        /// Converts the [`Clk`] to an [`Unprepared`] state.
        ///
        /// Equivalent to the kernel's [`clk_unprepare`] API.
        ///
        /// [`clk_unprepare`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_unprepare
        #[inline]
        pub fn unprepare(self) -> Clk<Unprepared> {
            // The `clk_get()` count moves to the returned `Clk<Unprepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, `clk.as_raw()` is a valid argument
            // for `clk_unprepare()`.
            unsafe { bindings::clk_unprepare(clk.as_raw()) }

            // INVARIANT: The `clk_prepare()` count was released above, so the
            // returned `Clk<Unprepared>` owns only the `clk_get()` count.
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
            // The `clk_get()` and `clk_prepare()` counts move to the returned
            // `Clk<Enabled>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, `clk.as_raw()` is a valid argument
            // for `clk_enable()`.
            to_result(unsafe { bindings::clk_enable(clk.as_raw()) })
                // INVARIANT: `clk_enable()` succeeded, so the returned
                // `Clk<Enabled>` owns a single count of it, which is released
                // when it leaves the [`Enabled`] state.
                .map(|()| Clk {
                    inner: clk.inner,
                    _phantom: PhantomData,
                })
                .map_err(|error| Error {
                    error,
                    clk: ManuallyDrop::into_inner(clk),
                })
        }

        /// Enables the clock for the lifetime of the returned guard.
        ///
        /// Unlike [`Clk::enable`], this does not consume `self`, so it can be used
        /// on a [`Clk<Prepared>`] that is shared or stored in driver data. The
        /// clock stays [`Prepared`] through `self`, and the guard dereferences to
        /// a [`Clk<Enabled>`].
        ///
        /// Equivalent to the kernel's [`clk_enable`] API.
        ///
        /// [`clk_enable`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_enable
        #[inline]
        pub fn enable_guard(&self) -> Result<EnabledGuard<'_>> {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument
            // for `clk_enable()`.
            to_result(unsafe { bindings::clk_enable(self.as_raw()) })?;

            // INVARIANT: `clk_enable()` succeeded, so the guard owns a single
            // count of it.
            Ok(EnabledGuard { clk: self })
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
            Ok(Clk::<Prepared>::get(dev, name)?.enable()?)
        }

        /// Behaves the same as [`Self::get`], except when there is no clock
        /// producer. In this case, instead of returning [`ENOENT`], it returns
        /// a dummy [`Clk`].
        #[inline]
        pub fn get_optional(dev: &Device<Bound>, name: Option<&CStr>) -> Result<Clk<Enabled>> {
            Ok(Clk::<Prepared>::get_optional(dev, name)?.enable()?)
        }

        /// Disables the [`Clk`] and converts it to the [`Prepared`] state.
        ///
        /// Equivalent to the kernel's [`clk_disable`] API.
        ///
        /// [`clk_disable`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_disable
        #[inline]
        pub fn disable(self) -> Clk<Prepared> {
            // The `clk_get()` and `clk_prepare()` counts move to the returned
            // `Clk<Prepared>`.
            let clk = ManuallyDrop::new(self);

            // SAFETY: By the type invariants, `clk.as_raw()` is a valid argument
            // for `clk_disable()`.
            unsafe { bindings::clk_disable(clk.as_raw()) };

            // INVARIANT: The `clk_enable()` count was released above, so the
            // returned `Clk<Prepared>` owns the `clk_get()` and `clk_prepare()`
            // counts.
            Clk {
                inner: clk.inner,
                _phantom: PhantomData,
            }
        }
    }

    impl<T: ClkState> Clk<T> {
        /// Obtain the raw [`struct clk`] pointer.
        #[inline]
        pub fn as_raw(&self) -> *mut bindings::clk {
            self.inner
        }

        /// Get clock's rate.
        ///
        /// Equivalent to the kernel's [`clk_get_rate`] API.
        ///
        /// The returned rate is only guaranteed to reflect what the hardware is
        /// doing once the clock is [`Enabled`].
        ///
        /// [`clk_get_rate`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_get_rate
        #[inline]
        pub fn rate(&self) -> Hertz {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument
            // for `clk_get_rate()`.
            Hertz(unsafe { bindings::clk_get_rate(self.as_raw()) })
        }

        /// Set clock's rate.
        ///
        /// Equivalent to the kernel's [`clk_set_rate`] API.
        ///
        /// [`clk_set_rate`]: https://docs.kernel.org/core-api/kernel-api.html#c.clk_set_rate
        #[inline]
        pub fn set_rate(&self, rate: Hertz) -> Result {
            // SAFETY: By the type invariants, `self.as_raw()` is a valid argument
            // for `clk_set_rate()`.
            to_result(unsafe { bindings::clk_set_rate(self.as_raw(), rate.as_hz()) })
        }
    }

    impl<T: ClkState> Drop for Clk<T> {
        fn drop(&mut self) {
            if T::ENABLED {
                // SAFETY: By the type invariants, `self.as_raw()` is a valid
                // argument for `clk_disable()`.
                unsafe { bindings::clk_disable(self.as_raw()) };
            }

            if T::PREPARED {
                // SAFETY: By the type invariants, `self.as_raw()` is a valid
                // argument for `clk_unprepare()`.
                unsafe { bindings::clk_unprepare(self.as_raw()) };
            }

            // SAFETY: By the type invariants, `self.as_raw()` is a valid
            // argument for `clk_put()`.
            unsafe { bindings::clk_put(self.as_raw()) };
        }
    }

    /// A guard that keeps a [`Clk`] enabled.
    ///
    /// Returned by [`Clk::enable_guard`]. Dropping it disables the clock again,
    /// while the borrowed [`Clk<Prepared>`] keeps it prepared. It dereferences to
    /// [`Clk<Enabled>`], so it can be passed wherever an enabled clock is
    /// expected.
    ///
    /// # Invariants
    ///
    /// The guard owns a single count of `clk_enable()` on `clk`, taken by
    /// [`Clk::enable_guard`].
    #[must_use = "if unused, the clock will be disabled immediately"]
    pub struct EnabledGuard<'a> {
        clk: &'a Clk<Prepared>,
    }

    impl Deref for EnabledGuard<'_> {
        type Target = Clk<Enabled>;

        #[inline]
        fn deref(&self) -> &Clk<Enabled> {
            // SAFETY: `Clk<T>` is `#[repr(transparent)]` over the `struct clk`
            // pointer for every `T`, so `Clk<Prepared>` and `Clk<Enabled>` have
            // the same layout. By the type invariants, the guard owns an enable
            // count for as long as the returned reference is alive, and a shared
            // reference cannot be used to drop the `Clk`.
            unsafe { &*ptr::from_ref(self.clk).cast::<Clk<Enabled>>() }
        }
    }

    impl Drop for EnabledGuard<'_> {
        #[inline]
        fn drop(&mut self) {
            // SAFETY: By the type invariants, the guard owns a `clk_enable()`
            // count on `self.clk`, which this releases.
            unsafe { bindings::clk_disable(self.clk.as_raw()) };
        }
    }

    /// A guard that keeps a [`Clk`] prepared.
    ///
    /// Returned by [`Clk::prepare_guard`]. Dropping it unprepares the clock
    /// again, which may sleep. It dereferences to [`Clk<Prepared>`], so it can be
    /// passed wherever a prepared clock is expected.
    ///
    /// # Invariants
    ///
    /// The guard owns a single count of `clk_prepare()` on `clk`, taken by
    /// [`Clk::prepare_guard`].
    #[must_use = "if unused, the clock will be unprepared immediately"]
    pub struct PreparedGuard<'a> {
        clk: &'a Clk<Unprepared>,
    }

    impl Deref for PreparedGuard<'_> {
        type Target = Clk<Prepared>;

        #[inline]
        fn deref(&self) -> &Clk<Prepared> {
            // SAFETY: `Clk<T>` is `#[repr(transparent)]` over the `struct clk`
            // pointer for every `T`, so `Clk<Unprepared>` and `Clk<Prepared>`
            // have the same layout. By the type invariants, the guard owns a
            // prepare count for as long as the returned reference is alive, and
            // a shared reference cannot be used to drop the `Clk`.
            unsafe { &*ptr::from_ref(self.clk).cast::<Clk<Prepared>>() }
        }
    }

    impl Drop for PreparedGuard<'_> {
        #[inline]
        fn drop(&mut self) {
            // SAFETY: By the type invariants, the guard owns a `clk_prepare()`
            // count on `self.clk`, which this releases.
            unsafe { bindings::clk_unprepare(self.clk.as_raw()) };
        }
    }

    /// A guard that keeps a [`Clk`] prepared and enabled.
    ///
    /// Returned by [`Clk::prepare_enable_guard`]. Dropping it disables and
    /// unprepares the clock again, which may sleep. It dereferences to
    /// [`Clk<Enabled>`], so it can be passed wherever an enabled clock is
    /// expected.
    ///
    /// # Invariants
    ///
    /// The guard owns a single count of both `clk_prepare()` and `clk_enable()`
    /// on `clk`, taken by [`Clk::prepare_enable_guard`].
    #[must_use = "if unused, the clock will be disabled and unprepared immediately"]
    pub struct PrepareEnabledGuard<'a> {
        clk: &'a Clk<Unprepared>,
    }

    impl Deref for PrepareEnabledGuard<'_> {
        type Target = Clk<Enabled>;

        #[inline]
        fn deref(&self) -> &Clk<Enabled> {
            // SAFETY: `Clk<T>` is `#[repr(transparent)]` over the `struct clk`
            // pointer for every `T`, so `Clk<Unprepared>` and `Clk<Enabled>`
            // have the same layout. By the type invariants, the guard owns a
            // prepare and an enable count for as long as the returned reference
            // is alive, and a shared reference cannot be used to drop the `Clk`.
            unsafe { &*ptr::from_ref(self.clk).cast::<Clk<Enabled>>() }
        }
    }

    impl Drop for PrepareEnabledGuard<'_> {
        #[inline]
        fn drop(&mut self) {
            // SAFETY: By the type invariants, the guard owns a count of both
            // `clk_prepare()` and `clk_enable()` on `self.clk`, which this
            // releases.
            unsafe { bindings::clk_disable_unprepare(self.clk.as_raw()) };
        }
    }
}

#[cfg(CONFIG_COMMON_CLK)]
pub use common_clk::*;
