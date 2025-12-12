// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! SoC Driver Abstraction
//!
//! C header: [`include/linux/sys_soc.h`](srctree/include/linux/sys_soc.h)

use crate::bindings;
use crate::error;
use crate::prelude::*;
use crate::str::CString;
use core::marker::PhantomPinned;
use core::ptr::addr_of;

/// Attributes for a SoC device
pub struct DeviceAttribute {
    /// Machine
    pub machine: Option<CString>,
    /// Family
    pub family: Option<CString>,
    /// Revision
    pub revision: Option<CString>,
    /// Serial Number
    pub serial_number: Option<CString>,
    /// SoC ID
    pub soc_id: Option<CString>,
}

// SAFETY: We provide no operations through `&BuiltDeviceAttribute`
unsafe impl Sync for BuiltDeviceAttribute {}

// SAFETY: All pointers are normal allocations, not thread-specific
unsafe impl Send for BuiltDeviceAttribute {}

#[pin_data]
struct BuiltDeviceAttribute {
    #[pin]
    backing: DeviceAttribute,
    inner: bindings::soc_device_attribute,
    // Since `inner` has pointers to `backing`, we are !Unpin
    #[pin]
    _pin: PhantomPinned,
}

fn cstring_to_c(mcs: &Option<CString>) -> *const kernel::ffi::c_char {
    mcs.as_ref()
        .map(|cs| cs.as_char_ptr())
        .unwrap_or(core::ptr::null())
}

impl BuiltDeviceAttribute {
    fn as_mut_ptr(&self) -> *mut bindings::soc_device_attribute {
        core::ptr::from_ref(&self.inner).cast_mut()
    }
}

impl DeviceAttribute {
    fn build(self) -> impl PinInit<BuiltDeviceAttribute> {
        pin_init!(BuiltDeviceAttribute {
            inner: bindings::soc_device_attribute {
                machine: cstring_to_c(&self.machine),
                family: cstring_to_c(&self.family),
                revision: cstring_to_c(&self.revision),
                serial_number: cstring_to_c(&self.serial_number),
                soc_id: cstring_to_c(&self.soc_id),
                data: core::ptr::null(),
                custom_attr_group: core::ptr::null(),
            },
            backing: self,
            _pin: PhantomPinned,
        })
    }
}

// SAFETY: We provide no operations through &Device
unsafe impl Sync for Device {}

// SAFETY: Device holds a pointer to a `soc_device`, which may be sent to any thread.
unsafe impl Send for Device {}

/// A registered soc device
#[repr(transparent)]
pub struct Device(*mut bindings::soc_device);

impl Device {
    /// # Safety
    /// * `attr` must be pinned
    /// * `attr` must be valid for reads during the function call
    /// * If a device is returned (e.g. no error), `attr` must remain valid for reads until the
    ///   returned `Device` is dropped.
    unsafe fn register(attr: *const BuiltDeviceAttribute) -> Result<Device> {
        let raw_soc =
            // SAFETY: The struct provided through attr is backed by pinned data next to it, so as
            // long as attr lives, the strings pointed to by the struct will too. By caller
            // invariant, `attr` is pinned, so the pinned data won't move. By caller invariant,
            // `attr` is valid during this call. If it returns a device, and so others may try to
            // read this data, by caller invariant, `attr` won't be released until the device is.
            error::from_err_ptr(unsafe { bindings::soc_device_register((*attr).as_mut_ptr()) })?;
        Ok(Device(raw_soc))
    }
}

#[pin_data(PinnedDrop)]
/// Registration handle for your soc_dev. If you let it go out of scope, your soc_dev will be
/// unregistered.
pub struct DeviceRegistration {
    #[pin]
    attr: BuiltDeviceAttribute,
    soc_dev: Device,
    // Since Device transitively points to the contents of attr, we are !Unpin
    #[pin]
    _pin: PhantomPinned,
}

#[pinned_drop]
impl PinnedDrop for DeviceRegistration {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: Device always contains a live pointer to a soc_device that can be unregistered
        unsafe { bindings::soc_device_unregister(self.soc_dev.0) }
    }
}

impl DeviceRegistration {
    /// Register a new SoC device
    pub fn register(attr: DeviceAttribute) -> impl PinInit<Self, Error> {
        try_pin_init!(&this in Self {
                    attr <- attr.build(),
                    // SAFETY: We have already initialized attr, and we are inside PinInit and Self
                    // is !Unpin, so attr won't be moved and is valid. If it returns success, attr
                    // will not be dropped until after our `PinnedDrop` implementation runs, so the
                    // device will be unregistered first.
                    soc_dev: unsafe { Device::register(addr_of!((*this.as_ptr()).attr))? },
                    _pin: PhantomPinned,
        }? Error)
    }
}
