// SPDX-License-Identifier: GPL-2.0

//! TTY port support.
//!
//! Provides [`DriverPort`] which combines a TTY port with driver-specific data,
//! following the C pattern of embedding `tty_port` as the first struct field.

use core::marker::PhantomData;

use pin_init::PinInit;

use crate::{
    bindings,
    error::VTABLE_DEFAULT_ERROR,
    prelude::*,
    types::Opaque,
};

/// A combined TTY port and driver data structure.
///
/// Follows the C pattern of embedding `tty_port` as the first field.
/// The `#[repr(C)]` layout enables safe `container_of` operations.
#[repr(C)]
#[pin_data]
pub struct DriverPort<Ops: Operations> {
    #[pin]
    port: TtyPort<Ops>,
    #[pin]
    data: Ops::PortData,
}

impl<Ops: Operations> DriverPort<Ops> {
    /// Creates a pin-initializer for a new driver port.
    pub fn new(
        data_init: impl PinInit<Ops::PortData, core::convert::Infallible>,
    ) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            port <- TtyPort::<Ops>::new(),
            data <- data_init,
        }? Error)
    }

    /// Returns a reference to the port-specific data.
    pub fn data(&self) -> &Ops::PortData {
        &self.data
    }

    /// Returns a raw pointer to the underlying `tty_port`.
    pub(super) fn as_raw(&self) -> *mut bindings::tty_port {
        self.port.as_raw()
    }

    /// Converts a raw `tty_port` pointer back to `&DriverPort` (container_of).
    ///
    /// # Safety
    /// `ptr` must point to a `tty_port` within a valid `DriverPort<Ops>`.
    unsafe fn from_raw<'a>(ptr: *mut bindings::tty_port) -> &'a Self {
        // SAFETY: DriverPort is #[repr(C)] with TtyPort as first field.
        unsafe { &*(ptr as *const Self) }
    }
}

// SAFETY: DriverPort is Send/Sync if Ops::PortData is, since TtyPort is both.
unsafe impl<Ops: Operations> Send for DriverPort<Ops> where Ops::PortData: Send {}
// SAFETY: DriverPort is Send/Sync if Ops::PortData is, since TtyPort is both.
unsafe impl<Ops: Operations> Sync for DriverPort<Ops> where Ops::PortData: Sync {}

/// Wrapper for `struct tty_port`. Typically used via [`DriverPort`].
///
/// # Invariants
/// Initialized via `tty_port_init()`, destroyed via `tty_port_destroy()` on drop.
#[repr(transparent)]
struct TtyPort<Ops: Operations>(Opaque<bindings::tty_port>, PhantomData<Ops>);

impl<Ops: Operations> TtyPort<Ops> {
    /// Creates a pin-initializer that calls `tty_port_init()` and sets the ops vtable.
    fn new() -> impl PinInit<Self, Error> {
        // SAFETY: tty_port_init initializes the port, vtable is static.
        unsafe {
            pin_init::pin_init_from_closure(|slot: *mut Self| {
                let port_ptr = slot.cast::<bindings::tty_port>();
                bindings::tty_port_init(port_ptr);
                (*port_ptr).ops = OperationsVTable::<Ops>::build();
                Ok(())
            })
        }
    }

    fn as_raw(&self) -> *mut bindings::tty_port {
        self.0.get()
    }
}

// SAFETY: TtyPort operations are internally synchronized by the kernel.
unsafe impl<Ops: Operations> Send for TtyPort<Ops> {}
// SAFETY: TtyPort operations are internally synchronized by the kernel.
unsafe impl<Ops: Operations> Sync for TtyPort<Ops> {}

impl<Ops: Operations> Drop for TtyPort<Ops> {
    fn drop(&mut self) {
        // SAFETY: Port was initialized in new(), must be destroyed.
        unsafe { bindings::tty_port_destroy(self.0.get()) };
    }
}

/// TTY port operations trait.
///
/// Implement to define callbacks for port events. The `PortData` type specifies
/// data stored alongside the port in [`DriverPort`].
#[vtable]
pub trait Operations: Sized {
    /// Port-specific data type stored in [`DriverPort`].
    type PortData: Sync;

    /// Called when the port is shut down (last user closes the device).
    fn shutdown(_port: &DriverPort<Self>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// Vtable adapter for port operations.
struct OperationsVTable<Ops: Operations>(PhantomData<Ops>);

impl<Ops: Operations> OperationsVTable<Ops> {
    /// # Safety
    /// `port` must be a valid `tty_port` within a `DriverPort<Ops>`.
    unsafe extern "C" fn shutdown(port: *mut bindings::tty_port) {
        // SAFETY: Port was registered with this vtable.
        let driver_port = unsafe { DriverPort::<Ops>::from_raw(port) };
        Ops::shutdown(driver_port);
    }

    const VTABLE: bindings::tty_port_operations = bindings::tty_port_operations {
        shutdown: if Ops::HAS_SHUTDOWN {
            Some(Self::shutdown)
        } else {
            None
        },
        carrier_raised: None,
        dtr_rts: None,
        activate: None,
        destruct: None,
    };

    const fn build() -> &'static bindings::tty_port_operations {
        &Self::VTABLE
    }
}
