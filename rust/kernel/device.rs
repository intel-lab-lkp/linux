// SPDX-License-Identifier: GPL-2.0

//! Generic devices that are part of the kernel's driver model.
//!
//! C header: [`include/linux/device.h`](../../../../include/linux/device.h)

use macros::pin_data;

use crate::{
    alloc::flags::*,
    bindings,
    error::Result,
    init::InPlaceInit,
    init::PinInit,
    pin_init,
    str::CStr,
    sync::{LockClassKey, RevocableMutex, RevocableMutexGuard, UniqueArc},
    types::{ARef, Opaque},
};
use core::{
    ops::{Deref, DerefMut},
    pin::Pin,
    ptr,
};

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

/// Device data.
///
/// When a device is unbound (for whatever reason, for example, because the device was unplugged or
/// because the user decided to unbind the driver), the driver is given a chance to clean up its
/// state.
///
/// The device data is reference-counted because other subsystems may hold pointers to it; some
/// device state must be freed and not used anymore, while others must remain accessible.
///
/// This struct separates the device data into two categories:
///   1. Registrations: are destroyed when the device is removed.
///   2. General data: remain available as long as the reference count is nonzero.
///
/// This struct implements the `DeviceRemoval` trait such that `registrations` can be revoked when
/// the device is unbound.
#[pin_data]
pub struct Data<T, U> {
    #[pin]
    registrations: RevocableMutex<T>,
    #[pin]
    general: U,
}

/// Safely creates an new reference-counted instance of [`Data`].
#[doc(hidden)]
#[macro_export]
macro_rules! new_device_data {
    ($reg:expr, $gen:expr, $name:literal) => {{
        static CLASS1: $crate::sync::LockClassKey = $crate::sync::LockClassKey::new();
        let regs = $reg;
        let gen = $gen;
        let name = $crate::c_str!($name);
        $crate::device::Data::try_new(regs, gen, name, &CLASS1)
    }};
}

impl<T, U> Data<T, U> {
    /// Creates a new instance of `Data`.
    ///
    /// It is recommended that the [`new_device_data`] macro be used as it automatically creates
    /// the lock classes.
    pub fn try_new(
        registrations: T,
        general: impl PinInit<U>,
        name: &'static CStr,
        key1: &'static LockClassKey,
    ) -> Result<Pin<UniqueArc<Self>>> {
        let ret = UniqueArc::pin_init(
            pin_init!(Self {
                registrations <- RevocableMutex::new(
                    registrations,
                    name,
                    key1,
                ),
                general <- general,
            }),
            GFP_KERNEL,
        )?;

        Ok(ret)
    }

    /// Returns the locked registrations if they're still available.
    pub fn registrations(&self) -> Option<RevocableMutexGuard<'_, T>> {
        self.registrations.try_write()
    }
}

impl<T, U> crate::driver::DeviceRemoval for Data<T, U> {
    fn device_remove(&self) {
        self.registrations.revoke();
    }
}

impl<T, U> Deref for Data<T, U> {
    type Target = U;

    fn deref(&self) -> &U {
        &self.general
    }
}

impl<T, U> DerefMut for Data<T, U> {
    fn deref_mut(&mut self) -> &mut U {
        &mut self.general
    }
}
