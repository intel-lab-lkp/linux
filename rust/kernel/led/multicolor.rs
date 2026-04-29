// SPDX-License-Identifier: GPL-2.0

//! Led mode for the `struct led_classdev_mc`.
//!
//! C header: [`include/linux/led-class-multicolor.h`](srctree/include/linux/led-class-multicolor.h)

use crate::{
    alloc::KVec,
    types::ScopeGuard, //
};

use super::*;

/// The led mode for the `struct led_classdev_mc`. Leds with this mode can have multiple colors.
pub enum MultiColor {}
impl Mode for MultiColor {
    type Device<T: LedOps<Mode = Self>> = MultiColorDevice<T>;
}
impl private::Sealed for MultiColor {}

/// The multicolor sub led info representation.
///
/// This structure represents the Rust abstraction for a C `struct mc_subled`.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
#[non_exhaustive]
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
    #[inline]
    pub const fn new(color: Color) -> Self {
        Self {
            color,
            brightness: 0,
            intensity: 0,
            channel: 0,
        }
    }

    /// Set arbitrary data for the driver.
    #[inline]
    pub const fn channel(mut self, channel: u32) -> Self {
        self.channel = channel;
        self
    }

    /// Set the initial intensity of the subled.
    #[inline]
    pub const fn initial_intensity(mut self, intensity: u32) -> Self {
        self.intensity = intensity;
        self
    }
}

/// The multicolor led class device representation.
///
/// This structure represents the Rust abstraction for a multicolor led class device.
#[pin_data(PinnedDrop)]
pub struct MultiColorDevice<T: LedOps<Mode = MultiColor>> {
    #[pin]
    ops: T,
    #[pin]
    classdev: Opaque<bindings::led_classdev_mc>,
}

impl<'a, S: DeviceBuilderState> DeviceBuilder<'a, S> {
    /// Registers a new [`MulticolorDevice`].
    pub fn build_multicolor<T: LedOps<Mode = MultiColor>>(
        self,
        parent: &'a T::Bus,
        ops: impl PinInit<T, Error> + 'a,
        subleds: &'a [MultiColorSubLed],
    ) -> impl PinInit<Devres<MultiColorDevice<T>>, Error> + 'a {
        Devres::new(
            parent.as_ref(),
            try_pin_init!(MultiColorDevice {
                ops <- ops,
                classdev <- Opaque::try_ffi_init(|ptr: *mut bindings::led_classdev_mc| {
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
                    let mut subleds_vec = KVec::new();
                    subleds_vec.extend_from_slice(subleds, GFP_KERNEL)?;
                    let (subled_info, num_colors, capacity) = subleds_vec.into_raw_parts();
                    debug_assert_eq!(num_colors, capacity);

                    let subled_guard = ScopeGuard::new(|| {
                        // SAFETY: `subled_info` is guaranteed to be a valid array pointer to
                        // `mc_subled` with the length and capacity of `num_colors`.
                        drop(unsafe { KVec::from_raw_parts(subled_info, num_colors, num_colors) });
                    });

                    // SAFETY: `try_ffi_init` guarantees that `ptr` is valid for write.
                    // `led_classdev_mc` gets fully initialized in-place by
                    // `led_classdev_multicolor_register_ext` including `mutex` and `list_head`.
                    unsafe {
                        ptr.write(bindings::led_classdev_mc {
                            led_cdev: bindings::led_classdev {
                                brightness_set: (!T::BLOCKING)
                                    .then_some(Adapter::<T>::brightness_set_callback),
                                brightness_set_blocking: T::BLOCKING
                                    .then_some(Adapter::<T>::brightness_set_blocking_callback),
                                brightness_get: T::HAS_BRIGHTNESS_GET
                                    .then_some(Adapter::<T>::brightness_get_callback),
                                blink_set: T::HAS_BLINK_SET
                                    .then_some(Adapter::<T>::blink_set_callback),
                                max_brightness: T::MAX_BRIGHTNESS,
                                brightness: self.initial_brightness,
                                color: self.color as u32,
                                name: self.name.map_or(core::ptr::null(), CStrExt::as_char_ptr),
                                ..bindings::led_classdev::default()
                            },
                            num_colors: u32::try_from(num_colors)?,
                            // CAST: The safeguards in the const block ensure that
                            // `MultiColorSubLed` has an identical layout to `mc_subled`.
                            subled_info: subled_info.cast::<bindings::mc_subled>(),
                        })
                    };

                    let mut init_data = bindings::led_init_data {
                        fwnode: self
                            .fwnode
                            .as_ref()
                            .map_or(core::ptr::null_mut(), |fwnode| fwnode.as_raw()),
                        default_label: core::ptr::null(),
                        devicename: self
                            .devicename
                            .map_or(core::ptr::null(), CStrExt::as_char_ptr),
                        devname_mandatory: self.devname_mandatory,
                    };

                    // SAFETY:
                    // - `parent.as_ref().as_raw()` is guaranteed to be a pointer to a valid
                    //    `device`.
                    // - `ptr` is guaranteed to be a pointer to an initialized `led_classdev_mc`.
                    to_result(unsafe {
                        bindings::led_classdev_multicolor_register_ext(
                            parent.as_ref().as_raw(),
                            ptr,
                            if self.name.is_none() {
                                &raw mut init_data
                            } else {
                                core::ptr::null_mut()
                            },
                        )
                    })?;

                    subled_guard.dismiss();

                    core::mem::forget(self.fwnode); // keep the reference count incremented

                    Ok::<_, Error>(())
                }),
            }),
        )
    }
}

impl<T: LedOps<Mode = MultiColor>> MultiColorDevice<T> {
    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::MultiColorDevice`.
    unsafe fn from_raw<'a>(led_cdev: *mut bindings::led_classdev) -> &'a Self {
        // SAFETY: The function's contract guarantees that `led_cdev` points to a `led_classdev`
        // field embedded within a valid `led::MultiColorDevice`. `container_of!` can therefore
        // safely calculate the address of the containing struct.
        let led_mc_cdev = unsafe { container_of!(led_cdev, bindings::led_classdev_mc, led_cdev) };

        // SAFETY: It is guaranteed that `led_mc_cdev` points to a `led_classdev_mc`
        // field embedded within a valid `led::MultiColorDevice`. `container_of!` can therefore
        // safely calculate the address of the containing struct.
        unsafe { &*container_of!(Opaque::cast_from(led_mc_cdev), Self, classdev) }
    }

    #[inline]
    fn parent(&self) -> &device::Device<Bound> {
        // SAFETY: `self.classdev.get()` is guaranteed to be a valid pointer to `led_classdev_mc`.
        unsafe { device::Device::from_raw((*(*self.classdev.get()).led_cdev.dev).parent) }
    }

    /// Returns the subleds passed to [`Device::new_multicolor`].
    #[inline]
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
                // CAST: It is guaranteed that `num_colors` fits into an `usize`.
                raw.num_colors as usize,
            )
        }
    }
}

// SAFETY: A `led::MultiColorDevice` can be unregistered from any thread.
unsafe impl<T: LedOps<Mode = MultiColor> + Send> Send for MultiColorDevice<T> {}

// SAFETY: `led::MultiColorDevice` can be shared among threads because all methods of `led::Device`
// are thread safe.
unsafe impl<T: LedOps<Mode = MultiColor> + Sync> Sync for MultiColorDevice<T> {}

struct Adapter<T: LedOps<Mode = MultiColor>> {
    _p: PhantomData<T>,
}

impl<T: LedOps<Mode = MultiColor>> Adapter<T> {
    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::MultiColorDevice`.
    /// This function is called on setting the brightness of a led.
    unsafe extern "C" fn brightness_set_callback(
        led_cdev: *mut bindings::led_classdev,
        brightness: u32,
    ) {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
        // `led_classdev` embedded within a `led::MultiColorDevice`.
        let classdev = unsafe { MultiColorDevice::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        // SAFETY: `classdev.classdev.get()` is guaranteed to be a pointer to a valid
        // `led_classdev_mc`.
        unsafe { bindings::led_mc_calc_color_components(classdev.classdev.get(), brightness) };

        let _ = classdev.ops.brightness_set(parent, classdev, brightness);
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::MultiColorDevice`.
    /// This function is called on setting the brightness of a led immediately.
    unsafe extern "C" fn brightness_set_blocking_callback(
        led_cdev: *mut bindings::led_classdev,
        brightness: u32,
    ) -> i32 {
        from_result(|| {
            // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
            // `led_classdev` embedded within a `led::MultiColorDevice`.
            let classdev = unsafe { MultiColorDevice::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

            // SAFETY: `classdev.classdev.get()` is guaranteed to be a pointer to a valid
            // `led_classdev_mc`.
            unsafe { bindings::led_mc_calc_color_components(classdev.classdev.get(), brightness) };

            classdev.ops.brightness_set(parent, classdev, brightness)?;
            Ok(0)
        })
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::MultiColorDevice`.
    /// This function is called on getting the brightness of a led.
    unsafe extern "C" fn brightness_get_callback(led_cdev: *mut bindings::led_classdev) -> u32 {
        // SAFETY: The function's contract guarantees that `led_cdev` is a valid pointer to a
        // `led_classdev` embedded within a `led::MultiColorDevice`.
        let classdev = unsafe { MultiColorDevice::<T>::from_raw(led_cdev) };
        // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
        let parent = unsafe { T::Bus::from_device(classdev.parent()) };

        classdev.ops.brightness_get(parent, classdev)
    }

    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::MultiColorDevice`.
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
            // `led_classdev` embedded within a `led::MultiColorDevice`.
            let classdev = unsafe { MultiColorDevice::<T>::from_raw(led_cdev) };
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
impl<T: LedOps<Mode = MultiColor>> PinnedDrop for MultiColorDevice<T> {
    fn drop(self: Pin<&mut Self>) {
        let raw = self.classdev.get();
        // SAFETY: The existence of `self` guarantees that `self.classdev.get()` is a pointer to a
        // valid `led_classdev_mc`.
        let dev: &device::Device = unsafe { device::Device::from_raw((*raw).led_cdev.dev) };

        let _fwnode = dev
            .fwnode()
            // SAFETY: the reference count of `fwnode` has previously been
            // incremented in `led::Device::new`.
            .map(|fwnode| unsafe { ARef::from_raw(NonNull::from(fwnode)) });

        // SAFETY: The existence of `self` guarantees that `self.classdev` has previously been
        // successfully registered with `led_classdev_multicolor_register_ext`.
        unsafe { bindings::led_classdev_multicolor_unregister(raw) };

        // SAFETY: `raw` is guaranteed to be a valid pointer to `led_classdev_mc`.
        let led_cdev = unsafe { &*raw };

        // SAFETY: `subled_info` is guaranteed to be a valid array pointer to `mc_subled` with the
        // length and capacity of `led_cdev.num_colors`. See `led::MulticolorDevice::new`.
        drop(unsafe {
            KVec::from_raw_parts(
                led_cdev.subled_info,
                led_cdev.num_colors as usize,
                led_cdev.num_colors as usize,
            )
        });
    }
}
