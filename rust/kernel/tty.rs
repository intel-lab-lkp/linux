// SPDX-License-Identifier: GPL-2.0

//! TTY subsystem support.
//!
//! C headers: [`include/linux/tty.h`](srctree/include/linux/tty.h),
//!            [`include/linux/tty_driver.h`](srctree/include/linux/tty_driver.h),
//!            [`include/linux/tty_port.h`](srctree/include/linux/tty_port.h)
//!
//! This module provides TTY bindings for Rust TTY drivers.

mod driver;
pub mod port;

use core::marker::PhantomData;

pub use driver::{
    flags,
    oflag,
    DriverType,
    Operations,
    Options,
    TtyDriver,
    TtyDriverBuilder,
    TTYAUX_MAJOR,
};
pub use port::{
    DriverPort,
    Operations as PortOperations,
};

use crate::{
    bindings,
    sync::Arc,
};

/// TTY struct wrapper, generic over driver data and driver state types.
///
/// - `DriverData`: Per-tty instance data stored in `tty_struct->driver_data`.
///   Use `Arc<T>` for shared data across multiple opens.
/// - `DriverState`: Driver-level data stored in `tty_driver->driver_state` (shared by all ttys).
///   Use `Arc<T>` for shared state.
#[repr(transparent)]
pub struct Tty<DriverData = (), DriverState = ()>(
    *mut bindings::tty_struct,
    PhantomData<(DriverData, DriverState)>,
);

impl<DriverData, DriverState> Tty<DriverData, DriverState> {
    /// Creates a TTY wrapper from a raw pointer.
    ///
    /// # Safety
    ///
    /// - `ptr` must be a valid pointer to a `tty_struct`.
    pub unsafe fn from_raw(ptr: *mut bindings::tty_struct) -> Self {
        Self(ptr, PhantomData)
    }

    /// Returns the raw pointer.
    pub fn as_raw(&self) -> *mut bindings::tty_struct {
        self.0
    }
}

impl<T: Send + Sync, DriverState> Tty<Arc<T>, DriverState> {
    /// Sets driver-specific data in the `driver_data` field, taking ownership of the Arc.
    ///
    /// Returns the previously set data, if any.
    pub fn set_driver_data(&self, data: Arc<T>) -> Option<Arc<T>> {
        let old = self.take_driver_data();
        // SAFETY: self.0 is valid.
        unsafe {
            (*self.0).driver_data = Arc::into_raw(data) as *mut _;
        }
        old
    }

    /// Takes the driver-specific data from the `driver_data` field, returning ownership.
    ///
    /// Returns `None` if no data was set.
    pub fn take_driver_data(&self) -> Option<Arc<T>> {
        // SAFETY: self.0 is valid.
        let ptr = unsafe { (*self.0).driver_data };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: self.0 is valid.
        unsafe {
            (*self.0).driver_data = core::ptr::null_mut();
        }
        // SAFETY: ptr was set via set_driver_data from an Arc<T>.
        Some(unsafe { Arc::from_raw(ptr.cast()) })
    }

    /// Returns a reference to the driver-specific data in the `driver_data` field.
    ///
    /// Returns `None` if no data was set.
    pub fn driver_data(&self) -> Option<&T> {
        // SAFETY: self.0 is valid.
        let ptr = unsafe { (*self.0).driver_data };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: ptr was set via set_driver_data from an Arc<T>.
        Some(unsafe { &*ptr.cast::<T>() })
    }
}

impl<DriverData, T: Send + Sync> Tty<DriverData, Arc<T>> {
    /// Returns a clone of the Arc holding the driver-level state.
    ///
    /// This is set by [`TtyDriverBuilder::set_driver_state`] and provides access to
    /// driver-level data from within TTY operation callbacks. Returns a cloned Arc,
    /// incrementing the reference count.
    pub fn driver_state(&self) -> Option<Arc<T>> {
        // SAFETY: self.0 is valid.
        let driver = unsafe { (*self.0).driver };
        if driver.is_null() {
            return None;
        }
        // SAFETY: driver is valid.
        let state = unsafe { (*driver).driver_state };
        if state.is_null() {
            return None;
        }
        // SAFETY: state was set via set_driver_state from an Arc<T>.
        // We reconstruct the Arc, clone it, then forget the original to avoid
        // decrementing the stored refcount.
        let arc = unsafe { Arc::from_raw(state.cast::<T>()) };
        let cloned = arc.clone();
        core::mem::forget(arc);
        Some(cloned)
    }

    /// Takes the driver-level state from `tty_driver->driver_state`, returning ownership.
    ///
    /// Returns `None` if no state was set.
    pub fn take_driver_state(&self) -> Option<Arc<T>> {
        // SAFETY: self.0 is valid.
        let driver = unsafe { (*self.0).driver };
        if driver.is_null() {
            return None;
        }
        // SAFETY: driver is valid.
        let ptr = unsafe { (*driver).driver_state };
        if ptr.is_null() {
            return None;
        }
        // SAFETY: driver is valid.
        unsafe {
            (*driver).driver_state = core::ptr::null_mut();
        }
        // SAFETY: ptr was set via set_driver_state from an Arc<T>.
        Some(unsafe { Arc::from_raw(ptr.cast()) })
    }

    /// Sets the driver-level state in `tty_driver->driver_state`, taking ownership.
    ///
    /// Returns the previously set state, if any.
    pub fn set_driver_state(&self, state: Arc<T>) -> Option<Arc<T>> {
        // SAFETY: self.0 is valid.
        let driver = unsafe { (*self.0).driver };
        if driver.is_null() {
            return None;
        }
        // Take old state first.
        let old = self.take_driver_state();
        // SAFETY: driver is valid.
        unsafe {
            (*driver).driver_state = Arc::into_raw(state) as *mut _;
        }
        old
    }
}
