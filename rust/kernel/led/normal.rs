// SPDX-License-Identifier: GPL-2.0

//! Led mode for the `struct led_classdev`.
//!
//! C header: [`include/linux/leds.h`](srctree/include/linux/leds.h)

use super::*;

/// The led mode for the `struct led_classdev`. Leds with this mode can only have a fixed color.
pub enum Normal {}

impl private::Mode for Normal {
    type Type = bindings::led_classdev;
    const REGISTER: RegisterFunc<Self::Type> = bindings::led_classdev_register_ext;
    const UNREGISTER: UnregisterFunc<Self::Type> = bindings::led_classdev_unregister;

    unsafe fn device<'a>(raw: *mut Self::Type) -> &'a device::Device {
        // SAFETY:
        // - The function's contract guarantees that `raw` is a valid pointer to `led_classdev`.
        unsafe { device::Device::from_raw((*raw).dev) }
    }

    unsafe fn from_classdev(led_cdev: *mut bindings::led_classdev) -> *mut Self::Type {
        led_cdev
    }
}

impl<T: LedOps<Mode = Normal>> Device<T> {
    /// Registers a new led classdev.
    ///
    /// The [`Device`] will be unregistered on drop.
    pub fn new<'a>(
        parent: &'a T::Bus,
        init_data: InitData<'a>,
        ops: T,
    ) -> impl PinInit<Devres<Self>, Error> + 'a {
        Self::__new(parent, init_data, ops, Ok)
    }
}
