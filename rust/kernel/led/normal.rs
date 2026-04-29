// SPDX-License-Identifier: GPL-2.0

//! Led mode for the `struct led_classdev`.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use super::*;

/// The led class device representation.
///
/// This structure represents the Rust abstraction for a led class device.
#[pin_data(PinnedDrop)]
pub struct Device<T: LedOps> {
    #[pin]
    ops: T,
    #[pin]
    classdev: Opaque<bindings::led_classdev>,
}

impl<'a, S: DeviceBuilderState> DeviceBuilder<'a, S> {
    /// Registers a new [`Device`].
    pub fn build<T: LedOps>(
        self,
        parent: &'a T::Bus,
        ops: impl PinInit<T, Error> + 'a,
    ) -> impl PinInit<Devres<Device<T>>, Error> + 'a {
        Devres::new(
            parent.as_ref(),
            try_pin_init!(Device {
                ops <- ops,
                classdev <- Opaque::try_ffi_init(|ptr: *mut bindings::led_classdev| {
                    // SAFETY: `try_ffi_init` guarantees that `ptr` is valid for write.
                    // `led_classdev` gets fully initialized in-place by
                    // `led_classdev_register_ext` including `mutex` and `list_head`.
                    unsafe {
                        ptr.write(bindings::led_classdev {
                            brightness_set: (!T::BLOCKING)
                                .then_some(Adapter::<T>::brightness_set_callback),
                            brightness_set_blocking: T::BLOCKING
                                .then_some(Adapter::<T>::brightness_set_blocking_callback),
                            brightness_get: T::HAS_BRIGHTNESS_GET
                                .then_some(Adapter::<T>::brightness_get_callback),
                            blink_set: T::HAS_BLINK_SET.then_some(Adapter::<T>::blink_set_callback),
                            max_brightness: T::MAX_BRIGHTNESS,
                            brightness: self.initial_brightness,
                            color: self.color as u32,
                            name: self.name.map_or(core::ptr::null(), CStrExt::as_char_ptr),
                            ..bindings::led_classdev::default()
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
                    // - `ptr` is guaranteed to be a pointer to an initialized `led_classdev`.
                    to_result(unsafe {
                        bindings::led_classdev_register_ext(
                            parent.as_ref().as_raw(),
                            ptr,
                            if self.name.is_none() {
                                &raw mut init_data
                            } else {
                                core::ptr::null_mut()
                            },
                        )
                    })?;

                    core::mem::forget(self.fwnode); // keep the reference count incremented

                    Ok::<_, Error>(())
                }),
            }),
        )
    }
}

impl<T: LedOps> Device<T> {
    /// # Safety
    /// `led_cdev` must be a valid pointer to a `led_classdev` embedded within a
    /// `led::Device`.
    unsafe fn from_raw<'a>(led_cdev: *mut bindings::led_classdev) -> &'a Self {
        // SAFETY: The function's contract guarantees that `led_cdev` points to a `led_classdev`
        // field embedded within a valid `led::Device`. `container_of!` can therefore
        // safely calculate the address of the containing struct.
        unsafe { &*container_of!(Opaque::cast_from(led_cdev), Self, classdev) }
    }

    #[inline]
    fn parent(&self) -> &device::Device<Bound> {
        // SAFETY: `self.classdev.get()` is guaranteed to be a valid pointer to `led_classdev`.
        unsafe { device::Device::from_raw((*(*self.classdev.get()).dev).parent) }
    }
}

// SAFETY: A `led::Device` can be unregistered from any thread.
unsafe impl<T: LedOps + Send> Send for Device<T> {}

// SAFETY: `led::Device` can be shared among threads because all methods of `led::Device`
// are thread safe.
unsafe impl<T: LedOps + Sync> Sync for Device<T> {}

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
            // `led_classdev` embedded within a `led::Device`.
            let classdev = unsafe { Device::<T>::from_raw(led_cdev) };
            // SAFETY: `classdev.parent()` is guaranteed to be contained in `T::Bus`.
            let parent = unsafe { T::Bus::from_device(classdev.parent()) };

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
        // `led_classdev` embedded within a `led::Device`.
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
            // `led_classdev` embedded within a `led::Device`.
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
        // valid `led_classdev`.
        let dev: &device::Device = unsafe { device::Device::from_raw((*raw).dev) };

        let _fwnode = dev
            .fwnode()
            // SAFETY: the reference count of `fwnode` has previously been
            // incremented in `led::Device::new`.
            .map(|fwnode| unsafe { ARef::from_raw(NonNull::from(fwnode)) });

        // SAFETY: The existence of `self` guarantees that `self.classdev` has previously been
        // successfully registered with `led_classdev_register_ext`.
        unsafe { bindings::led_classdev_unregister(raw) };
    }
}
