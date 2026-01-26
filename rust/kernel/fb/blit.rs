// SPDX-License-Identifier: GPL-2.0

//! Framebuffer blit operations.
//!
//! This module provides safe wrappers for framebuffer blit operation structures and functions.
//!
//! C header: [`include/linux/fb.h`](srctree/include/linux/fb.h)

use crate::{bindings, fb};

/// Wrapper for `fb_fillrect` with safe accessors.
///
/// Describes a filled rectangle operation for framebuffer devices.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct FillRect(bindings::fb_fillrect);

impl FillRect {
    /// Create a new `FillRect` from the raw C structure.
    ///
    /// `fb_fillrect` is a POD type, so any bit pattern is valid.
    pub const fn from_raw(raw: bindings::fb_fillrect) -> Self {
        Self(raw)
    }

    /// Returns a reference to the underlying C `fb_fillrect` structure.
    #[inline]
    fn as_raw(&self) -> &bindings::fb_fillrect {
        &self.0
    }
}

/// Wrapper for `fb_copyarea` with safe accessors.
///
/// Describes a copy area operation for framebuffer devices.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct CopyArea(bindings::fb_copyarea);

impl CopyArea {
    /// Create a new `CopyArea` from the raw C structure.
    ///
    /// `fb_copyarea` is a POD type, so any bit pattern is valid.
    pub const fn from_raw(raw: bindings::fb_copyarea) -> Self {
        Self(raw)
    }

    /// Returns a reference to the underlying C `fb_copyarea` structure.
    #[inline]
    fn as_raw(&self) -> &bindings::fb_copyarea {
        &self.0
    }
}

/// Wrapper for `fb_image` with safe accessors.
///
/// Describes an image blit operation for framebuffer devices.
#[repr(transparent)]
pub struct Image(bindings::fb_image);

impl Image {
    /// Create a new `Image` from the raw C structure.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `raw` is properly initialized.
    pub const unsafe fn from_raw(raw: bindings::fb_image) -> Self {
        Self(raw)
    }

    /// Returns a reference to the underlying C `fb_image` structure.
    #[inline]
    fn as_raw(&self) -> &bindings::fb_image {
        &self.0
    }
}

/// Software rectangle fill operation.
///
/// Invokes the generic `cfb_fillrect` helper.
pub fn cfb_fillrect<T: fb::Driver>(device: &fb::Device<T>, rect: &FillRect) {
    // SAFETY: Both `device.as_raw()` and `rect.as_raw()` return valid pointers by type invariants.
    unsafe {
        bindings::cfb_fillrect(device.as_raw(), rect.as_raw());
    }
}

/// Software area copy operation.
///
/// Invokes the generic `cfb_copyarea` helper.
pub fn cfb_copyarea<T: fb::Driver>(device: &fb::Device<T>, area: &CopyArea) {
    // SAFETY: Both `device.as_raw()` and `area.as_raw()` return valid pointers by type invariants.
    unsafe {
        bindings::cfb_copyarea(device.as_raw(), area.as_raw());
    }
}

/// Software image blit operation.
///
/// Invokes the generic `cfb_imageblit` helper.
pub fn cfb_imageblit<T: fb::Driver>(device: &fb::Device<T>, image: &Image) {
    // SAFETY: Both `device.as_raw()` and `image.as_raw()` return valid pointers by type invariants.
    unsafe {
        bindings::cfb_imageblit(device.as_raw(), image.as_raw());
    }
}
