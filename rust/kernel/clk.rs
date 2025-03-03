// SPDX-License-Identifier: GPL-2.0

//! Clock abstractions.
//!
//! C header: [`include/linux/clk.h`](srctree/include/linux/clk.h)

use crate::{
    bindings,
    device::Device,
    error::{from_err_ptr, to_result, Result},
    prelude::*,
};

use core::{ops::Deref, ptr};

/// Frequency unit.
pub type Hertz = crate::ffi::c_ulong;

/// A simple implementation of `struct clk` from the C code.
#[repr(transparent)]
pub struct Clk(*mut bindings::clk);

impl Clk {
    /// Gets clock corresponding to a device and a connection id and returns `Clk`.
    pub fn get(dev: &Device, name: Option<&CStr>) -> Result<Self> {
        let con_id = if let Some(name) = name {
            name.as_ptr() as *const _
        } else {
            ptr::null()
        };

        // SAFETY: It is safe to call `clk_get()` for a valid device pointer.
        Ok(Self(from_err_ptr(unsafe {
            bindings::clk_get(dev.as_raw(), con_id)
        })?))
    }

    /// Obtain the raw `struct clk *`.
    pub fn as_raw(&self) -> *mut bindings::clk {
        self.0
    }

    /// Clock enable.
    pub fn enable(&self) -> Result<()> {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        to_result(unsafe { bindings::clk_enable(self.as_raw()) })
    }

    /// Clock disable.
    pub fn disable(&self) {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        unsafe { bindings::clk_disable(self.as_raw()) };
    }

    /// Clock prepare.
    pub fn prepare(&self) -> Result<()> {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        to_result(unsafe { bindings::clk_prepare(self.as_raw()) })
    }

    /// Clock unprepare.
    pub fn unprepare(&self) {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        unsafe { bindings::clk_unprepare(self.as_raw()) };
    }

    /// Clock prepare enable.
    pub fn prepare_enable(&self) -> Result<()> {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        to_result(unsafe { bindings::clk_prepare_enable(self.as_raw()) })
    }

    /// Clock disable unprepare.
    pub fn disable_unprepare(&self) {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        unsafe { bindings::clk_disable_unprepare(self.as_raw()) };
    }

    /// Clock get rate.
    pub fn rate(&self) -> Hertz {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        unsafe { bindings::clk_get_rate(self.as_raw()) }
    }

    /// Clock set rate.
    pub fn set_rate(&self, rate: Hertz) -> Result<()> {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        to_result(unsafe { bindings::clk_set_rate(self.as_raw(), rate) })
    }
}

impl Drop for Clk {
    fn drop(&mut self) {
        // SAFETY: It is safe to call clk APIs of the C code for a clock pointer earlier returned
        // by `clk_get()`.
        unsafe { bindings::clk_put(self.as_raw()) };
    }
}

/// A simple implementation of optional `Clk`.
pub struct OptionalClk(Clk);

impl OptionalClk {
    /// Gets optional clock corresponding to a device and a connection id and returns `Clk`.
    pub fn get(dev: &Device, name: Option<&CStr>) -> Result<Self> {
        let con_id = if let Some(name) = name {
            name.as_ptr() as *const _
        } else {
            ptr::null()
        };

        // SAFETY: It is safe to call `clk_get_optional()` for a valid device pointer.
        Ok(Self(Clk(from_err_ptr(unsafe {
            bindings::clk_get_optional(dev.as_raw(), con_id)
        })?)))
    }
}

// Make `OptionalClk` behave like `Clk`.
impl Deref for OptionalClk {
    type Target = Clk;

    fn deref(&self) -> &Clk {
        &self.0
    }
}
