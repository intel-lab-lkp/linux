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
    types::{
        ARef,
        Opaque, //
    }, //
};

#[cfg(CONFIG_LEDS_CLASS_MULTICOLOR)]
mod multicolor;
mod normal;

#[cfg(CONFIG_LEDS_CLASS_MULTICOLOR)]
pub use multicolor::{MultiColor, MultiColorDevice, MultiColorSubLed};
pub use normal::{Device, Normal};

/// The led init data representation.
///
/// This structure represents the Rust abstraction for a C `struct led_init_data` with additional
/// fields from `struct led_classdev`.
#[derive(Default)]
pub struct InitData<'a> {
    fwnode: Option<ARef<FwNode>>,
    devicename: Option<&'a CStr>,
    devname_mandatory: bool,
    initial_brightness: u32,
    default_trigger: Option<&'a CStr>,
    color: Color,
}

impl InitData<'static> {
    /// Creates a new [`InitData`].
    pub fn new() -> Self {
        Self::default()
    }
}

impl<'a> InitData<'a> {
    /// Sets the firmware node.
    pub fn fwnode(self, fwnode: Option<ARef<FwNode>>) -> Self {
        Self { fwnode, ..self }
    }

    /// Sets the device name.
    pub fn devicename(self, devicename: &'a CStr) -> Self {
        Self {
            devicename: Some(devicename),
            ..self
        }
    }

    /// Sets if a device name is mandatory.
    pub fn devicename_mandatory(self, mandatory: bool) -> Self {
        Self {
            devname_mandatory: mandatory,
            ..self
        }
    }

    /// Sets the initial brightness value for the led.
    ///
    /// The default brightness is 0.
    /// If [`LedOps::brightness_get`] is implemented, this value will be ignored.
    pub fn initial_brightness(self, brightness: u32) -> Self {
        Self {
            initial_brightness: brightness,
            ..self
        }
    }

    /// Set the default led trigger.
    ///
    /// This value can be overwritten by the "linux,default-trigger" fwnode property.
    pub fn default_trigger(self, trigger: &'a CStr) -> Self {
        Self {
            default_trigger: Some(trigger),
            ..self
        }
    }

    /// Sets the color of the led.
    ///
    /// This value can be overwritten by the "color" fwnode property.
    pub fn color(self, color: Color) -> Self {
        Self { color, ..self }
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
///     type Mode = led::Normal;
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
///     KBox::pin_init(led::Device::new(
///         parent,
///         led::InitData::new(),
///         MyLedOps,
///     ), GFP_KERNEL)
/// }
/// ```
/// Led drivers must implement this trait in order to register and handle a [`Device`].
#[vtable]
pub trait LedOps: Send + 'static + Sized {
    /// The bus device required by the implementation.
    #[allow(private_bounds)]
    type Bus: AsBusDevice<Bound>;

    /// The led mode to use.
    ///
    /// See [`Mode`].
    type Mode: Mode;

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
        classdev: &<Self::Mode as Mode>::Device<Self>,
        brightness: u32,
    ) -> Result<()>;

    /// Gets the current brightness level.
    fn brightness_get(
        &self,
        dev: &Self::Bus,
        classdev: &<Self::Mode as Mode>::Device<Self>,
    ) -> u32 {
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
        classdev: &<Self::Mode as Mode>::Device<Self>,
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
    #[cfg_attr(
        CONFIG_LEDS_CLASS_MULTICOLOR,
        doc = "Use this color for a [`MultiColor`] led."
    )]
    #[cfg_attr(
        not(CONFIG_LEDS_CLASS_MULTICOLOR),
        doc = "Use this color for a `MultiColor` led."
    )]
    /// If the led supports RGB, use [`Color::Rgb`] instead.
    Multi = bindings::LED_COLOR_ID_MULTI,
    #[cfg_attr(
        CONFIG_LEDS_CLASS_MULTICOLOR,
        doc = "Use this color for a [`MultiColor`] led with rgb support."
    )]
    #[cfg_attr(
        not(CONFIG_LEDS_CLASS_MULTICOLOR),
        doc = "Use this color for a `MultiColor` led with rgb support."
    )]
    Rgb = bindings::LED_COLOR_ID_RGB,
    Purple = bindings::LED_COLOR_ID_PURPLE,
    Orange = bindings::LED_COLOR_ID_ORANGE,
    Pink = bindings::LED_COLOR_ID_PINK,
    Cyan = bindings::LED_COLOR_ID_CYAN,
    Lime = bindings::LED_COLOR_ID_LIME,
}

impl TryFrom<u32> for Color {
    type Error = Error;

    fn try_from(value: u32) -> core::result::Result<Self, Self::Error> {
        const _: () = {
            assert!(bindings::LED_COLOR_ID_MAX == 15);
        };
        if value < bindings::LED_COLOR_ID_MAX {
            // SAFETY:
            // - `Color` is represented as `u32`
            // - the const block above guarantees that no additional color has been added
            // - `value` is guaranteed to be in the color id range
            Ok(unsafe { transmute::<u32, Color>(value) })
        } else {
            Err(EINVAL)
        }
    }
}

/// The led mode.
///
/// Each led mode has its own led class device type with different capabilities.
///
#[cfg_attr(
    CONFIG_LEDS_CLASS_MULTICOLOR,
    doc = "See [`Normal`] and [`MultiColor`]."
)]
#[cfg_attr(
    not(CONFIG_LEDS_CLASS_MULTICOLOR),
    doc = "See [`Normal`] and `MultiColor`."
)]
pub trait Mode: private::Sealed {
    /// The class device for the led mode.
    type Device<T: LedOps<Mode = Self>>;
}

mod private {
    pub trait Sealed {}
}
