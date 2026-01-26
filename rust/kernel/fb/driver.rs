// SPDX-License-Identifier: GPL-2.0

//! Framebuffer driver core.
//!
//! This module provides the core abstractions for implementing framebuffer drivers.
//!
//! C header: [`include/linux/fb.h`]

use crate::{
    bindings, device, devres, error::to_result, fb, fs::file, mm, prelude::*, sync::aref::ARef,
};
use macros::vtable;

/// Framebuffer driver information.
pub struct DriverInfo {
    /// Driver name.
    pub name: &'static CStr,
    /// Driver description.
    pub desc: &'static CStr,
}

/// Framebuffer operations trait.
///
/// This trait defines the operations that a framebuffer driver must or can implement.
/// It corresponds to `struct fb_ops` in C.
///
/// All methods receive a `device` parameter that provides access to both driver-specific data
/// (via `device.data()` or direct dereference) and generic framebuffer info (via `device.var()`,
/// `device.fix()`, etc.).
#[vtable]
pub trait Operations {
    /// Driver-specific data type for operations context.
    type Data: Sync + Send;

    /// Read from framebuffer device.
    ///
    /// For framebuffers with strange non-linear layouts or that do not work with normal memory
    /// mapped access.
    fn read(
        _device: &fb::Device<impl Driver<Data = Self::Data>>,
        _buf: &mut [u8],
        _ppos: &mut file::Offset,
    ) -> Result<usize> {
        Err(EINVAL)
    }

    /// Write to framebuffer device.
    ///
    /// For framebuffers with strange non-linear layouts or that do not work with normal memory
    /// mapped access.
    fn write(
        _device: &fb::Device<impl Driver<Data = Self::Data>>,
        _buf: &[u8],
        _ppos: &mut file::Offset,
    ) -> Result<usize> {
        Err(EINVAL)
    }

    /// Set a color register.
    fn setcolreg(
        _device: &fb::Device<impl Driver<Data = Self::Data>>,
        _regno: u32,
        _red: u32,
        _green: u32,
        _blue: u32,
        _transp: u32,
    ) -> Result {
        Ok(())
    }

    /// Draws a rectangle.
    fn fillrect(_device: &fb::Device<impl Driver<Data = Self::Data>>, _rect: &fb::FillRect) {
        // Default: no-op (driver may rely on software fallback or macro)
    }

    /// Copy data from area to another.
    fn copyarea(_device: &fb::Device<impl Driver<Data = Self::Data>>, _area: &fb::CopyArea) {
        // Default: no-op (driver may rely on software fallback or macro)
    }

    /// Draws an image to the display.
    fn imageblit(_device: &fb::Device<impl Driver<Data = Self::Data>>, _image: &fb::Image) {
        // Default: no-op (driver may rely on software fallback or macro)
    }

    /// Perform framebuffer-specific mmap.
    fn mmap(
        _device: &fb::Device<impl Driver<Data = Self::Data>>,
        _vma: &mm::virt::VmaNew,
    ) -> Result {
        Err(EINVAL)
    }

    /// Teardown any resources to do with this framebuffer.
    ///
    /// Called when the last reference to the framebuffer is dropped. Use this to clean up
    /// driver-specific resources.
    ///
    /// Note: The framework automatically calls `framebuffer_release()` after this method
    /// returns, so drivers should *not* call `framebuffer_release()` themselves. This follows
    /// RAII principles: since `Device::new()` calls `framebuffer_alloc()`, the framework is
    /// responsible for calling `framebuffer_release()`.
    fn destroy(_device: &fb::Device<impl Driver<Data = Self::Data>>) {}
}

/// Trait for framebuffer drivers.
#[vtable]
pub trait Driver {
    /// Driver-specific context data.
    type Data: Sync + Send;

    /// Operations implementation for this driver.
    type Ops: Operations<Data = Self::Data>;

    /// Driver metadata.
    const INFO: DriverInfo;
}

/// Registration for a framebuffer device.
///
/// The device is unregistered when this structure is dropped.
pub struct Registration<T: Driver>(ARef<fb::Device<T>>);

impl<T: Driver> Registration<T> {
    /// Creates a new [`Registration`] and registers the framebuffer device.
    fn new(fb: &fb::Device<T>) -> Result<Self> {
        // SAFETY: `fb.as_raw()` is valid by the invariants of `fb::Device`.
        to_result(unsafe { bindings::register_framebuffer(fb::Device::as_raw(fb)) })?;

        Ok(Self(ARef::from(fb)))
    }

    /// Creates a new [`Registration`] and transfers ownership to devres.
    pub fn new_foreign_owned(fb: &fb::Device<T>, dev: &device::Device<device::Bound>) -> Result
    where
        T: 'static,
    {
        // Verify that the device in fb_info matches the provided device
        let fb_device = <fb::Device<T> as AsRef<device::Device>>::as_ref(fb);
        if fb_device.as_raw() != dev.as_raw() {
            return Err(EINVAL);
        }

        let reg = Registration::<T>::new(fb)?;

        devres::register(dev, reg, GFP_KERNEL)
    }

    /// Returns a reference to the registered framebuffer device.
    pub fn device(&self) -> &fb::Device<T> {
        &self.0
    }
}

// SAFETY: All `&self` methods on this type are thread-safe. `ARef<fb::Device<T>>` and
// `fb::Device<T>` are `Sync`, so it is safe to share `Registration` between threads.
unsafe impl<T: Driver> Sync for Registration<T> {}

// SAFETY: Registration and unregistration from the framebuffer subsystem can happen from any
// thread.
unsafe impl<T: Driver> Send for Registration<T> {}

impl<T: Driver> Drop for Registration<T> {
    fn drop(&mut self) {
        // SAFETY: `self.0` is guaranteed to be valid for the lifetime of `Registration`. The
        // existence of this `Registration` guarantees that the device is registered.
        unsafe { bindings::unregister_framebuffer(fb::Device::as_raw(&self.0)) };
    }
}
