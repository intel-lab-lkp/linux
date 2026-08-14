// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM File objects.
//!
//! C header: [`include/drm/drm_file.h`](srctree/include/drm/drm_file.h)

use crate::{
    bindings,
    drm,
    prelude::*,
    sync::atomic::{
        Relaxed,
        Release, //
    },
    types::{
        CovariantForLt,
        ForLt,
        Opaque, //
    }, //
};
use core::marker::PhantomData;

/// Trait that must be implemented by DRM drivers to represent a DRM File (a client instance).
///
/// The lifetime `'a` allows the file data to borrow from
/// [`RegistrationData`](drm::Driver::RegistrationData).
pub trait DriverFile<'a>: Sized {
    /// The parent `Driver` implementation for this `DriverFile`.
    type Driver: drm::Driver;

    /// Open a new DRM file, creating the per-file driver data.
    ///
    /// Called when a client opens the DRM device. The returned file data may borrow from
    /// `reg_data` with lifetime `'a`.
    fn open(
        device: &drm::Device<Self::Driver, drm::Registered>,
        reg_data: &'a <Self::Driver as drm::Driver>::RegistrationData<'a>,
    ) -> Result<Pin<KBox<Self>>>;
}

/// An open DRM File.
///
/// # Invariants
///
/// `self.0` is a valid instance of a `struct drm_file`.
#[repr(transparent)]
pub struct File<D: drm::Driver>(Opaque<bindings::drm_file>, PhantomData<D>);

impl<D: drm::Driver> File<D> {
    #[doc(hidden)]
    /// Not intended to be called externally, except via declare_drm_ioctls!()
    ///
    /// # Safety
    ///
    /// `ptr` must be a valid pointer to an open `struct drm_file`.
    pub unsafe fn from_raw<'a>(ptr: *mut bindings::drm_file) -> &'a File<D> {
        // SAFETY: `ptr` is valid by the safety requirements of this function.
        unsafe { &*ptr.cast() }
    }

    pub(super) fn as_raw(&self) -> *mut bindings::drm_file {
        self.0.get()
    }

    /// Return a pinned reference to the driver file data.
    ///
    /// Only available when `D::File` implements [`trait@CovariantForLt`]. For invariant types, use
    /// [`inner_with()`](Self::inner_with).
    pub fn inner(&self) -> Pin<&<D::File as ForLt>::Of<'_>>
    where
        D::File: CovariantForLt,
    {
        // SAFETY: `driver_priv` was initialized by `open_callback()`. `CovariantForLt` guarantees
        // the lifetime shortening from `'static` to `'_` is sound.
        unsafe { Pin::new_unchecked(&*(*self.as_raw()).driver_priv.cast_const().cast()) }
    }

    /// Access the driver file data through a closure.
    ///
    /// This works for all file data types, including invariant ones. For covariant types,
    /// [`inner()`](Self::inner) provides direct access without a closure.
    pub fn inner_with<R, F>(&self, f: F) -> R
    where
        F: for<'a> FnOnce(Pin<&'a <D::File as ForLt>::Of<'a>>) -> R,
    {
        // SAFETY: `driver_priv` was initialized by `open_callback()`. The HRTB `for<'a>` prevents
        // the caller from choosing a concrete lifetime, making the lifetime shortening sound
        // regardless of variance.
        f(unsafe { Pin::new_unchecked(&*(*self.as_raw()).driver_priv.cast_const().cast()) })
    }

    /// The open callback of a `struct drm_file`.
    ///
    /// Called from `drm_open()`, which is itself called from `fops_open()`. The latter holds a
    /// `RegistrationGuard`, so the device is guaranteed to be registered for the duration of this
    /// callback.
    pub(crate) extern "C" fn open_callback(
        raw_dev: *mut bindings::drm_device,
        raw_file: *mut bindings::drm_file,
    ) -> core::ffi::c_int
    where
        for<'a> <D::File as ForLt>::Of<'a>: DriverFile<'a, Driver = D>,
    {
        // SAFETY: The DRM core guarantees that `raw_dev` is valid. `fops_open()` holds a
        // `RegistrationGuard`, so the device is registered and the `Registered` context holds.
        let dev: &drm::device::Device<D, drm::Registered> =
            unsafe { drm::device::Device::from_raw(raw_dev) };

        dev.registration_data_with(|reg_data| {
            let inner = match <<D::File as ForLt>::Of<'_> as DriverFile<'_>>::open(dev, reg_data) {
                Err(e) => return e.to_errno(),
                Ok(i) => i,
            };

            // SAFETY: This pointer is treated as pinned, and the Drop guarantee is upheld in
            // `postclose_callback()` or the filelist iteration in `Registration::drop()`.
            let driver_priv = KBox::into_raw(unsafe { Pin::into_inner_unchecked(inner) });

            dev.open_count.fetch_add(1, Relaxed);

            // SAFETY: `raw_file` is a valid pointer to a `struct drm_file`.
            unsafe { (*raw_file).driver_priv = driver_priv.cast() };

            0
        })
    }

    /// The postclose callback of a `struct drm_file`.
    pub(crate) extern "C" fn postclose_callback(
        raw_dev: *mut bindings::drm_device,
        raw_file: *mut bindings::drm_file,
    ) where
        for<'a> <D::File as ForLt>::Of<'a>: DriverFile<'a, Driver = D>,
    {
        // SAFETY: `raw_file` is a valid pointer to a `struct drm_file`.
        let driver_priv = unsafe { (*raw_file).driver_priv };

        if driver_priv.is_null() {
            return;
        }

        // SAFETY: `driver_priv` was created in `open_callback()` through `KBox::into_raw` and has
        // not been dropped yet (the NULL check above guards against double-free from the filelist
        // iteration in `Registration::drop()`).
        let _ = unsafe { KBox::from_raw(driver_priv.cast::<<D::File as ForLt>::Of<'static>>()) };

        // SAFETY: `raw_dev` is valid for the lifetime of the `struct drm_file`.
        let dev: &drm::device::Device<D> = unsafe { drm::device::Device::from_raw(raw_dev) };
        if dev.open_count.fetch_sub(1, Release) == 1 {
            dev.open_count_wq.wake_up();
        }
    }
}

impl<D: drm::Driver> super::private::Sealed for File<D> {}
