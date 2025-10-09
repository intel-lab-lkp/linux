// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the leds driver model.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use core::pin::Pin;

use pin_init::{pin_data, pinned_drop, PinInit};

use crate::{
    alloc::KBox,
    container_of,
    device::{self, property::FwNode},
    error::{code::EINVAL, from_result, to_result, Error, Result},
    prelude::GFP_KERNEL,
    str::CStr,
    try_pin_init,
    types::Opaque,
};

/// The led class device representation.
///
/// This structure represents the Rust abstraction for a C `struct led_classdev`.
#[pin_data(PinnedDrop)]
pub struct Device {
    handler: KBox<dyn HandlerImpl>,
    #[pin]
    classdev: Opaque<bindings::led_classdev>,
}

/// The led init data representation.
///
/// This structure represents the Rust abstraction for a C `struct led_init_data`.
#[derive(Default)]
pub struct InitData<'a> {
    fwnode: Option<&'a FwNode>,
    default_label: Option<&'a CStr>,
    devicename: Option<&'a CStr>,
    devname_mandatory: bool,
}

impl InitData<'static> {
    /// Creates a new [`LedInitData`]
    pub fn new() -> Self {
        Self::default()
    }
}

impl<'a> InitData<'a> {
    /// Sets the firmware node
    pub fn fwnode<'b, 'c>(self, fwnode: &'b FwNode) -> InitData<'c>
    where
        'a: 'c,
        'b: 'c,
    {
        InitData {
            fwnode: Some(fwnode),
            ..self
        }
    }

    /// Sets a default label
    pub fn default_label<'b, 'c>(self, label: &'b CStr) -> InitData<'c>
    where
        'a: 'c,
        'b: 'c,
    {
        InitData {
            default_label: Some(label),
            ..self
        }
    }

    /// Sets the device name
    pub fn devicename<'b, 'c>(self, devicename: &'b CStr) -> InitData<'c>
    where
        'a: 'c,
        'b: 'c,
    {
        InitData {
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
}

/// The led handler trait.
///
/// # Examples
///
///```
/// # use kernel::{c_str, led, alloc::KBox, error::{Result, code::ENOSYS}};
/// # use core::pin::Pin;
///
/// struct MyHandler;
///
///
/// impl led::Handler for MyHandler {
///     const BLOCKING = false;
///     const MAX_BRIGHTNESS = 255;
///
///     fn brightness_set(&self, _brightness: u32) -> Result<()> {
///         // Set the brightness for the led here
///         Ok(())
///     }
/// }
///
/// fn register_my_led() -> Result<Pin<KBox<led::Device>>> {
///     let handler = MyHandler;
///     KBox::pin_init(led::Device::new(
///         None,
///         None,
///         led::InitData::new()
///             .default_label(c_str!("my_led")),
///         handler,
///     ))
/// }
///```
/// Led drivers must implement this trait in order to register and handle a [`Device`].
pub trait Handler {
    /// If set true, [`Handler::brightness_set`] and [`Handler::blink_set`] must not sleep
    /// and perform the operation immediately.
    const BLOCKING: bool;
    /// Set this to true, if [`Handler::blink_set`] is implemented.
    const BLINK: bool = false;
    /// The max brightness level
    const MAX_BRIGHTNESS: u32;

    /// Sets the brightness level
    ///
    /// See also [`Handler::BLOCKING`]
    fn brightness_set(&self, brightness: u32) -> Result<()>;

    /// Activates hardware accelerated blinking.
    ///
    /// delays are in milliseconds. If both are zero, a sensible default should be chosen.
    /// The caller should adjust the timings in that case and if it can't match the values
    /// specified exactly. Setting the brightness to 0 will disable the hardware accelerated
    /// blinking.
    ///
    /// See also [`Handler::BLOCKING`]
    fn blink_set(&self, _delay_on: &mut usize, _delay_off: &mut usize) -> Result<()> {
        Err(EINVAL)
    }
}

trait HandlerImpl {
    fn brightness_set(&self, brightness: u32) -> Result<()>;
    fn blink_set(&self, delay_on: &mut usize, delay_off: &mut usize) -> Result<()>;
}

impl<T: Handler> HandlerImpl for T {
    fn brightness_set(&self, brightness: u32) -> Result<()> {
        T::brightness_set(self, brightness)
    }

    fn blink_set(&self, delay_on: &mut usize, delay_off: &mut usize) -> Result<()> {
        T::blink_set(self, delay_on, delay_off)
    }
}

// SAFETY: A `led::Device` can be unregistered from any thread.
unsafe impl Send for Device {}

// SAFETY: `led::Device` can be shared among threads because all methods of `led::Device`
// are thread safe.
unsafe impl Sync for Device {}

impl Device {
    /// Registers a new led classdev.
    ///
    /// The [`Device`] will be unregistered and drop.
    pub fn new<'a, T: Handler + 'static>(
        parent: Option<&'a device::Device>,
        init_data: InitData<'a>,
        handler: T,
    ) -> impl PinInit<Self, Error> + use<'a, T> {
        try_pin_init!(Self {
            handler <- {
                let handler: KBox<dyn HandlerImpl> = KBox::<T>::new(handler, GFP_KERNEL)?;
                Ok::<_, Error>(handler)
            },
            classdev <- Opaque::try_ffi_init(|ptr: *mut bindings::led_classdev| {
                // SAFETY: `try_ffi_init` guarantees that `ptr` is valid for write.
                unsafe { ptr.write(bindings::led_classdev {
                    max_brightness: T::MAX_BRIGHTNESS,
                    brightness_set: T::BLOCKING.then_some(
                        brightness_set as unsafe extern "C" fn(*mut bindings::led_classdev, u32)
                    ),
                    brightness_set_blocking: (!T::BLOCKING).then_some(
                        brightness_set_blocking
                            as unsafe extern "C" fn(*mut bindings::led_classdev,u32) -> i32
                    ),
                    blink_set: T::BLINK.then_some(
                        blink_set
                            as unsafe extern "C" fn(*mut bindings::led_classdev, *mut usize,
                                                    *mut usize) -> i32
                    ),
                    .. bindings::led_classdev::default()
                }) };

                let mut init_data = bindings::led_init_data {
                    fwnode: init_data.fwnode.map_or(core::ptr::null_mut(), FwNode::as_raw),
                    default_label: init_data.default_label
                                            .map_or(core::ptr::null(), CStr::as_char_ptr),
                    devicename: init_data.devicename.map_or(core::ptr::null(), CStr::as_char_ptr),
                    devname_mandatory: init_data.devname_mandatory,
                };

                let parent = parent
                    .map_or(core::ptr::null_mut(), device::Device::as_raw);

                // SAFETY:
                // - `parent` is guaranteed to be a pointer to a valid `device`
                //    or a null pointer.
                // - `ptr` is guaranteed to be a pointer to an initialized `led_classdev`.
                to_result(unsafe {
                    bindings::led_classdev_register_ext(parent, ptr, &mut init_data)
                })
            }),
        })
    }
}

extern "C" fn brightness_set(led_cdev: *mut bindings::led_classdev, brightness: u32) {
    // SAFETY: `led_cdev` is a valid pointer to a `led_classdev` stored inside a `Device`.
    let classdev = unsafe {
        &*container_of!(
            led_cdev.cast::<Opaque<bindings::led_classdev>>(),
            Device,
            classdev
        )
    };
    let _ = classdev.handler.brightness_set(brightness);
}

extern "C" fn brightness_set_blocking(
    led_cdev: *mut bindings::led_classdev,
    brightness: u32,
) -> i32 {
    // SAFETY: `led_cdev` is a valid pointer to a `led_classdev` stored inside a `Device`.
    let classdev = unsafe {
        &*container_of!(
            led_cdev.cast::<Opaque<bindings::led_classdev>>(),
            Device,
            classdev
        )
    };
    from_result(|| {
        classdev.handler.brightness_set(brightness)?;
        Ok(0)
    })
}

extern "C" fn blink_set(
    led_cdev: *mut bindings::led_classdev,
    delay_on: *mut usize,
    delay_off: *mut usize,
) -> i32 {
    // SAFETY: `led_cdev` is a valid pointer to a `led_classdev` stored inside a `Device`.
    let classdev = unsafe {
        &*container_of!(
            led_cdev.cast::<Opaque<bindings::led_classdev>>(),
            Device,
            classdev
        )
    };
    from_result(|| {
        classdev.handler.blink_set(
            // SAFETY: `delay_on` is guaranteed to be a valid pointer to usize
            unsafe { &mut *delay_on },
            // SAFETY: `delay_on` is guaranteed to be a valid pointer to usize
            unsafe { &mut *delay_off },
        )?;
        Ok(0)
    })
}

#[pinned_drop]
impl PinnedDrop for Device {
    fn drop(self: Pin<&mut Self>) {
        // SAFETY: The existence of `self` guarantees that `self.classdev` has previously been
        // successfully registered with `led_classdev_register_ext`
        unsafe { bindings::led_classdev_unregister(self.classdev.get()) };
    }
}
