// SPDX-License-Identifier: GPL-2.0

//! LED subsystem abstractions.
//!
//! C header: [`includes/linux/leds.h`](srctree/include/linux/leds.h)

use core::ffi::c_ulong;
use core::num::NonZeroU8;
use core::ptr;
use core::time::Duration;

use crate::device::Device;
use crate::{error::from_result, init::PinInit, prelude::*, types::Opaque};

/// Color of an LED.
#[derive(Copy, Clone)]
pub enum Color {
    /// White
    White,
    /// Red
    Red,
    /// Green
    Green,
    /// Blue
    Blue,
    /// Amber
    Amber,
    /// Violet
    Violet,
    /// Yellow
    Yellow,
    /// Purple
    Purple,
    /// Orange
    Orange,
    /// Pink
    Pink,
    /// Cyan
    Cyan,
    /// Lime
    Lime,

    /// Infrared
    IR,
    /// Multicolor LEDs
    Multi,
    /// Multicolor LEDs that can do arbitrary color,
    /// so this would include `RGBW` and similar
    RGB,
}

impl Color {
    /// Get String representation of the Color.
    pub fn name_cstr(&self) -> Option<&'static CStr> {
        // SAFETY: C function call, enum is valid argument.
        let name = unsafe { bindings::led_get_color_name(u32::from(self) as u8) };
        if name.is_null() {
            return None;
        }
        // SAFETY: pointer from above, valid for static lifetime.
        Some(unsafe { CStr::from_char_ptr(name) })
    }

    /// Get String representation of the Color.
    #[inline]
    pub fn name(&self) -> Option<&'static str> {
        // SAFETY: name from C name array which is valid UTF-8.
        self.name_cstr()
            .map(|name| unsafe { name.as_str_unchecked() })
    }
}

impl core::fmt::Display for Color {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(self.name().unwrap_or("Invalid color"))
    }
}

impl From<Color> for u32 {
    fn from(color: Color) -> Self {
        match color {
            Color::White => bindings::LED_COLOR_ID_WHITE,
            Color::Red => bindings::LED_COLOR_ID_RED,
            Color::Green => bindings::LED_COLOR_ID_GREEN,
            Color::Blue => bindings::LED_COLOR_ID_BLUE,
            Color::Amber => bindings::LED_COLOR_ID_AMBER,
            Color::Violet => bindings::LED_COLOR_ID_VIOLET,
            Color::Yellow => bindings::LED_COLOR_ID_YELLOW,
            Color::Purple => bindings::LED_COLOR_ID_PURPLE,
            Color::Orange => bindings::LED_COLOR_ID_ORANGE,
            Color::Pink => bindings::LED_COLOR_ID_PINK,
            Color::Cyan => bindings::LED_COLOR_ID_CYAN,
            Color::Lime => bindings::LED_COLOR_ID_LIME,
            Color::IR => bindings::LED_COLOR_ID_IR,
            Color::Multi => bindings::LED_COLOR_ID_MULTI,
            Color::RGB => bindings::LED_COLOR_ID_RGB,
        }
    }
}

impl From<&Color> for u32 {
    fn from(color: &Color) -> Self {
        (*color).into()
    }
}

impl TryFrom<u32> for Color {
    type Error = Error;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        Ok(match value {
            bindings::LED_COLOR_ID_WHITE => Color::White,
            bindings::LED_COLOR_ID_RED => Color::Red,
            bindings::LED_COLOR_ID_GREEN => Color::Green,
            bindings::LED_COLOR_ID_BLUE => Color::Blue,
            bindings::LED_COLOR_ID_AMBER => Color::Amber,
            bindings::LED_COLOR_ID_VIOLET => Color::Violet,
            bindings::LED_COLOR_ID_YELLOW => Color::Yellow,
            bindings::LED_COLOR_ID_PURPLE => Color::Purple,
            bindings::LED_COLOR_ID_ORANGE => Color::Orange,
            bindings::LED_COLOR_ID_PINK => Color::Pink,
            bindings::LED_COLOR_ID_CYAN => Color::Cyan,
            bindings::LED_COLOR_ID_LIME => Color::Lime,
            bindings::LED_COLOR_ID_IR => Color::IR,
            bindings::LED_COLOR_ID_MULTI => Color::Multi,
            bindings::LED_COLOR_ID_RGB => Color::RGB,
            _ => return Err(EINVAL),
        })
    }
}

/// Config for the LED.
///
/// Some fields are optional and only used depending on how the led is registered.
pub struct LedConfig {
    /// Color of the LED.
    pub color: Color,
}

/// A Led backed by a C `struct led_classdev`, additionally offering
/// driver data storage.
///
/// The LED is unregistered once this LED struct is dropped.
// TODO: add devm register variant
#[pin_data(PinnedDrop)]
pub struct Led<T> {
    #[pin]
    led: Opaque<bindings::led_classdev>,
    /// Driver private data
    pub data: T,
}

impl<T> Led<T> {
    /// Return a raw reference to `Self` from a raw `struct led_classdev` C pointer.
    ///
    /// # Safety
    ///
    /// `ptr` must point to a [`bindings::led_classdev`] field in a struct of type `Self`.
    unsafe fn led_container_of(ptr: *mut bindings::led_classdev) -> *mut Self {
        let ptr = ptr.cast::<Opaque<bindings::led_classdev>>();

        // SAFETY: By the safety requirement of this function ptr is embedded in self.
        unsafe { crate::container_of!(ptr, Self, led).cast_mut() }
    }
}

impl<'a, T> Led<T>
where
    T: Operations + 'a,
{
    /// Register a new LED with a predefine name.
    pub fn register_with_name(
        name: &'a CStr,
        device: Option<&'a Device>,
        config: &'a LedConfig,
        data: T,
    ) -> impl PinInit<Self, Error> + 'a {
        try_pin_init!( Self {
            led <- Opaque::try_ffi_init(move |place: *mut bindings::led_classdev| {
            // SAFETY: `place` is a pointer to a live allocation, so erasing is valid.
            unsafe { place.write_bytes(0, 1) };

            // SAFETY: `place` is a pointer to a live allocation of `bindings::led_classdev`.
            unsafe { Self::build_with_name(place, name) };

            // SAFETY: `place` is a pointer to a live allocation of `bindings::led_classdev`.
            unsafe { Self::build_config(place, config) };

            // SAFETY: `place` is a pointer to a live allocation of `bindings::led_classdev`.
            unsafe { Self::build_vtable(place) };

        let dev = device.map(|dev| dev.as_raw()).unwrap_or(ptr::null_mut());
            // SAFETY: `place` is a pointer to a live allocation of `bindings::led_classdev`.
        crate::error::to_result(unsafe {
            bindings::led_classdev_register_ext(dev, place, ptr::null_mut())
        })
            }),
            data: data,
        })
    }

    /// Add nameto the led_classdev.
    ///
    /// # Safety
    ///
    /// `ptr` has to be valid.
    unsafe fn build_with_name(ptr: *mut bindings::led_classdev, name: &'a CStr) {
        // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
        let name_ptr = unsafe { ptr::addr_of_mut!((*ptr).name) };
        // SAFETY: `name_ptr` points to a valid allocation and we have exclusive access.
        unsafe { ptr::write(name_ptr, name.as_char_ptr()) };
    }

    /// Add config to led_classdev.
    ///
    /// # Safety
    ///
    /// `ptr` has to be valid.
    unsafe fn build_config(ptr: *mut bindings::led_classdev, config: &'a LedConfig) {
        // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
        let color_ptr = unsafe { ptr::addr_of_mut!((*ptr).color) };
        // SAFETY: `color_ptr` points to a valid allocation and we have exclusive access.
        unsafe { ptr::write(color_ptr, config.color.into()) };
    }
}

impl<T> Led<T>
where
    T: Operations,
{
    /// Add the Operations vtable to the `led_classdev` struct.
    ///
    /// # Safety
    ///
    /// `ptr` has to be valid.
    unsafe fn build_vtable(ptr: *mut bindings::led_classdev) {
        // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
        let max_brightness = unsafe { ptr::addr_of_mut!((*ptr).max_brightness) };
        // SAFETY: `max_brightness` points to a valid allocation and we have exclusive access.
        unsafe { ptr::write(max_brightness, T::MAX_BRIGHTNESS as _) };

        // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
        let set_fn: *mut Option<_> = unsafe { ptr::addr_of_mut!((*ptr).brightness_set) };
        // SAFETY: `set_fn` points to a valid allocation and we have exclusive access.
        unsafe { ptr::write(set_fn, Some(brightness_set::<T>)) }

        if T::HAS_BRIGHTNESS_GET {
            // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
            let get_fn: *mut Option<_> = unsafe { ptr::addr_of_mut!((*ptr).brightness_get) };
            // SAFETY: `set_fn` points to a valid allocation and we have exclusive access.
            unsafe { ptr::write(get_fn, Some(brightness_get::<T>)) }
        }

        if T::HAS_BLINK_SET {
            // SAFETY: `ptr` is pointing to a live allocation, so the deref is safe.
            let blink_fn: *mut Option<_> = unsafe { ptr::addr_of_mut!((*ptr).blink_set) };
            // SAFETY: `set_fn` points to a valid allocation and we have exclusive access.
            unsafe { ptr::write(blink_fn, Some(blink_set::<T>)) }
        }
    }
}

#[pinned_drop]
impl<T> PinnedDrop for Led<T> {
    fn drop(self: Pin<&mut Self>) {
        // SAEFTY: led is pointing to a live allocation
        unsafe { bindings::led_classdev_unregister(self.led.get()) }
    }
}

// SAFETY: `struct led_classdev` holds a mutex.
unsafe impl<T: Send> Send for Led<T> {}
// SAFETY: `struct led_classdev` holds a mutex.
unsafe impl<T: Sync> Sync for Led<T> {}

/// LED brightness.
#[derive(Debug, Copy, Clone)]
pub enum Brightness {
    /// LED off.
    Off,
    /// Led set to the given value.
    On(NonZeroU8),
}

impl Brightness {
    /// Half LED brightness
    // SAFETY: constant value non zero
    pub const HALF: Self = Self::On(unsafe { NonZeroU8::new_unchecked(127) });
    /// Full LED brightness.
    pub const FULL: Self = Self::On(NonZeroU8::MAX);

    /// Convert C brightness value to Rust brightness enum
    fn from_c_enum(brightness: bindings::led_brightness) -> Self {
        u8::try_from(brightness)
            .map(NonZeroU8::new)
            .map(|brightness| brightness.map(Self::On).unwrap_or(Self::Off))
            .inspect_err(|_| pr_warn!("Brightness out of u8 range\n"))
            .unwrap_or(Self::FULL)
    }

    /// Convert rust brightness enum to C value
    fn as_c_enum(&self) -> bindings::led_brightness {
        u8::from(self) as bindings::led_brightness
    }
}

impl From<&Brightness> for u8 {
    fn from(brightness: &Brightness) -> Self {
        match brightness {
            Brightness::Off => 0,
            Brightness::On(v) => v.get(),
        }
    }
}

/// Led operations vtable.
// TODO: add blocking variant (either via const generic or second trait)
#[macros::vtable]
pub trait Operations: Sized {
    /// The maximum brightnes this led supports.
    const MAX_BRIGHTNESS: u8;

    /// Set LED brightness level.
    /// This functions must not sleep.
    fn brightness_set(this: &mut Led<Self>, brightness: Brightness);

    /// Get the LED brightness level.
    fn brightness_get(_this: &mut Led<Self>) -> Brightness {
        crate::build_error(crate::error::VTABLE_DEFAULT_ERROR)
    }

    /// Activae hardware accelerated blink, delays are in milliseconds
    fn blink_set(
        _this: &mut Led<Self>,
        _delay_on: Duration,
        _delay_off: Duration,
    ) -> Result<(Duration, Duration)> {
        crate::build_error(crate::error::VTABLE_DEFAULT_ERROR)
    }
}

unsafe extern "C" fn brightness_set<T>(
    led_cdev: *mut bindings::led_classdev,
    brightness: bindings::led_brightness,
) where
    T: Operations,
{
    // SAFETY: By the safety requirement of this function `led_cdev` is embedded inside a `Led<T>`
    // struct.
    let led = unsafe { &mut *(Led::<T>::led_container_of(led_cdev.cast())) };
    let brightness = Brightness::from_c_enum(brightness);
    T::brightness_set(led, brightness);
}

unsafe extern "C" fn brightness_get<T>(
    led_cdev: *mut bindings::led_classdev,
) -> bindings::led_brightness
where
    T: Operations,
{
    // SAFETY: By the safety requirement of this function `led_cdev` is embedded inside a `Led<T>`
    // struct.
    let led = unsafe { &mut *(Led::<T>::led_container_of(led_cdev.cast())) };
    T::brightness_get(led).as_c_enum()
}

unsafe extern "C" fn blink_set<T>(
    led_cdev: *mut bindings::led_classdev,
    delay_on: *mut c_ulong,
    delay_off: *mut c_ulong,
) -> i32
where
    T: Operations,
{
    from_result(|| {
        // SAFETY: By the safety requirement of this function `led_cdev` is embedded inside a
        // `Led<T>` struct.
        let led = unsafe { &mut *(Led::<T>::led_container_of(led_cdev.cast())) };

        // SAFETY: By the safety requirement of this function `delay_on` is pointing to a valid
        // `c_uint`
        let on = Duration::from_millis(unsafe { *delay_on } as u64);
        // SAFETY: By the safety requirement of this function `delay_off` is pointing to a valid
        // `c_uint`
        let off = Duration::from_millis(unsafe { *delay_off } as u64);

        let (on, off) = T::blink_set(led, on, off)?;

        // writing back return values
        // SAFETY: By the safety requirement of this function `delay_on` is pointing to a valid
        // `c_uint`
        unsafe { ptr::write(delay_on, on.as_millis() as c_ulong) };
        // SAFETY: By the safety requirement of this function `delay_off` is pointing to a valid
        // `c_uint`
        unsafe { ptr::write(delay_off, off.as_millis() as c_ulong) };

        Ok(0)
    })
}
