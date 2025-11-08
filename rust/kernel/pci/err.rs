// SPDX-License-Identifier: GPL-2.0

//! PCI error handling abstractions.
//!
//! This module provides traits and types to handle PCI bus errors in Rust PCI drivers.

use core::marker::PhantomData;

use kernel::prelude::*;

use crate::{
    device,
    error::VTABLE_DEFAULT_ERROR, //
};

use super::Device;

/// Result type for PCI error handling operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum ErsResult {
    /// No result/none/not supported in device driver
    None = bindings::pci_ers_result_PCI_ERS_RESULT_NONE,
    /// Device driver can recover without slot reset
    CanRecover = bindings::pci_ers_result_PCI_ERS_RESULT_CAN_RECOVER,
    /// Device driver wants slot to be reset
    NeedReset = bindings::pci_ers_result_PCI_ERS_RESULT_NEED_RESET,
    /// Device has completely failed, is unrecoverable
    Disconnect = bindings::pci_ers_result_PCI_ERS_RESULT_DISCONNECT,
    /// Device driver is fully recovered and operational
    Recovered = bindings::pci_ers_result_PCI_ERS_RESULT_RECOVERED,
    /// No AER capabilities registered for the driver
    NoAerDriver = bindings::pci_ers_result_PCI_ERS_RESULT_NO_AER_DRIVER,
}

impl ErsResult {
    fn into_c(self) -> bindings::pci_ers_result_t {
        self as bindings::pci_ers_result_t
    }
}

/// PCI channel state representation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum ChannelState {
    /// I/O channel is in normal state
    Normal = bindings::pci_channel_io_normal,
    /// I/O to channel is blocked
    Frozen = bindings::pci_channel_io_frozen,
    /// PCI card is dead
    PermanentFailure = bindings::pci_channel_io_perm_failure,
}

impl TryFrom<u32> for ChannelState {
    type Error = kernel::error::Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            bindings::pci_channel_io_normal => Ok(ChannelState::Normal),
            bindings::pci_channel_io_frozen => Ok(ChannelState::Frozen),
            bindings::pci_channel_io_perm_failure => Ok(ChannelState::PermanentFailure),
            _ => Err(kernel::error::code::EINVAL),
        }
    }
}

/// PCI bus error handler trait.
#[vtable]
pub trait ErrorHandler {
    /// The driver type associated with this error handler.
    type Driver;

    /// PCI bus error detected on this device
    fn error_detected(
        _dev: &Device<device::Bound>,
        _error: ChannelState,
        _this: Pin<&Self::Driver>,
    ) -> ErsResult {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// MMIO has been re-enabled, but not DMA
    fn mmio_enabled(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) -> ErsResult {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// PCI slot has been reset
    fn slot_reset(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) -> ErsResult {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// PCI function reset prepare
    fn reset_prepare(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// PCI function reset completed
    fn reset_done(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Device driver may resume normal operations
    fn resume(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Allow device driver to record more details of a correctable error
    fn cor_error_detected(_dev: &Device<device::Bound>, _this: Pin<&Self::Driver>) {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

#[vtable]
impl ErrorHandler for () {
    type Driver = ();
}

/// A vtable for the error handler trait.
pub(super) struct ErrorHandlerVTable<T: ErrorHandler>(PhantomData<T>);

impl<T: ErrorHandler + 'static> ErrorHandlerVTable<T> {
    extern "C" fn error_detected(
        pdev: *mut bindings::pci_dev,
        error: bindings::pci_channel_state_t,
    ) -> bindings::pci_ers_result_t {
        // SAFETY: The PCI bus only ever calls the error_detected callback with a valid pointer
        // to a `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `error_detected()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `error_detected` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        let error = ChannelState::try_from(error).unwrap_or(ChannelState::PermanentFailure);

        T::error_detected(pdev, error, data).into_c()
    }

    extern "C" fn mmio_enabled(pdev: *mut bindings::pci_dev) -> bindings::pci_ers_result_t {
        // SAFETY: The PCI bus only ever calls the mmio_enabled callback with a valid pointer
        // to a `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `mmio_enabled()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `mmio_enabled` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::mmio_enabled(pdev, data).into_c()
    }

    extern "C" fn slot_reset(pdev: *mut bindings::pci_dev) -> bindings::pci_ers_result_t {
        // SAFETY: The PCI bus only ever calls the slot_reset callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `slot_reset()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `slot_reset` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::slot_reset(pdev, data).into_c()
    }

    extern "C" fn reset_prepare(pdev: *mut bindings::pci_dev) {
        // SAFETY: The PCI bus only ever calls the reset_prepare callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `reset_prepare()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `reset_prepare` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::reset_prepare(pdev, data)
    }

    extern "C" fn reset_done(pdev: *mut bindings::pci_dev) {
        // SAFETY: The PCI bus only ever calls the reset_done callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `reset_done()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `reset_done` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::reset_done(pdev, data)
    }

    extern "C" fn resume(pdev: *mut bindings::pci_dev) {
        // SAFETY: The PCI bus only ever calls the resume callback with a valid pointer to a
        // `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `resume()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `resume` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::resume(pdev, data)
    }

    extern "C" fn cor_error_detected(pdev: *mut bindings::pci_dev) {
        // SAFETY: The PCI bus only ever calls the cor_error_detected callback with a valid pointer
        // to a `struct pci_dev`.
        //
        // INVARIANT: `pdev` is valid for the duration of `cor_error_detected()`.
        let pdev = unsafe { &*pdev.cast::<Device<device::CoreInternal>>() };

        // SAFETY: `cor_error_detected` is only ever called after a successful call to
        // `probe_callback`, hence it's guaranteed that `Device::set_drvdata()` has been called
        // and stored a `Pin<KBox<T>>`.
        let data = unsafe { pdev.as_ref().drvdata_borrow::<Pin<KBox<T::Driver>>>() };

        T::cor_error_detected(pdev, data)
    }

    const VTABLE: bindings::pci_error_handlers = bindings::pci_error_handlers {
        error_detected: if T::HAS_ERROR_DETECTED {
            Some(Self::error_detected)
        } else {
            None
        },
        mmio_enabled: if T::HAS_MMIO_ENABLED {
            Some(Self::mmio_enabled)
        } else {
            None
        },
        slot_reset: if T::HAS_SLOT_RESET {
            Some(Self::slot_reset)
        } else {
            None
        },
        reset_prepare: if T::HAS_RESET_PREPARE {
            Some(Self::reset_prepare)
        } else {
            None
        },
        reset_done: if T::HAS_RESET_DONE {
            Some(Self::reset_done)
        } else {
            None
        },
        resume: if T::HAS_RESUME {
            Some(Self::resume)
        } else {
            None
        },
        cor_error_detected: if T::HAS_COR_ERROR_DETECTED {
            Some(Self::cor_error_detected)
        } else {
            None
        },
    };

    pub(super) const fn vtable_ptr() -> *const bindings::pci_error_handlers {
        core::ptr::from_ref(&Self::VTABLE)
    }
}
