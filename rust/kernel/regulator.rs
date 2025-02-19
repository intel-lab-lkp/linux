// SPDX-License-Identifier: GPL-2.0

//! Regulator abstractions.
//!
//! C header: [`include/linux/regulator/consumer.h`](srctree/include/linux/regulator/consumer.h)

use crate::{
    bindings,
    device::Device,
    error::{from_err_ptr, to_result, Result},
    prelude::*,
};

use core::ptr::NonNull;

/// A `struct regulator` abstraction.
///
/// Note that each instance of [`Regulator`] obtained from `Regulator::get()`
/// can only be enabled once. This ensures that the calls to enable and disable
/// are perfectly balanced before `regulator_put()` is called, as mandated by
/// the C API.
///
/// # Invariants
///
/// - [`Regulator`] is a non-null wrapper over a pointer to a `struct regulator`
///   obtained from `regulator_get()`.
/// - Each instance of [`Regulator`] obtained from `Regulator::get()` can only
///   be enabled once.
pub struct Regulator {
    inner: NonNull<bindings::regulator>,
    enabled: bool,
}

impl Regulator {
    /// Obtains a [`Regulator`] instance from the system.
    pub fn get(dev: &Device, name: &CStr) -> Result<Self> {
        // SAFETY: It is safe to call `regulator_get()`, on a device pointer
        // earlier received from the C code.
        let inner = from_err_ptr(unsafe { bindings::regulator_get(dev.as_raw(), name.as_ptr()) })?;

        // SAFETY: We can safely trust `inner` to be a pointer to a valid
        // regulator if `ERR_PTR` was not returned.
        let inner = unsafe { NonNull::new_unchecked(inner) };

        Ok(Self {
            inner,
            enabled: false,
        })
    }

    /// Enable the regulator.
    pub fn enable(&mut self) -> Result {
        if self.enabled {
            return Ok(());
        }

        // SAFETY: Safe as per the type invariants of `Regulator`.
        let res = to_result(unsafe { bindings::regulator_enable(self.inner.as_ptr()) });
        if res.is_ok() {
            self.enabled = true;
        }

        res
    }

    /// Disable the regulator.
    pub fn disable(&mut self) -> Result {
        if !self.enabled {
            return Ok(());
        }

        // SAFETY: Safe as per the type invariants of `Regulator`.
        let res = to_result(unsafe { bindings::regulator_disable(self.inner.as_ptr()) });
        if res.is_ok() {
            self.enabled = false;
        }

        res
    }

    /// Set the voltage for the regulator.
    pub fn set_voltage(&self, min_uv: Microvolt, max_uv: Microvolt) -> Result {
        // SAFETY: Safe as per the type invariants of `Regulator`.
        to_result(unsafe {
            bindings::regulator_set_voltage(self.inner.as_ptr(), min_uv.0, max_uv.0)
        })
    }

    /// Get the current voltage of the regulator.
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
        if self.enabled {
            // It is a requirement from the C API that the calls to enable and
            // disabled are balanced before calling `regulator_put()`.
            self.disable();
        }

        // SAFETY: By the type invariants, we know that `self` owns a reference,
        // so it is safe to relinquish it now.
        unsafe { bindings::regulator_put(self.inner.as_ptr()) };
    }
}

/// A voltage in microvolts.
///
/// The explicit type is used to avoid confusion with other multiples of the
/// volt, which can be desastrous.
#[repr(transparent)]
pub struct Microvolt(pub i32);
