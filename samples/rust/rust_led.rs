// SPDX-License-Identifier: GPL-2.0
//! Rust LED sample.

use core::time::Duration;

use kernel::c_str;
use kernel::leds::{Brightness, Color, Led, LedConfig, Operations};
use kernel::prelude::*;

module! {
    type: RustLed,
    name: "rust_led",
    author: "Rust for Linux Contributors",
    description: "Rust led sample",
    license: "GPL",
}

/// Rust LED sample driver
// Hold references in scope as droping would unregister the LED
#[allow(dead_code)]
struct RustLed {
    /// LED which just supports on/off.
    on_off: Pin<Box<Led<LedOnOff>>>,
    /// LED which supports brightness levels and blinking.
    blink: Pin<Box<Led<LedBlinking>>>,
}

impl kernel::Module for RustLed {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("registering on_off led\n");
        let on_off = Box::pin_init(
            Led::register_with_name(
                c_str!("sample:red:on_off"),
                None,
                &LedConfig { color: Color::Red },
                LedOnOff,
            ),
            GFP_KERNEL,
        )?;

        let blink = Box::pin_init(
            Led::register_with_name(
                c_str!("sample:green:blink"),
                None,
                &LedConfig {
                    color: Color::Green,
                },
                LedBlinking,
            ),
            GFP_KERNEL,
        )?;

        Ok(Self { on_off, blink })
    }
}

struct LedOnOff;

#[vtable]
impl Operations for LedOnOff {
    const MAX_BRIGHTNESS: u8 = 1;

    fn brightness_set(_this: &mut Led<Self>, brightness: Brightness) {
        match brightness {
            Brightness::Off => pr_info!("Switching led off\n"),
            Brightness::On(v) => pr_info!("Switching led on: {}\n", v.get()),
        }
    }
}

struct LedBlinking;

impl LedBlinking {
    const HW_DURATION: Duration = Duration::from_millis(1000);
}

#[vtable]
impl Operations for LedBlinking {
    const MAX_BRIGHTNESS: u8 = 255;

    fn brightness_set(_this: &mut Led<Self>, brightness: Brightness) {
        match brightness {
            Brightness::Off => pr_info!("blinking: off\n"),
            Brightness::On(v) => pr_info!("blinking: on at {}\n", v.get()),
        }
    }

    fn blink_set(
        _this: &mut Led<Self>,
        delay_on: Duration,
        delay_off: Duration,
    ) -> Result<(Duration, Duration)> {
        pr_info!("blinking: try delay {delay_on:?} {delay_off:?}\n");
        if !(delay_on.is_zero() && delay_off.is_zero())
            && !(delay_on == Self::HW_DURATION && delay_off == Self::HW_DURATION)
        {
            return Err(EINVAL);
        }

        pr_info!("blinking: setting dealy\n");
        Ok((Self::HW_DURATION, Self::HW_DURATION))
    }
}
