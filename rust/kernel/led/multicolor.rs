// SPDX-License-Identifier: GPL-2.0

//! Led mode for the `struct led_classdev_mc`.
//!
//! C header: [`include/linux/led-class-multicolor.h`](srctree/include/linux/led-class-multicolor.h)

use crate::{
    alloc::KVec,
    error::code::EINVAL,
    prelude::*, //
};

use super::*;

/// The led mode for the `struct led_classdev_mc`. Leds with this mode can have multiple colors.
pub enum MultiColor {}

/// The multicolor sub led info representation.
///
/// This structure represents the Rust abstraction for a C `struct mc_subled`.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct MultiColorSubLed {
    /// the color of the sub led
    pub color: Color,
    /// the brightness of the sub led.
    ///
    /// The value will be automatically calculated.
    /// See `MultiColor::pre_brightness_set`.
    pub brightness: u32,
    /// the intensity of the sub led.
    pub intensity: u32,
    /// arbitrary data for the driver to store.
    pub channel: u32,
    _p: PhantomData<()>, // Only allow creation with `MultiColorSubLed::new`.
}

// We directly pass a reference to the `subled_info` field in `led_classdev_mc` to the driver via
// `Device::subleds()`.
// We need safeguards to ensure `MultiColorSubLed` and `mc_subled` stay identical.
const _: () = {
    use core::mem::offset_of;

    const fn assert_same_type<T>(_: &T, _: &T) {}

    let rust_zeroed = MultiColorSubLed {
        color: Color::White,
        brightness: 0,
        intensity: 0,
        channel: 0,
        _p: PhantomData,
    };
    let c_zeroed = bindings::mc_subled {
        color_index: 0,
        brightness: 0,
        intensity: 0,
        channel: 0,
    };

    assert!(offset_of!(MultiColorSubLed, color) == offset_of!(bindings::mc_subled, color_index));
    assert_same_type(&0u32, &c_zeroed.color_index);

    assert!(
        offset_of!(MultiColorSubLed, brightness) == offset_of!(bindings::mc_subled, brightness)
    );
    assert_same_type(&rust_zeroed.brightness, &c_zeroed.brightness);

    assert!(offset_of!(MultiColorSubLed, intensity) == offset_of!(bindings::mc_subled, intensity));
    assert_same_type(&rust_zeroed.intensity, &c_zeroed.intensity);

    assert!(offset_of!(MultiColorSubLed, channel) == offset_of!(bindings::mc_subled, channel));
    assert_same_type(&rust_zeroed.channel, &c_zeroed.channel);

    assert!(size_of::<MultiColorSubLed>() == size_of::<bindings::mc_subled>());
};

impl MultiColorSubLed {
    /// Create a new multicolor sub led info.
    pub const fn new(color: Color) -> Self {
        Self {
            color,
            brightness: 0,
            intensity: 0,
            channel: 0,
            _p: PhantomData,
        }
    }

    /// Set arbitrary data for the driver.
    pub const fn channel(mut self, channel: u32) -> Self {
        self.channel = channel;
        self
    }

    /// Set the initial intensity of the subled.
    pub const fn initial_intensity(mut self, intensity: u32) -> Self {
        self.intensity = intensity;
        self
    }
}

impl private::Mode for MultiColor {
    type Type = bindings::led_classdev_mc;
    const REGISTER: RegisterFunc<Self::Type> = bindings::led_classdev_multicolor_register_ext;
    const UNREGISTER: UnregisterFunc<Self::Type> = bindings::led_classdev_multicolor_unregister;

    unsafe fn device<'a>(raw: *mut Self::Type) -> &'a device::Device {
        // SAFETY:
        // - The function's contract guarantees that `raw` is a valid pointer to `led_classdev`.
        unsafe { device::Device::from_raw((*raw).led_cdev.dev) }
    }

    unsafe fn from_classdev(led_cdev: *mut bindings::led_classdev) -> *mut Self::Type {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to
        // `led_classdev` embedded within a `Self::Type`.
        unsafe { container_of!(led_cdev, bindings::led_classdev_mc, led_cdev) }
    }

    unsafe fn pre_brightness_set(raw: *mut Self::Type, brightness: u32) {
        // SAFETY: The function's contract guarantees that `raw` is a valid pointer to
        // `led_classdev_mc`.
        unsafe { bindings::led_mc_calc_color_components(raw, brightness) };
    }

    fn release(led_cdev: &mut Self::Type) {
        // SAFETY: `subled_info` is guaranteed to be a valid array pointer to `mc_subled` with the
        // length and capacity of `led_cdev.num_colors`. See `led::Device::new_multicolor`.
        let _subleds_vec = unsafe {
            KVec::from_raw_parts(
                led_cdev.subled_info,
                led_cdev.num_colors as usize,
                led_cdev.num_colors as usize,
            )
        };
    }
}

impl<T: LedOps<Mode = MultiColor>> Device<T> {
    /// Registers a new multicolor led classdev.
    ///
    /// The [`Device`] will be unregistered on drop.
    pub fn new_multicolor<'a>(
        parent: &'a T::Bus,
        init_data: InitData<'a>,
        ops: T,
        subleds: &'a [MultiColorSubLed],
    ) -> impl PinInit<Devres<Self>, Error> + 'a {
        assert!(subleds.len() <= u32::MAX as usize);
        Self::__new(parent, init_data, ops, |led_cdev| {
            let mut subleds_vec = KVec::new();
            subleds_vec.extend_from_slice(subleds, GFP_KERNEL)?;
            let (subled_info, num_colors, capacity) = subleds_vec.into_raw_parts();
            debug_assert_eq!(num_colors, capacity);

            let mut used = 0;
            if subleds.iter().any(|subled| {
                let bit = 1 << (subled.color as u32);
                if (used & bit) != 0 {
                    true
                } else {
                    used |= bit;
                    false
                }
            }) {
                dev_err!(parent.as_ref(), "duplicate color in multicolor led\n");
                return Err(EINVAL);
            }

            Ok(bindings::led_classdev_mc {
                led_cdev,
                // CAST: We checked above that the length of subleds fits into a u32.
                num_colors: num_colors as u32,
                // CAST: The safeguards in the const block ensure that `MultiColorSubLed` has an
                // identical layout to `mc_subled`.
                subled_info: subled_info.cast::<bindings::mc_subled>(),
            })
        })
    }

    /// Returns the subleds passed to [`Device::new_multicolor`].
    pub fn subleds(&self) -> &[MultiColorSubLed] {
        // SAFETY: The existence of `self` guarantees that `self.classdev.get()` is a pointer to a
        // valid `led_classdev_mc`.
        let raw = unsafe { &*self.classdev.get() };
        // SAFETY: `raw.subled_info` is a valid pointer to `mc_subled[num_colors]`.
        // CAST: The safeguards in the const block ensure that `MultiColorSubLed` has an identical
        // layout to `mc_subled`.
        unsafe {
            core::slice::from_raw_parts(
                raw.subled_info.cast::<MultiColorSubLed>(),
                raw.num_colors as usize,
            )
        }
    }
}
