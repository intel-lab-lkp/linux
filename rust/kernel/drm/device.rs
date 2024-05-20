// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM device.
//!
//! C header: [`include/linux/drm/drm_device.h`](../../../../include/linux/drm/drm_device.h)

use crate::{
    bindings, device, drm,
    error::code::*,
    error::from_err_ptr,
    error::Result,
    types::{ARef, AlwaysRefCounted, ForeignOwnable, Opaque},
};
use alloc::boxed::Box;
use core::{ffi::c_void, marker::PhantomData, pin::Pin, ptr::NonNull};

/// A typed DRM device with a specific driver. The device is always reference-counted.
#[repr(transparent)]
pub struct Device<T: drm::drv::Driver>(Opaque<bindings::drm_device>, PhantomData<T>);

impl<T: drm::drv::Driver> Device<T> {
    pub(crate) fn new(
        dev: &device::Device,
        vtable: &Pin<Box<bindings::drm_driver>>,
    ) -> Result<ARef<Self>> {
        let raw_drm = unsafe { bindings::drm_dev_alloc(&**vtable, dev.as_raw()) };
        let raw_drm = NonNull::new(from_err_ptr(raw_drm)? as *mut _).ok_or(ENOMEM)?;

        // SAFETY: The reference count is one, and now we take ownership of that reference as a
        // drm::device::Device.
        Ok(unsafe { ARef::from_raw(raw_drm) })
    }

    pub(crate) fn as_raw(&self) -> *mut bindings::drm_device {
        self.0.get()
    }

    // Not intended to be called externally, except via declare_drm_ioctls!()
    #[doc(hidden)]
    pub unsafe fn borrow<'a>(raw: *const bindings::drm_device) -> &'a Self {
        unsafe { &*(raw as *const Self) }
    }

    pub(crate) fn raw_data(&self) -> *const c_void {
        // SAFETY: `self` is guaranteed to hold a valid `bindings::drm_device` pointer.
        unsafe { *self.as_raw() }.dev_private
    }

    // SAFETY: Must be called only once after device creation.
    pub(crate) unsafe fn set_raw_data(&self, ptr: *const c_void) {
        // SAFETY: Safe as by the safety precondition.
        unsafe { &mut *self.as_raw() }.dev_private = ptr as _;
    }

    /// Returns a borrowed reference to the user data associated with this Device.
    pub fn data(&self) -> Option<<T::Data as ForeignOwnable>::Borrowed<'_>> {
        let dev_private = self.raw_data();

        if dev_private.is_null() {
            None
        } else {
            // SAFETY: `dev_private` is NULL before the DRM device is registered; after the DRM
            // device has been registered dev_private is guaranteed to be valid.
            Some(unsafe { T::Data::borrow(dev_private) })
        }
    }
}

// SAFETY: DRM device objects are always reference counted and the get/put functions
// satisfy the requirements.
unsafe impl<T: drm::drv::Driver> AlwaysRefCounted for Device<T> {
    fn inc_ref(&self) {
        unsafe { bindings::drm_dev_get(&self.as_raw() as *const _ as *mut _) };
    }

    unsafe fn dec_ref(obj: NonNull<Self>) {
        // SAFETY: The Device<T> type has the same layout as drm_device, so we can just cast.
        unsafe { bindings::drm_dev_put(obj.as_ptr() as *mut _) };
    }
}

// SAFETY: `Device` only holds a pointer to a C device, which is safe to be used from any thread.
unsafe impl<T: drm::drv::Driver> Send for Device<T> {}

// SAFETY: `Device` only holds a pointer to a C device, references to which are safe to be used
// from any thread.
unsafe impl<T: drm::drv::Driver> Sync for Device<T> {}
