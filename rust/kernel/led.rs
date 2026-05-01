// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the leds driver model.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use core::{
    marker::PhantomData,
    mem::transmute,
    ptr::NonNull, //
};

use crate::{
    container_of,
    device::{
        self,
        property::FwNode,
        AsBusDevice,
        Bound, //
    },
    devres::Devres,
    error::{
        from_result,
        to_result,
        VTABLE_DEFAULT_ERROR, //
    },
    macros::vtable,
    prelude::*,
    str::CStrExt,
    sync::aref::ARef,
    types::Opaque, //
};

mod normal;

pub use normal::Device;

/// The name of the led is determined by the driver.
pub enum Named {}
/// The name of the led is determined by its fwnode.
pub enum Unnamed {}

/// How the name of the led should be determined.
pub trait DeviceBuilderState: private::Sealed {}

impl DeviceBuilderState for Named {}
impl private::Sealed for Named {}
impl DeviceBuilderState for Unnamed {}
impl private::Sealed for Unnamed {}

/// The builder to register a led class device.
///
/// See [`LedOps`].
pub struct DeviceBuilder<'a, S> {
    fwnode: Option<ARef<FwNode>>,
    name: Option<&'a CStr>,
    devicename: Option<&'a CStr>,
    devname_mandatory: bool,
    initial_brightness: u32,
    color: Color,
    _p: PhantomData<S>,
}

impl<S: DeviceBuilderState> DeviceBuilder<'static, S> {
    /// Creates a new [`DeviceBuilder`].
    #[inline]
    #[expect(
        clippy::new_without_default,
        reason = "no need and derive is prevented by S"
    )]
    pub fn new() -> Self {
        Self {
            fwnode: None,
            name: None,
            devicename: None,
            devname_mandatory: false,
            initial_brightness: 0,
            color: Color::default(),
            _p: PhantomData,
        }
    }
}

impl<'a> DeviceBuilder<'a, Unnamed> {
    /// Sets the firmware node.
    #[inline]
    pub fn fwnode(self, fwnode: Option<ARef<FwNode>>) -> Self {
        Self { fwnode, ..self }
    }

    /// Sets the device name.
    #[inline]
    pub fn devicename(self, devicename: &'a CStr) -> Self {
        Self {
            devicename: Some(devicename),
            ..self
        }
    }

    /// Sets if a device name is mandatory.
    #[inline]
    pub fn devicename_mandatory(self, mandatory: bool) -> Self {
        Self {
            devname_mandatory: mandatory,
            ..self
        }
    }
}

impl<'a, S: DeviceBuilderState> DeviceBuilder<'a, S> {
    /// Sets the initial brightness value for the led.
    ///
    /// The default brightness is 0.
    /// If [`LedOps::brightness_get`] is implemented, this value will be ignored.
    #[inline]
    pub fn initial_brightness(self, brightness: u32) -> Self {
        Self {
            initial_brightness: brightness,
            ..self
        }
    }

    /// Sets the color of the led.
    ///
    /// This value can be overwritten by the "color" fwnode property.
    #[inline]
    pub fn color(self, color: Color) -> Self {
        Self { color, ..self }
    }
}

impl<'a> DeviceBuilder<'a, Named> {
    /// Sets the name of the led.
    ///
    /// Setting this will prevent the fwnode from being used and prevents automatic name
    /// composition.
    #[inline]
    pub fn name(self, name: &'a CStr) -> Self {
        Self {
            name: Some(name),
            ..self
        }
    }
}

/// Trait defining the operations for a LED driver.
///
/// # Examples
/// ```
/// use kernel::{
///      device,
///      devres::Devres,
///      led,
///      macros::vtable,
///      platform,
///      prelude::*, //
///  };
///
/// struct MyLedOps;
///
///
/// #[vtable]
/// impl led::LedOps for MyLedOps {
///     type Bus = platform::Device<device::Bound>;
///     const BLOCKING: bool = false;
///     const MAX_BRIGHTNESS: u32 = 255;
///
///     fn brightness_set(
///         &self,
///         _dev: &platform::Device<device::Bound>,
///         _classdev: &led::Device<Self>,
///         _brightness: u32
///     ) -> Result<()> {
///         // Set the brightness for the led here
///         Ok(())
///     }
/// }
///
/// fn register_my_led(
///     parent: &platform::Device<device::Bound>,
/// ) -> Result<Pin<KBox<Devres<led::Device<MyLedOps>>>>> {
///     KBox::pin_init(led::DeviceBuilder::new()
///         .name(c"white:test")
///         .build(
///             parent,
///             Ok(MyLedOps),
///         ), GFP_KERNEL)
/// }
/// ```
/// Led drivers must implement this trait in order to register and handle a [`Device`].
#[vtable]
pub trait LedOps: Send + Sync + 'static + Sized {
    /// The bus device required by the implementation.
    #[allow(private_bounds)]
    type Bus: AsBusDevice<Bound>;

    /// If set true, [`LedOps::brightness_set`] and [`LedOps::blink_set`] must perform the
    /// operation immediately. If set false, they must not sleep.
    const BLOCKING: bool;
    /// The max brightness level.
    const MAX_BRIGHTNESS: u32;

    /// Sets the brightness level.
    ///
    /// See also [`LedOps::BLOCKING`].
    fn brightness_set(
        &self,
        dev: &Self::Bus,
        classdev: &Device<Self>,
        brightness: u32,
    ) -> Result<()>;

    /// Gets the current brightness level.
    fn brightness_get(&self, dev: &Self::Bus, classdev: &Device<Self>) -> Result<u32> {
        let _ = (dev, classdev);
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Activates hardware accelerated blinking.
    ///
    /// delays are in milliseconds. If both are zero, a sensible default should be chosen.
    /// The caller should adjust the timings in that case and if it can't match the values
    /// specified exactly. Setting the brightness to 0 will disable the hardware accelerated
    /// blinking.
    ///
    /// See also [`LedOps::BLOCKING`].
    fn blink_set(
        &self,
        dev: &Self::Bus,
        classdev: &Device<Self>,
        delay_on: &mut usize,
        delay_off: &mut usize,
    ) -> Result<()> {
        let _ = (dev, classdev, delay_on, delay_off);
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

/// Led colors.
#[derive(Copy, Clone, Debug, Default)]
#[repr(u32)]
#[non_exhaustive]
#[expect(
    missing_docs,
    reason = "it shouldn't be necessary to document each color"
)]
pub enum Color {
    #[default]
    White = bindings::LED_COLOR_ID_WHITE,
    Red = bindings::LED_COLOR_ID_RED,
    Green = bindings::LED_COLOR_ID_GREEN,
    Blue = bindings::LED_COLOR_ID_BLUE,
    Amber = bindings::LED_COLOR_ID_AMBER,
    Violet = bindings::LED_COLOR_ID_VIOLET,
    Yellow = bindings::LED_COLOR_ID_YELLOW,
    Ir = bindings::LED_COLOR_ID_IR,
    Multi = bindings::LED_COLOR_ID_MULTI,
    Rgb = bindings::LED_COLOR_ID_RGB,
    Purple = bindings::LED_COLOR_ID_PURPLE,
    Orange = bindings::LED_COLOR_ID_ORANGE,
    Pink = bindings::LED_COLOR_ID_PINK,
    Cyan = bindings::LED_COLOR_ID_CYAN,
    Lime = bindings::LED_COLOR_ID_LIME,
}
static_assert!(bindings::LED_COLOR_ID_MAX == 15);

impl Color {
    /// Name of the color
    #[inline]
    pub fn as_c_str(self) -> &'static CStr {
        // SAFETY:
        // - `self as u8` is a valid led color id.
        // - `led_get_color_name` always returns a valid C string pointer.
        unsafe { CStr::from_char_ptr(bindings::led_get_color_name(self as u8)) }
    }
}

impl TryFrom<u32> for Color {
    type Error = Error;

    fn try_from(value: u32) -> core::result::Result<Self, Self::Error> {
        if value < bindings::LED_COLOR_ID_MAX {
            // SAFETY:
            // - `Color` is represented as `u32`
            // - the static_assert above guarantees that no additional color has been added
            // - `value` is guaranteed to be in the color id range
            Ok(unsafe { transmute::<u32, Color>(value) })
        } else {
            Err(EINVAL)
        }
    }
}

mod private {
    pub trait Sealed {}
}
