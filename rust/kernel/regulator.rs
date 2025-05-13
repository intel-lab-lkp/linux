// SPDX-License-Identifier: GPL-2.0

//! Regulator abstractions, providing a standard kernel interface to control
//! voltage and current regulators.
//!
//! The intention is to allow systems to dynamically control regulator power
//! output in order to save power and prolong battery life. This applies to both
//! voltage regulators (where voltage output is controllable) and current sinks
//! (where current limit is controllable).
//!
//! C header: [`include/linux/regulator/consumer.h`](srctree/include/linux/regulator/consumer.h)
//!
//! Regulators are modeled in Rust with two types: [`Regulator`] and
//! [`EnabledRegulator`].
//!
//! The transition between these types is done by calling
//! [`Regulator::enable()`] and [`EnabledRegulator::disable()`] respectively.
//!
//! Use an enum or [`kernel::types::Either`] to gracefully transition between
//! the two states at runtime if needed. Store [`EnabledRegulator`] directly
//! otherwise.
//!
//! See [`Voltage and current regulator API`]("https://docs.kernel.org/driver-api/regulator.html")
//! for more information.

use crate::{
    bindings,
    device::Device,
    error::{from_err_ptr, to_result, Result},
    prelude::*,
};

use core::{mem::ManuallyDrop, ptr::NonNull};

/// A `struct regulator` abstraction.
///
/// # Examples
///
/// Enabling a regulator:
///
/// ```
/// # use kernel::prelude::*;
/// # use kernel::c_str;
/// # use kernel::device::Device;
/// # use kernel::regulator::{Microvolt, Regulator, EnabledRegulator};
/// fn enable(dev: &Device, min_uv: Microvolt, max_uv: Microvolt) -> Result {
///    // Obtain a reference to a (fictitious) regulator.
///    let regulator: Regulator = Regulator::get(dev, c_str!("vcc"))?;
///
///    // The voltage can be set before enabling the regulator if needed, e.g.:
///    regulator.set_voltage(min_uv, max_uv)?;
///
///    // The same applies for `get_voltage()`, i.e.:
///    let voltage: Microvolt = regulator.get_voltage()?;
///
///    // Enables the regulator, consuming the previous value.
///    //
///    // From now on, the regulator is known to be enabled because of the type
///    // `EnabledRegulator`.
///    let regulator: EnabledRegulator = regulator.enable()?;
///
///    // The voltage can also be set after enabling the regulator, e.g.:
///    regulator.set_voltage(min_uv, max_uv)?;
///
///    // The same applies for `get_voltage()`, i.e.:
///    let voltage: Microvolt = regulator.get_voltage()?;
///
///    // Dropping an enabled regulator will disable it. The refcount will be
///    // decremented.
///    drop(regulator);
///    // ...
///    # Ok::<(), Error>(())
/// }
///```
///
/// Disabling a regulator:
///
///```
/// # use kernel::prelude::*;
/// # use kernel::c_str;
/// # use kernel::device::Device;
/// # use kernel::regulator::{Microvolt, Regulator, EnabledRegulator};
/// fn disable(dev: &Device, regulator: EnabledRegulator) -> Result {
///    // We can also disable an enabled regulator without reliquinshing our
///    // refcount:
///    let regulator: Regulator = regulator.disable()?;
///
///    // The refcount will be decremented when `regulator` is dropped.
///    drop(regulator);
///    // ...
///    # Ok::<(), Error>(())
/// }
/// ```
///
/// # Invariants
///
/// - [`Regulator`] is a non-null wrapper over a pointer to a `struct
///   regulator` obtained from [`regulator_get()`](https://docs.kernel.org/driver-api/regulator.html#c.regulator_get).
/// - Each instance of [`Regulator`] is associated with a single count of
///   [`regulator_get()`](https://docs.kernel.org/driver-api/regulator.html#c.regulator_get).
pub struct Regulator {
    inner: NonNull<bindings::regulator>,
}

impl Regulator {
    /// Obtains a [`Regulator`] instance from the system.
    pub fn get(dev: &Device, name: &CStr) -> Result<Self> {
        // SAFETY: It is safe to call `regulator_get()`, on a device pointer
        // received from the C code.
        let inner = from_err_ptr(unsafe { bindings::regulator_get(dev.as_raw(), name.as_ptr()) })?;

        // SAFETY: We can safely trust `inner` to be a pointer to a valid
        // regulator if `ERR_PTR` was not returned.
        let inner = unsafe { NonNull::new_unchecked(inner) };

        Ok(Self { inner })
    }

    /// Enables the regulator.
    pub fn enable(self) -> Result<EnabledRegulator> {
        // SAFETY: Safe as per the type invariants of `Regulator`.
        let res = to_result(unsafe { bindings::regulator_enable(self.inner.as_ptr()) });
        res.map(|()| EnabledRegulator { inner: self })
    }

    /// Sets the voltage for the regulator.
    ///
    /// This can be used to ensure that the device powers up cleanly.
    pub fn set_voltage(&self, min_uv: Microvolt, max_uv: Microvolt) -> Result {
        // SAFETY: Safe as per the type invariants of `Regulator`.
        to_result(unsafe {
            bindings::regulator_set_voltage(self.inner.as_ptr(), min_uv.0, max_uv.0)
        })
    }

    /// Gets the current voltage of the regulator.
    pub fn get_voltage(&self) -> Result<Microvolt> {
        // SAFETY: Safe as per the type invariants of `Regulator`.
        let voltage = unsafe { bindings::regulator_get_voltage(self.inner.as_ptr()) };
        if voltage < 0 {
            Err(Error::from_errno(voltage))
        } else {
            Ok(Microvolt(voltage))
        }
    }
}

impl Drop for Regulator {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference,
        // so it is safe to relinquish it now.
        unsafe { bindings::regulator_put(self.inner.as_ptr()) };
    }
}

/// A [`Regulator`] that is known to be enabled.
///
/// # Invariants
///
/// - [`EnabledRegulator`] is a valid regulator that has been enabled.
/// - Each instance of [`EnabledRegulator`] is associated with a single count
///   of [`regulator_enable()`](https://docs.kernel.org/driver-api/regulator.html#c.regulator_enable)
///   that was obtained from the [`Regulator`] instance once it was enabled.
pub struct EnabledRegulator {
    inner: Regulator,
}

impl EnabledRegulator {
    fn as_ptr(&self) -> *mut bindings::regulator {
        self.inner.inner.as_ptr()
    }

    /// Disables the regulator.
    pub fn disable(self) -> Result<Regulator> {
        // Keep the count on `regulator_get()`.
        let regulator = ManuallyDrop::new(self);

        // SAFETY: Safe as per the type invariants of `Self`.
        let res = to_result(unsafe { bindings::regulator_disable(regulator.as_ptr()) });

        res.map(|()| Regulator {
            inner: regulator.inner.inner,
        })
    }

    /// Sets the voltage for the regulator.
    pub fn set_voltage(&self, min_uv: Microvolt, max_uv: Microvolt) -> Result {
        self.inner.set_voltage(min_uv, max_uv)
    }

    /// Gets the current voltage of the regulator.
    pub fn get_voltage(&self) -> Result<Microvolt> {
        self.inner.get_voltage()
    }
}

impl Drop for EnabledRegulator {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference,
        // so it is safe to relinquish it now.
        unsafe { bindings::regulator_disable(self.as_ptr()) };
    }
}

/// A voltage in microvolts.
///
/// The explicit type is used to avoid confusion with other multiples of the
/// volt, which can be desastrous.
#[repr(transparent)]
#[derive(Copy, Clone, PartialEq, Eq)]
pub struct Microvolt(pub i32);
