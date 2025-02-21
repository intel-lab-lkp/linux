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

use core::ptr;

/// A simple implementation of `struct clk` from the C code.
#[repr(transparent)]
pub struct Clk(*mut bindings::clk);

impl Clk {
    /// Creates `Clk` instance for a device and a connection id.
    pub fn new(dev: &Device, name: Option<&CStr>) -> Result<Self> {
        let con_id = if let Some(name) = name {
            name.as_ptr() as *const _
        } else {
            ptr::null()
        };

        // SAFETY: It is safe to call `clk_get()`, on a device pointer earlier received from the C
        // code.
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
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        to_result(unsafe { bindings::clk_enable(self.0) })
    }

    /// Clock disable.
    pub fn disable(&self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        unsafe { bindings::clk_disable(self.0) };
    }

    /// Clock prepare.
    pub fn prepare(&self) -> Result<()> {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        to_result(unsafe { bindings::clk_prepare(self.0) })
    }

    /// Clock unprepare.
    pub fn unprepare(&self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        unsafe { bindings::clk_unprepare(self.0) };
    }

    /// Clock prepare enable.
    pub fn prepare_enable(&self) -> Result<()> {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        to_result(unsafe { bindings::clk_prepare_enable(self.0) })
    }

    /// Clock disable unprepare.
    pub fn disable_unprepare(&self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        unsafe { bindings::clk_disable_unprepare(self.0) };
    }

    /// Clock get rate.
    pub fn rate(&self) -> usize {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        unsafe { bindings::clk_get_rate(self.0) }
    }

    /// Clock set rate.
    pub fn set_rate(&self, rate: usize) -> Result<()> {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // use it now.
        to_result(unsafe { bindings::clk_set_rate(self.0, rate) })
    }
}

impl Drop for Clk {
    fn drop(&mut self) {
        // SAFETY: By the type invariants, we know that `self` owns a reference, so it is safe to
        // relinquish it now.
        unsafe { bindings::clk_put(self.0) };
    }
}
