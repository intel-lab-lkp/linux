// SPDX-License-Identifier: GPL-2.0

//! Abstractions for the leds driver model.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use core::{marker::PhantomData, pin::Pin, ptr::NonNull};

use pin_init::{pin_data, pinned_drop, PinInit};

use crate::{
    build_error, container_of,
    device::{self, property::FwNode, Bound, AsBusDevice},
    devres::Devres,
    error::{from_result, to_result, Error, Result, VTABLE_DEFAULT_ERROR},
    macros::vtable,
    str::CStr,
    try_pin_init,
    types::{ARef, Opaque},
};

/// The led class device representation.
///
/// This structure represents the Rust abstraction for a C `struct led_classdev`.
#[pin_data(PinnedDrop)]
pub struct Device<T: LedOps> {
    ops: T,
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
    /// Creates a new [`InitData`]
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
///     const BLOCKING: bool = false;
///     const MAX_BRIGHTNESS: u32 = 255;
///
///     fn brightness_set(
///         &self,
///         _dev: &platform::Device<device::Bound>,
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
    /// If set true, [`LedOps::brightness_set`] and [`LedOps::blink_set`] must perform the
    /// operation immediately. If set false, they must not sleep.
    const BLOCKING: bool;
    /// The max brightness level
    const MAX_BRIGHTNESS: u32;

    /// Sets the brightness level.
    ///
    /// See also [`LedOps::BLOCKING`]
    fn brightness_set(&self, dev: &Self::Bus, brightness: u32) -> Result<()>;

    /// Gets the current brightness level.
    fn brightness_get(&self, _dev: &Self::Bus) -> u32 {
        build_error!(VTABLE_DEFAULT_ERROR)
    }

    /// Activates hardware accelerated blinking.
    ///
    /// delays are in milliseconds. If both are zero, a sensible default should be chosen.
    /// The caller should adjust the timings in that case and if it can't match the values
    /// specified exactly. Setting the brightness to 0 will disable the hardware accelerated
    /// blinking.
    ///
    /// See also [`LedOps::BLOCKING`]
    fn blink_set(
        &self,
        _dev: &Self::Bus,
        _delay_on: &mut usize,
        _delay_off: &mut usize,
    ) -> Result<()> {
        build_error!(VTABLE_DEFAULT_ERROR)
    }
}

// SAFETY: A `led::Device` can be unregistered from any thread.
unsafe impl<T: LedOps + Send> Send for Device<T> {}

// SAFETY: `led::Device` can be shared among threads because all methods of `led::Device`
// are thread safe.
unsafe impl<T: LedOps + Sync> Sync for Device<T> {}

impl<T: LedOps> Device<T> {
    /// Registers a new led classdev.
    ///
    /// The [`Device`] will be unregistered on drop.
    pub fn new<'a>(
        parent: &'a T::Bus,
        init_data: InitData<'a>,
        ops: T,
    ) -> impl PinInit<Devres<Self>, Error> + 'a {
        Devres::new(
            parent.as_ref(),
            try_pin_init!(Self {
                ops,
                classdev <- Opaque::try_ffi_init(|ptr: *mut bindings::led_classdev| {
                    // SAFETY: `try_ffi_init` guarantees that `ptr` is valid for write.
                    // `led_classdev` gets fully initialized in-place by
                    // `led_classdev_register_ext` including `mutex` and `list_head`.
                    unsafe {
                        ptr.write(bindings::led_classdev {
                            max_brightness: T::MAX_BRIGHTNESS,
                            brightness_set: (!T::BLOCKING)
                                .then_some(Adapter::<T>::brightness_set_callback),
                            brightness_set_blocking: T::BLOCKING
                                .then_some(Adapter::<T>::brightness_set_blocking_callback),
                            brightness_get: T::HAS_BRIGHTNESS_GET
                                .then_some(Adapter::<T>::brightness_get_callback),
                            blink_set: T::HAS_BLINK_SET.then_some(Adapter::<T>::blink_set_callback),
                            ..bindings::led_classdev::default()
                        })
                    };

                    let fwnode = init_data.fwnode.map(ARef::from);

                    let mut init_data = bindings::led_init_data {
                        fwnode: fwnode
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
                    // - `parent.as_raw()` is guaranteed to be a pointer to a valid `device`
                    //    or a null pointer.
                    // - `ptr` is guaranteed to be a pointer to an initialized `led_classdev`.
                    to_result(unsafe {
                        bindings::led_classdev_register_ext(
                            parent.as_ref().as_raw(),
                            ptr,
                            &mut init_data,
                        )
                    })?;

                    core::mem::forget(fwnode); // keep the reference count incremented

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
        // field embedded within a valid `led::Device`. `container_of!` can therefore
        // safely calculate the address of the containing struct.
        unsafe { &*container_of!(Opaque::cast_from(led_cdev), Self, classdev) }
    }

    fn parent(&self) -> &device::Device<Bound> {
        // SAFETY:
        // - `self.classdev.get()` is guaranteed to be a valid pointer to `led_classdev`.
        unsafe { device::Device::from_raw((*(*self.classdev.get()).dev).parent) }
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
        // `led_classdev` embedded within a `led::Device`.
        let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        let _ = classdev.ops.brightness_set(parent, brightness);
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
            // `led_classdev` embedded within a `led::Device`.
            let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

            classdev.ops.brightness_set(parent, brightness)?;
            Ok(0)
        })
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    /// This function is called on getting the brightness of a led.
    unsafe extern "C" fn brightness_get_callback(led_cdev: *mut bindings::led_classdev) -> u32 {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
        // `led_classdev` embedded within a `led::Device`.
        let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        classdev.ops.brightness_get(parent)
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
            // `led_classdev` embedded within a `led::Device`.
            let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

            classdev.ops.blink_set(
                parent,
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
        // valid `struct led_classdev`.
        let dev: &device::Device = unsafe { device::Device::from_raw((*raw).dev) };

        let _fwnode = dev
            .fwnode()
            // SAFETY: the reference count of `fwnode` has previously been
            // incremented in `led::Device::new`.
            .map(|fwnode| unsafe { ARef::from_raw(NonNull::from(fwnode)) });

        // SAFETY: The existence of `self` guarantees that `self.classdev` has previously been
        // successfully registered with `led_classdev_register_ext`.
        unsafe { bindings::led_classdev_unregister(self.classdev.get()) };
    }
}
