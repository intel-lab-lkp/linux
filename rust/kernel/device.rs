// SPDX-License-Identifier: GPL-2.0

//! Generic devices that are part of the kernel's driver model.
//!
//! C header: [`include/linux/device.h`](../../../../include/linux/device.h)

use crate::{
    bindings,
    types::{ARef, Opaque},
};
use core::ptr;

/// A ref-counted device.
///
/// # Invariants
///
/// The pointer stored in `Self` is non-null and valid for the lifetime of the ARef instance. In
/// particular, the ARef instance owns an increment on underlying object’s reference count.
#[repr(transparent)]
pub struct Device(Opaque<bindings::device>);

impl Device {
    /// Creates a new ref-counted instance of an existing device pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count.
    pub unsafe fn from_raw(ptr: *mut bindings::device) -> ARef<Self> {
        // SAFETY: By the safety requirements, ptr is valid.
        // Initially increase the reference count by one to compensate for the final decrement once
        // this newly created `ARef<Device>` instance is dropped.
        unsafe { bindings::get_device(ptr) };

        // CAST: `Self` is a `repr(transparent)` wrapper around `bindings::device`.
        let ptr = ptr.cast::<Self>();

        // SAFETY: By the safety requirements, ptr is valid.
        unsafe { ARef::from_raw(ptr::NonNull::new_unchecked(ptr)) }
    }

    /// Obtain the raw `struct device *`.
    pub(crate) fn as_raw(&self) -> *mut bindings::device {
        self.0.get()
    }

    /// Convert a raw `struct device` pointer to a `&Device`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count for
    /// the entire duration when the returned reference exists.
    pub unsafe fn as_ref<'a>(ptr: *mut bindings::device) -> &'a Self {
        // SAFETY: Guaranteed by the safety requirements of the function.
        unsafe { &*ptr.cast() }
    }
}

// SAFETY: Instances of `Device` are always ref-counted.
unsafe impl crate::types::AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is nonzero.
        unsafe { bindings::get_device(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is nonzero.
        unsafe { bindings::put_device(obj.cast().as_ptr()) }
    }
}

// SAFETY: `Device` only holds a pointer to a C device, which is safe to be used from any thread.
unsafe impl Send for Device {}

// SAFETY: `Device` only holds a pointer to a C device, references to which are safe to be used
// from any thread.
unsafe impl Sync for Device {}
