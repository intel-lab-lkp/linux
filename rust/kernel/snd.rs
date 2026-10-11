// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (C) 2026 Tristan de Cacqueray

//! Abstractions for the snd subsystem.
//!
//! This module provides bindings for working with ALSA in kernel modules.
//!
//! C header: [`include/sound/core.h`](srctree/include/sound/core.h)

use core::marker::PhantomData;
use kernel::{
    bindings,
    device::Device,
    error::{to_result, Result},
    prelude::*,
    sync::Arc,
    types::ForeignOwnable,
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

/// A rawmidi substream instance. Must not be shared outside of the RawMidiInput/Output operations.
pub struct RawMidiSubstream {
    ptr: *mut bindings::snd_rawmidi_substream,
}

impl RawMidiSubstream {
    /// # Safety
    ///
    /// The caller must ensure that `ptr` is a valid pointer provided by the ALSA core.
    unsafe fn from_raw(ptr: *mut bindings::snd_rawmidi_substream) -> Self {
        Self { ptr }
    }

    /// Copies data from hardware to the ALSA core input ring buffer.
    ///
    /// Returns the number of bytes successfully copied
    pub fn receive(&self, buffer: &[u8]) -> Result<usize> {
        let count: c_int = buffer.len().try_into().map_err(|_| EINVAL)?;
        // SAFETY: `self.ptr` is valid and the buffer length matched the buffer ptr.
        let ret = unsafe { bindings::snd_rawmidi_receive(self.ptr, buffer.as_ptr(), count) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as usize)
        }
    }

    /// Retrieves data from the ALSA core output ring buffer to be transmitted to hardware.
    ///
    /// Returns the number of bytes successfully copied into `buffer`.
    pub fn transmit(&self, buffer: &mut [u8]) -> Result<usize> {
        let count: c_int = buffer.len().try_into().map_err(|_| EINVAL)?;
        // SAFETY: `self.ptr` is valid between `open` and `close`.
        // C writes at most `count` bytes into the `buffer`.
        let ret = unsafe { bindings::snd_rawmidi_transmit(self.ptr, buffer.as_mut_ptr(), count) };
        if ret < 0 {
            Err(Error::from_errno(ret))
        } else {
            Ok(ret as usize)
        }
    }
}

/// Operations for a MIDI input stream.
pub trait RawMidiInput: Send + Sync {
    /// Call when the input substream is opened.
    fn open(&self, _substream: &RawMidiSubstream) -> Result<()> {
        Ok(())
    }
    /// Called when the input substream is closed.
    fn close(&self, _substream: &RawMidiSubstream) -> Result<()> {
        Ok(())
    }
    /// Called to start or stop reading input data.
    fn trigger(&self, _substream: &RawMidiSubstream, _up: bool);
}

/// Operations for a MIDI output stream.
pub trait RawMidiOutput: Send + Sync {
    /// Called when the output substream is opened.
    fn open(&self, _substream: &RawMidiSubstream) -> Result<()> {
        Ok(())
    }
    /// Called when the output substream is closed.
    fn close(&self, _substream: &RawMidiSubstream) -> Result<()> {
        Ok(())
    }
    /// Called to start or stop transmitting output data.
    fn trigger(&self, _substream: &RawMidiSubstream, _up: bool);
}

struct RawMidiOps<T>(PhantomData<T>);
impl<T: 'static> RawMidiOps<T> {
    /// # Safety
    ///
    /// rawmidi must contain Arc<T> in private_data.
    unsafe extern "C" fn private_free_cb(rmidi: *mut bindings::snd_rawmidi) {
        // SAFETY: paired with `into_foreign` in `new`.
        drop::<Arc<T>>(unsafe { Arc::from_foreign((*rmidi).private_data) });
    }
}

impl<T: RawMidiInput + 'static> RawMidiOps<T> {
    const INPUT: bindings::snd_rawmidi_ops = bindings::snd_rawmidi_ops {
        open: Some(Self::open_input),
        close: Some(Self::close_input),
        trigger: Some(Self::trigger_input),
        drain: None,
    };

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn open_input(s: *mut bindings::snd_rawmidi_substream) -> c_int {
        with_ctx_err::<T>(s, |dev, sub| T::open(dev, sub))
    }

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn close_input(s: *mut bindings::snd_rawmidi_substream) -> c_int {
        with_ctx_err::<T>(s, |dev, sub| T::close(dev, sub))
    }

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn trigger_input(s: *mut bindings::snd_rawmidi_substream, up: c_int) {
        with_ctx::<T, _>(s, |dev, sub| T::trigger(dev, sub, up != 0))
    }
}

impl<T: RawMidiOutput + 'static> RawMidiOps<T> {
    const OUTPUT: bindings::snd_rawmidi_ops = bindings::snd_rawmidi_ops {
        open: Some(Self::open_output),
        close: Some(Self::close_output),
        trigger: Some(Self::trigger_output),
        drain: None,
    };

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn open_output(s: *mut bindings::snd_rawmidi_substream) -> c_int {
        with_ctx_err::<T>(s, |dev, sub| T::open(dev, sub))
    }

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn close_output(s: *mut bindings::snd_rawmidi_substream) -> c_int {
        with_ctx_err::<T>(s, |dev, sub| T::close(dev, sub))
    }

    /// # SAFETY
    ///
    /// substream must contain Arc<T> in private_data.
    unsafe extern "C" fn trigger_output(s: *mut bindings::snd_rawmidi_substream, up: c_int) {
        with_ctx::<T, _>(s, |dev, sub| T::trigger(dev, sub, up != 0))
    }
}

// Helper for C FFI
fn with_ctx_err<T: 'static>(
    s: *mut bindings::snd_rawmidi_substream,
    f: impl FnOnce(&T, &RawMidiSubstream) -> Result,
) -> c_int {
    with_ctx::<T, _>(s, |dev, sub| match f(dev, sub) {
        Ok(()) => 0,
        Err(e) => e.to_errno(),
    })
}

// Helper for C FFI
fn with_ctx<T: 'static, R>(
    s: *mut bindings::snd_rawmidi_substream,
    f: impl FnOnce(&T, &RawMidiSubstream) -> R,
) -> R {
    // SAFETY: caller contract (see callbacks): `s` and `rmidi` are valid,
    // `private_data` is a live foreign-owned `Arc<T>`.
    let data = unsafe { <Arc<T> as ForeignOwnable>::borrow((*(*s).rmidi).private_data) };
    // SAFETY: substream is valid.
    let substream = unsafe { RawMidiSubstream::from_raw(s) };
    f(&data, &substream)
}

/// A rawmidi instance.
///
/// TODO: track lifetime with the parent sndcard?
pub struct RawMidi {
    ptr: *mut bindings::snd_rawmidi,
}

impl RawMidi {
    /// Creates a new rawmidi instance bound to a parent soundcard.
    ///
    /// Note: this presently requires both a Input and Output implementation
    /// even if the input_count or output_count is 0.
    /// TODO: enforce correctness in the API.
    pub fn new<T: RawMidiInput + RawMidiOutput + 'static>(
        card: &SoundCard,
        id: &CStr,
        device_idx: u16,
        output_count: u16,
        input_count: u16,
        data: Arc<T>,
    ) -> Result<Self> {
        let mut rmidi_ptr: *mut bindings::snd_rawmidi = core::ptr::null_mut();

        // SAFETY: `card.ptr` is a valid soundcard allocation.
        // id is converted to match the C API.
        // The bindings expect a mut but snd_rawmidi_init just uses strcpy.
        to_result(unsafe {
            bindings::snd_rawmidi_new(
                card.ptr,
                id.as_char_ptr().cast_mut(),
                device_idx.into(),
                output_count.into(),
                input_count.into(),
                &mut rmidi_ptr,
            )
        })?;

        // SAFETY: `rmidi_ptr` is validated by the return check above.
        let rmidi = unsafe { &mut *rmidi_ptr };

        rmidi.private_data = Arc::into_foreign(data).cast();
        // Note: private_free will drop the private_data from the foreign ptr.
        // need to make sure it is freed in the event the cb is not called!
        rmidi.private_free = Some(RawMidiOps::<T>::private_free_cb);

        if input_count > 0 {
            rmidi.info_flags |= bindings::SNDRV_RAWMIDI_INFO_INPUT;
            // SAFETY: rmidi_ptr is validated above.
            unsafe {
                bindings::snd_rawmidi_set_ops(
                    rmidi_ptr,
                    bindings::SNDRV_RAWMIDI_STREAM_INPUT as i32,
                    &RawMidiOps::<T>::INPUT,
                )
            };
        }
        if output_count > 0 {
            rmidi.info_flags |= bindings::SNDRV_RAWMIDI_INFO_OUTPUT;
            // SAFETY: rmidi_ptr is validated above.
            unsafe {
                bindings::snd_rawmidi_set_ops(
                    rmidi_ptr,
                    bindings::SNDRV_RAWMIDI_STREAM_OUTPUT as i32,
                    &RawMidiOps::<T>::OUTPUT,
                );
            }
        }
        if input_count > 0 && output_count > 0 {
            rmidi.info_flags |= bindings::SNDRV_RAWMIDI_INFO_DUPLEX;
        }

        Ok(Self { ptr: rmidi_ptr })
    }

    /// Set the rawmidi device name.
    pub fn set_name(&mut self, name: &CStr) -> Result<()> {
        // SAFETY: `self.ptr` is valid between `new` and card free.
        let mut rmidi = unsafe { *self.ptr };
        copy_to_c_buf(&mut rmidi.name, name)
    }
}
