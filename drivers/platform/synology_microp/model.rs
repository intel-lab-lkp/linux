// SPDX-License-Identifier: GPL-2.0

use kernel::led::Color;

pub(crate) struct Model {
    pub(crate) led_power: Color,
    pub(crate) led_alert: Option<Color>,
    pub(crate) led_usb_copy: bool,
    pub(crate) led_esata: bool,
}

impl Model {
    pub(super) const fn new() -> Self {
        Self {
            led_power: Color::Blue,
            led_alert: None,
            led_usb_copy: false,
            led_esata: false,
        }
    }

    pub(super) const fn led_power(self, color: Color) -> Self {
        Self {
            led_power: color,
            ..self
        }
    }

    pub(super) const fn led_alert(self, color: Color) -> Self {
        Self {
            led_alert: Some(color),
            ..self
        }
    }

    pub(super) const fn led_esata(self) -> Self {
        Self {
            led_esata: true,
            ..self
        }
    }

    pub(super) const fn led_usb_copy(self) -> Self {
        Self {
            led_usb_copy: true,
            ..self
        }
    }
}
