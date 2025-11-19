// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the leds driver model.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use core::{
    marker::PhantomData,
    mem::transmute,
    pin::Pin,
    ptr::NonNull, //
};

use pin_init::{
    pin_data,
    pinned_drop,
    PinInit, //
};

use crate::{
    build_error,
    container_of,
    device::{
        self,
        property::FwNode,
        AsBusDevice,
        Bound, //
    },
    devres::Devres,
    error::{
        code::EINVAL,
        from_result,
        to_result,
        Error,
        Result,
        VTABLE_DEFAULT_ERROR, //
    },
    macros::vtable,
    str::CStr,
    try_pin_init,
    types::{
        ARef,
        Opaque, //
    }, //
};

#[cfg(CONFIG_LEDS_CLASS_MULTICOLOR)]
mod multicolor;
mod normal;

#[cfg(CONFIG_LEDS_CLASS_MULTICOLOR)]
pub use multicolor::{MultiColor, MultiColorSubLed};
pub use normal::Normal;

/// The led class device representation.
///
/// This structure represents the Rust abstraction for a led class device.
#[pin_data(PinnedDrop)]
pub struct Device<T: LedOps> {
    ops: T,
    #[pin]
    classdev: Opaque<<T::Mode as private::Mode>::Type>,
}

/// The led init data representation.
///
/// This structure represents the Rust abstraction for a C `struct led_init_data` with additional
/// fields from `struct led_classdev`.
#[derive(Default)]
pub struct InitData<'a> {
    fwnode: Option<ARef<FwNode>>,
    default_label: Option<&'a CStr>,
    devicename: Option<&'a CStr>,
    devname_mandatory: bool,
    initial_brightness: u32,
    default_trigger: Option<&'a CStr>,
    color: Color,
}

impl InitData<'static> {
    /// Creates a new [`InitData`]
    pub fn new() -> Self {
        Self::default()
    }
}

impl<'a> InitData<'a> {
    /// Sets the firmware node
    pub fn fwnode(self, fwnode: Option<ARef<FwNode>>) -> Self {
        Self { fwnode, ..self }
    }

    /// Sets a default label
    pub fn default_label(self, label: &'a CStr) -> Self {
        Self {
            default_label: Some(label),
            ..self
        }
    }

    /// Sets the device name
    pub fn devicename(self, devicename: &'a CStr) -> Self {
        Self {
            devicename: Some(devicename),
            ..self
        }
    }

    /// Sets if a device name is mandatory
    pub fn devicename_mandatory(self, mandatory: bool) -> Self {
        Self {
            devname_mandatory: mandatory,
            ..self
        }
    }

    /// Sets the initial brightness value for the led
    ///
    /// The default brightness is 0.
    /// If [`LedOps::brightness_get`] is implemented, this value will be ignored.
    pub fn initial_brightness(self, brightness: u32) -> Self {
        Self {
            initial_brightness: brightness,
            ..self
        }
    }

    /// Set the default led trigger
    ///
    /// This value can be overwritten by the "linux,default-trigger" fwnode property.
    pub fn default_trigger(self, trigger: &'a CStr) -> Self {
        Self {
            default_trigger: Some(trigger),
            ..self
        }
    }

    /// Sets the color of the led
    ///
    /// This value can be overwritten by the "color" fwnode property.
    pub fn color(self, color: Color) -> Self {
        Self { color, ..self }
    }
}

/// Trait defining the operations for a LED driver.
///
/// # Examples
///
///```
/// # use kernel::{
/// #     c_str, device, devres::Devres,
/// #     error::Result, led,
/// #     macros::vtable, platform, prelude::*,
/// # };
/// # use core::pin::Pin;
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
///         led::InitData::new()
///             .default_label(c_str!("my_led")),
///         MyLedOps,
///     ), GFP_KERNEL)
/// }
///```
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
    /// The max brightness level
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
    fn brightness_get(&self, _dev: &Self::Bus, _classdev: &Device<Self>) -> u32 {
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
        _dev: &Self::Bus,
        _classdev: &Device<Self>,
        _delay_on: &mut usize,
        _delay_off: &mut usize,
    ) -> Result<()> {
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
pub trait Mode: private::Mode {}

impl<T: private::Mode> Mode for T {}

type RegisterFunc<T> =
    unsafe extern "C" fn(*mut bindings::device, *mut T, *mut bindings::led_init_data) -> i32;

type UnregisterFunc<T> = unsafe extern "C" fn(*mut T);

mod private {
    pub trait Mode {
        type Type;
        const REGISTER: super::RegisterFunc<Self::Type>;
        const UNREGISTER: super::UnregisterFunc<Self::Type>;

        /// # Safety
        /// `raw` must be a valid pointer to [`Self::Type`].
        unsafe fn device<'a>(raw: *mut Self::Type) -> &'a crate::device::Device;

        /// # Safety
        /// `led_cdev` must be a valid pointer to `led_classdev` embedded within [`Self::Type`].
        unsafe fn from_classdev(led_cdev: *mut bindings::led_classdev) -> *mut Self::Type;

        /// # Safety
        /// `raw` must be a valid pointer to [`Self::Type`].
        unsafe fn pre_brightness_set(_raw: *mut Self::Type, _brightness: u32) {}

        fn release(_led_cdev: &mut Self::Type) {}
    }
}

// SAFETY: A `led::Device` can be unregistered from any thread.
unsafe impl<T: LedOps + Send> Send for Device<T> {}

// SAFETY: `led::Device` can be shared among threads because all methods of `led::Device`
// are thread safe.
unsafe impl<T: LedOps + Sync> Sync for Device<T> {}

impl<T: LedOps> Device<T> {
    fn __new<'a>(
        parent: &'a T::Bus,
        init_data: InitData<'a>,
        ops: T,
        func: impl FnOnce(bindings::led_classdev) -> Result<<T::Mode as private::Mode>::Type> + 'a,
    ) -> impl PinInit<Devres<Self>, Error> + 'a {
        Devres::new(
            parent.as_ref(),
            try_pin_init!(Self {
                ops,
                classdev <- Opaque::try_ffi_init(|ptr: *mut <T::Mode as private::Mode>::Type| {
                    // SAFETY: `try_ffi_init` guarantees that `ptr` is valid for write.
                    // `T::Mode::Type` (and the embedded led_classdev) gets fully initialized
                    // in-place by `T::Mode::REGISTER` including `mutex` and `list_head`.
                    unsafe {
                        ptr.write((func)(bindings::led_classdev {
                            brightness_set: (!T::BLOCKING)
                                .then_some(Adapter::<T>::brightness_set_callback),
                            brightness_set_blocking: T::BLOCKING
                                .then_some(Adapter::<T>::brightness_set_blocking_callback),
                            brightness_get: T::HAS_BRIGHTNESS_GET
                                .then_some(Adapter::<T>::brightness_get_callback),
                            blink_set: T::HAS_BLINK_SET.then_some(Adapter::<T>::blink_set_callback),
                            max_brightness: T::MAX_BRIGHTNESS,
                            brightness: init_data.initial_brightness,
                            default_trigger: init_data.default_trigger
                                .map_or(core::ptr::null(), CStr::as_char_ptr),
                            color: init_data.color as u32,
                            ..bindings::led_classdev::default()
                        })?)
                    };

                    let mut init_data_raw = bindings::led_init_data {
                        fwnode: init_data.fwnode
                            .as_ref()
                            .map_or(core::ptr::null_mut(), |fwnode| fwnode.as_raw()),
                        default_label: init_data
                            .default_label
                            .map_or(core::ptr::null(), CStr::as_char_ptr),
                        devicename: init_data
                            .devicename
                            .map_or(core::ptr::null(), CStr::as_char_ptr),
                        devname_mandatory: init_data.devname_mandatory,
                    };

                    // SAFETY:
                    // - `parent.as_ref().as_raw()` is guaranteed to be a pointer to a valid
                    //    `device`.
                    // - `ptr` is guaranteed to be a pointer to an initialized `T::Mode::Type`.
                    to_result(unsafe {
                        (<T::Mode as private::Mode>::REGISTER)(
                            parent.as_ref().as_raw(),
                            ptr,
                            &mut init_data_raw,
                        )
                    })?;

                    core::mem::forget(init_data.fwnode); // keep the reference count incremented

                    Ok::<_, Error>(())
                }),
            }),
        )
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    unsafe fn from_raw<'a>(led_cdev: *mut bindings::led_classdev) -> &'a Self {
        // SAFETY: The function's contract guarantees that `led_cdev` points to a `led_classdev`
        // embedded within a `led::Device` and thus is embedded within `T::Mode::Type`.
        let raw = unsafe { <T::Mode as private::Mode>::from_classdev(led_cdev) };

        // SAFETY: The function's contract guarantees that `raw` points to a `led_classdev` field
        // embedded within a valid `led::Device`. `container_of!` can therefore safely calculate
        // the address of the containing struct.
        unsafe { &*container_of!(Opaque::cast_from(raw), Self, classdev) }
    }

    fn parent(&self) -> &device::Device<Bound> {
        // SAFETY: `self.classdev.get()` is guaranteed to be a valid pointer to `T::Mode::Type`.
        let device = unsafe { <T::Mode as private::Mode>::device(self.classdev.get()) };
        // SAFETY: `led::Device::__new` doesn't allow to register a class device without an parent.
        let parent = unsafe { device.parent().unwrap_unchecked() };
        // SAFETY: the existence of `self` guarantees that `parent` is bound to a driver.
        unsafe { parent.as_bound() }
    }
}

struct Adapter<T: LedOps> {
    _p: PhantomData<T>,
}

impl<T: LedOps> Adapter<T> {
    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    /// This function is called on setting the brightness of a led.
    unsafe extern "C" fn brightness_set_callback(
        led_cdev: *mut bindings::led_classdev,
        brightness: u32,
    ) {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
        // `T::Mode::Type` embedded within a `led::Device`.
        let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        // SAFETY: `classdev.classdev.get()` is guaranteed to be a valid pointer to a
        // `T::Mode::Type`.
        unsafe {
            <T::Mode as private::Mode>::pre_brightness_set(classdev.classdev.get(), brightness);
        }

        let _ = classdev.ops.brightness_set(parent, classdev, brightness);
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    /// This function is called on setting the brightness of a led immediately.
    unsafe extern "C" fn brightness_set_blocking_callback(
        led_cdev: *mut bindings::led_classdev,
        brightness: u32,
    ) -> i32 {
        from_result(|| {
            // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
            // `T::Mode::Type` embedded within a `led::Device`.
            let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

            // SAFETY: `classdev.classdev.get()` is guaranteed to be a valid pointer to a
            // `T::Mode::Type`.
            unsafe {
                <T::Mode as private::Mode>::pre_brightness_set(classdev.classdev.get(), brightness);
            }

            classdev.ops.brightness_set(parent, classdev, brightness)?;
            Ok(0)
        })
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    /// This function is called on getting the brightness of a led.
    unsafe extern "C" fn brightness_get_callback(led_cdev: *mut bindings::led_classdev) -> u32 {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
        // `T::Mode::Type` embedded within a `led::Device`.
        let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        classdev.ops.brightness_get(parent, classdev)
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    /// `delay_on` and `delay_off` must be valid pointers to `usize` and have
    /// exclusive access for the period of this function.
    /// This function is called on enabling hardware accelerated blinking.
    unsafe extern "C" fn blink_set_callback(
        led_cdev: *mut bindings::led_classdev,
        delay_on: *mut usize,
        delay_off: *mut usize,
    ) -> i32 {
        from_result(|| {
            // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
            // `T::Mode::Type` embedded within a `led::Device`.
            let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

            classdev.ops.blink_set(
                parent,
                classdev,
                // SAFETY: The function's contract guarantees that `delay_on` points to a `usize`
                // and is exclusive for the period of this function.
                unsafe { &mut *delay_on },
                // SAFETY: The function's contract guarantees that `delay_off` points to a `usize`
                // and is exclusive for the period of this function.
                unsafe { &mut *delay_off },
            )?;
            Ok(0)
        })
    }
}

#[pinned_drop]
impl<T: LedOps> PinnedDrop for Device<T> {
    fn drop(self: Pin<&mut Self>) {
        let raw = self.classdev.get();
        // SAFETY: The existence of `self` guarantees that `self.classdev.get()` is a pointer to a
        // valid `T::Mode::Type`.
        let dev: &device::Device = unsafe { <T::Mode as private::Mode>::device(raw) };

        let _fwnode = dev
            .fwnode()
            // SAFETY: the reference count of `fwnode` has previously been
            // incremented in `led::Device::__new`.
            .map(|fwnode| unsafe { ARef::from_raw(NonNull::from(fwnode)) });

        // SAFETY: The existence of `self` guarantees that `self.classdev` has previously been
        // successfully registered with `T::Mode::REGISTER`.
        unsafe { (<T::Mode as private::Mode>::UNREGISTER)(raw) };

        // SAFETY: The existence of `self` guarantees that `self.classdev.get()` is a pointer to a
        // valid `T::Mode::Type`.
        <T::Mode as private::Mode>::release(unsafe { &mut *raw });
    }
}
