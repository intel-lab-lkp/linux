// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Tristan de Cacqueray

//! Abstractions for the snd subsystem.
//!
//! This module provides bindings for working with ALSA in kernel modules.
//!
//! C header: [`include/sound/core.h`](srctree/include/sound/core.h)

use kernel::{
    bindings,
    device::Device,
    error::{Result},
    prelude::*,
};

/// A sound card instance.
pub struct SoundCard {
    ptr: *mut bindings::snd_card,
}

impl SoundCard {
    /// Creates a new sound card bound to a device lifecycle
    pub fn new(dev: &Device, id: &CStr, module: &'static kernel::ThisModule) -> Result<Self> {
        let mut card_ptr: *mut bindings::snd_card = core::ptr::null_mut();

        // SAFETY: C helper copies the arguments.
        let ret = unsafe {
            bindings::snd_devm_card_new(
                dev.as_raw(),
                -1,
                id.as_char_ptr(),
                module.as_ptr(),
                0,
                &mut card_ptr,
            )
        };

        if ret < 0 {
            return Err(Error::from_errno(ret));
        }

        Ok(Self { ptr: card_ptr })
    }

    /// Sets the driver field of the sound card.
    pub fn set_driver(&mut self, name: &CStr) -> Result<()> {
        // SAFETY: `self.ptr` is valid and points to an initialized `snd_card`.
        let card = unsafe { &mut *self.ptr };
        copy_to_c_buf(&mut card.driver, name)
    }

    /// Sets the shortname field of the sound card.
    pub fn set_shortname(&mut self, name: &CStr) -> Result<()> {
        // SAFETY: `self.ptr` is valid and points to an initialized `snd_card`.
        let card = unsafe { &mut *self.ptr };
        copy_to_c_buf(&mut card.shortname, name)
    }

    /// Triggers registration of the card components to the user-space ALSA layers
    pub fn register(self) -> Result<RegisteredSoundCard> {
        // SAFETY: `self.ptr` is valid and points to an initialized `snd_card`.
        let ret = unsafe { bindings::snd_card_register(self.ptr) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(RegisteredSoundCard { _inner: self })
    }
}

/// Helper to copy `src` (including its NUL) into the fixed-size C buffer `dest`.
/// Returns `EINVAL` if `src` does not fit.
fn copy_to_c_buf(dest: &mut [ffi::c_char], src: &CStr) -> Result<()> {
    let bytes = src.to_bytes_with_nul();
    if bytes.len() > dest.len() {
        return Err(EINVAL);
    }
    let (used, rest) = dest.split_at_mut(bytes.len());
    for (d, &b) in used.iter_mut().zip(bytes) {
        *d = b as ffi::c_char;
    }
    rest.fill(0);
    Ok(())
}

/// A sound card that is already registered.
pub struct RegisteredSoundCard {
    _inner: SoundCard,
}
