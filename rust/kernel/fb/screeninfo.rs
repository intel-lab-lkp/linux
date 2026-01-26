// SPDX-License-Identifier: GPL-2.0

//! Framebuffer screen information types.
//!
//! This module provides safe wrappers for framebuffer screen information structures and
//! related constants.
//!
//! C header: [`include/linux/fb.h`](srctree/include/linux/fb.h)

use crate::{bindings, ffi, prelude::*};

/// Framebuffer type constants.
pub mod types {
    /// Packed pixels.
    pub const FB_TYPE_PACKED_PIXELS: u32 = crate::bindings::FB_TYPE_PACKED_PIXELS;
}

/// Framebuffer visual constants.
pub mod visual {
    /// True color.
    pub const FB_VISUAL_TRUECOLOR: u32 = crate::bindings::FB_VISUAL_TRUECOLOR;
}

/// Framebuffer acceleration constants.
pub mod accel {
    /// No hardware accelerator.
    pub const FB_ACCEL_NONE: u32 = crate::bindings::FB_ACCEL_NONE;
}

/// Framebuffer activation constants.
pub mod activate {
    /// Set values immediately (or at vblank).
    pub const FB_ACTIVATE_NOW: u32 = crate::bindings::FB_ACTIVATE_NOW;
}

/// Framebuffer video mode constants.
pub mod vmode {
    /// Non-interlaced.
    pub const FB_VMODE_NONINTERLACED: u32 = crate::bindings::FB_VMODE_NONINTERLACED;
}

/// Wrapper for `fb_bitfield`.
///
/// Describes a bitfield within a pixel, typically used for color components.
#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct Bitfield(bindings::fb_bitfield);

impl Bitfield {
    /// Create a new `Bitfield`.
    pub const fn new(offset: u32, length: u32, msb_right: u32) -> Self {
        Self(bindings::fb_bitfield {
            offset,
            length,
            msb_right,
        })
    }

    /// Create a new `Bitfield` from the raw C structure.
    ///
    /// `fb_bitfield` is a POD type, so any bit pattern is valid.
    pub(crate) const fn from_raw(raw: bindings::fb_bitfield) -> Self {
        Self(raw)
    }

    /// Returns the wrapped C structure.
    pub(crate) const fn into_raw(self) -> bindings::fb_bitfield {
        self.0
    }

    /// Bit offset within the pixel.
    pub const fn offset(&self) -> u32 {
        self.0.offset
    }

    /// Bitfield length in bits.
    pub const fn length(&self) -> u32 {
        self.0.length
    }
}

/// Wrapper for `fb_var_screeninfo`.
///
/// Describes variable screen parameters that can be changed by the user or driver
/// (e.g., resolution, color depth).
#[repr(transparent)]
pub struct VarScreenInfo(bindings::fb_var_screeninfo);

impl VarScreenInfo {
    /// Create a zeroed `VarScreenInfo`.
    ///
    /// Most fields will need to be set by the driver.
    ///
    /// `fb_var_screeninfo` is a POD type, so the all-zero bit pattern is valid.
    pub const fn new_zeroed() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }

    /// Create a reference from a raw C structure pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is valid for reading and remains valid for the lifetime
    /// of the returned reference.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::fb_var_screeninfo) -> &'a Self {
        // SAFETY: `VarScreenInfo` is a transparent wrapper around `bindings::fb_var_screeninfo`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the wrapped C structure.
    pub fn into_raw(self) -> bindings::fb_var_screeninfo {
        self.0
    }

    /// Visible resolution (horizontal).
    #[inline]
    pub fn xres(&self) -> u32 {
        self.0.xres
    }

    /// Visible resolution (vertical).
    #[inline]
    pub fn yres(&self) -> u32 {
        self.0.yres
    }

    /// Bits per pixel.
    pub fn bits_per_pixel(&self) -> u32 {
        self.0.bits_per_pixel
    }

    /// Red color bitfield.
    pub fn red(&self) -> Bitfield {
        Bitfield::from_raw(self.0.red)
    }

    /// Green color bitfield.
    pub fn green(&self) -> Bitfield {
        Bitfield::from_raw(self.0.green)
    }

    /// Blue color bitfield.
    pub fn blue(&self) -> Bitfield {
        Bitfield::from_raw(self.0.blue)
    }

    /// Transparency/alpha color bitfield.
    pub fn transp(&self) -> Bitfield {
        Bitfield::from_raw(self.0.transp)
    }

    /// Set the visible resolution (horizontal).
    pub fn set_xres(&mut self, xres: u32) {
        self.0.xres = xres;
    }

    /// Set the visible resolution (vertical).
    pub fn set_yres(&mut self, yres: u32) {
        self.0.yres = yres;
    }

    /// Set the virtual resolution (horizontal).
    pub fn set_xres_virtual(&mut self, xres_virtual: u32) {
        self.0.xres_virtual = xres_virtual;
    }

    /// Set the virtual resolution (vertical).
    pub fn set_yres_virtual(&mut self, yres_virtual: u32) {
        self.0.yres_virtual = yres_virtual;
    }

    /// Set bits per pixel.
    pub fn set_bits_per_pixel(&mut self, bits_per_pixel: u32) {
        self.0.bits_per_pixel = bits_per_pixel;
    }

    /// Set the red color bitfield.
    pub fn set_red(&mut self, red: Bitfield) {
        self.0.red = red.into_raw();
    }

    /// Set the green color bitfield.
    pub fn set_green(&mut self, green: Bitfield) {
        self.0.green = green.into_raw();
    }

    /// Set the blue color bitfield.
    pub fn set_blue(&mut self, blue: Bitfield) {
        self.0.blue = blue.into_raw();
    }

    /// Set the transparency (alpha) color bitfield.
    pub fn set_transp(&mut self, transp: Bitfield) {
        self.0.transp = transp.into_raw();
    }

    /// Set the activation flags.
    pub fn set_activate(&mut self, activate: u32) {
        self.0.activate = activate;
    }

    /// Set the video mode flags.
    pub fn set_vmode(&mut self, vmode: u32) {
        self.0.vmode = vmode;
    }

    /// Set the width (for compatibility, typically same as xres).
    pub fn set_width(&mut self, width: u32) {
        self.0.width = width;
    }

    /// Set the height (for compatibility, typically same as yres).
    pub fn set_height(&mut self, height: u32) {
        self.0.height = height;
    }
}

/// Wrapper for `fb_fix_screeninfo`.
///
/// Describes fixed screen parameters that cannot be changed by the user
/// (e.g., framebuffer memory address, type).
#[repr(transparent)]
pub struct FixScreenInfo(bindings::fb_fix_screeninfo);

impl FixScreenInfo {
    /// Create a zeroed `FixScreenInfo`.
    ///
    /// Most fields will need to be set by the driver.
    ///
    /// `fb_fix_screeninfo` is a POD type, so the all-zero bit pattern is valid.
    pub const fn new_zeroed() -> Self {
        Self(unsafe { core::mem::zeroed() })
    }

    /// Create a reference from a raw C structure pointer.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is valid for reading and remains valid for the lifetime
    /// of the returned reference.
    #[inline]
    pub unsafe fn from_raw<'a>(ptr: *const bindings::fb_fix_screeninfo) -> &'a Self {
        // SAFETY: `FixScreenInfo` is a transparent wrapper around `bindings::fb_fix_screeninfo`.
        unsafe { &*ptr.cast() }
    }

    /// Returns the wrapped C structure.
    pub fn into_raw(self) -> bindings::fb_fix_screeninfo {
        self.0
    }

    /// Framebuffer memory start (physical address).
    #[inline]
    pub fn smem_start(&self) -> usize {
        self.0.smem_start as usize
    }

    /// Length of framebuffer memory in bytes.
    #[inline]
    pub fn smem_len(&self) -> u32 {
        self.0.smem_len
    }

    /// Length of a line in bytes.
    #[inline]
    pub fn line_length(&self) -> u32 {
        self.0.line_length
    }

    /// Set the framebuffer identification string.
    ///
    /// The string (including NUL terminator) is truncated to 16 bytes if it exceeds that length.
    pub fn set_id(&mut self, id: &'static CStr) {
        const FB_ID_LEN: usize = 16;
        let src = id.to_bytes_with_nul();

        // Copy the string into the id array
        let len = core::cmp::min(src.len(), FB_ID_LEN);
        for (i, &byte) in src.iter().take(len).enumerate() {
            self.0.id[i] = byte as ffi::c_char;
        }
        // Zero out the rest of the array if the string is shorter
        for i in len..FB_ID_LEN {
            self.0.id[i] = 0;
        }
    }

    /// Set the framebuffer type.
    pub fn set_type(&mut self, type_: u32) {
        self.0.type_ = type_;
    }

    /// Set the visual type.
    pub fn set_visual(&mut self, visual: u32) {
        self.0.visual = visual;
    }

    /// Set the acceleration type.
    pub fn set_accel(&mut self, accel: u32) {
        self.0.accel = accel;
    }

    /// Set the physical address of framebuffer memory start.
    pub fn set_smem_start(&mut self, start: usize) {
        self.0.smem_start = start;
    }

    /// Set the length of framebuffer memory in bytes.
    pub fn set_smem_len(&mut self, len: u32) {
        self.0.smem_len = len;
    }

    /// Set the length of a line in bytes.
    pub fn set_line_length(&mut self, length: u32) {
        self.0.line_length = length;
    }
}
