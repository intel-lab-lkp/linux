// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device::Bound,
    devres::{
        self,
        Devres, //
    },
    led::{
        self,
        LedOps,
        MultiColorSubLed, //
    },
    new_mutex,
    prelude::*,
    serdev,
    str::CString,
    sync::Mutex, //
};
use pin_init::pin_init_scope;

use crate::{
    command::Command,
    model::Model, //
};

#[pin_data]
pub(crate) struct Data {
    #[pin]
    status: Devres<led::MultiColorDevice<StatusLedHandler>>,
    power_name: CString,
    #[pin]
    power: Devres<led::Device<LedHandler>>,
}

impl Data {
    pub(super) fn register<'a>(
        dev: &'a serdev::Device<Bound>,
        model: &'a Model,
    ) -> impl PinInit<Self, Error> + 'a {
        pin_init_scope(move || {
            if let Some(color) = model.led_alert {
                let name = CString::try_from_fmt(fmt!("{}:alarm", color.as_c_str().to_str()?))?;
                devres::register(
                    dev.as_ref(),
                    led::DeviceBuilder::new().color(color).name(&name).build(
                        dev,
                        try_pin_init!(LedHandler {
                            blink <- new_mutex!(false),
                            command: Command::AlertLed,
                        }),
                    ),
                    GFP_KERNEL,
                )?;
            }

            if model.led_usb_copy {
                devres::register(
                    dev.as_ref(),
                    led::DeviceBuilder::new()
                        .color(led::Color::Green)
                        .name(c"green:usb")
                        .build(
                            dev,
                            try_pin_init!(LedHandler {
                                blink <- new_mutex!(false),
                                command: Command::UsbLed,
                            }),
                        ),
                    GFP_KERNEL,
                )?;
            }

            if model.led_esata {
                devres::register(
                    dev.as_ref(),
                    led::DeviceBuilder::new()
                        .color(led::Color::Green)
                        .name(c"green:esata")
                        .build(
                            dev,
                            try_pin_init!(LedHandler {
                                blink <- new_mutex!(false),
                                command: Command::EsataLed,
                            }),
                        ),
                    GFP_KERNEL,
                )?;
            }

            Ok(try_pin_init!(Self {
                status <- led::DeviceBuilder::new()
                    .color(led::Color::Multi)
                    .name(c"multicolor:status")
                    .build_multicolor(
                        dev,
                        try_pin_init!(StatusLedHandler {
                            blink <- new_mutex!(false),
                        }),
                        StatusLedHandler::SUBLEDS,
                    ),
                power_name: CString::try_from_fmt(fmt!(
                    "{}:power",
                    model.led_power.as_c_str().to_str()?
                ))?,
                power <- led::DeviceBuilder::new()
                    .color(model.led_power)
                    .name(power_name)
                    .build(
                        dev,
                        try_pin_init!(LedHandler {
                            blink <- new_mutex!(true),
                            command: Command::PowerLed,
                        }),
                    ),
            }))
        })
    }
}

#[derive(Copy, Clone)]
pub(crate) enum StatusLedColor {
    Green,
    Orange,
}

#[derive(Copy, Clone)]
pub(crate) enum State {
    On,
    Blink,
    Off,
}

#[pin_data]
struct LedHandler {
    #[pin]
    blink: Mutex<bool>,
    command: fn(State) -> Command,
}

/// Blink delay measured using video recording on DS923+ for Power and Status Led.
///
/// We assume it is the same for all other leds and models.
const BLINK_DELAY: usize = 167;

#[vtable]
impl LedOps for LedHandler {
    type Bus = serdev::Device<Bound>;
    type Mode = led::Normal;
    const BLOCKING: bool = true;
    const MAX_BRIGHTNESS: u32 = 1;

    fn brightness_set(
        &self,
        dev: &Self::Bus,
        _classdev: &led::Device<Self>,
        brightness: u32,
    ) -> Result<()> {
        let mut blink = self.blink.lock();
        (self.command)(if brightness == 0 {
            *blink = false;
            State::Off
        } else if *blink {
            State::Blink
        } else {
            State::On
        })
        .write(dev)?;

        Ok(())
    }

    fn blink_set(
        &self,
        dev: &Self::Bus,
        _classdev: &led::Device<Self>,
        delay_on: &mut usize,
        delay_off: &mut usize,
    ) -> Result<()> {
        let mut blink = self.blink.lock();

        (self.command)(if *delay_on == 0 && *delay_off != 0 {
            State::Off
        } else if *delay_on != 0 && *delay_off == 0 {
            State::On
        } else {
            *blink = true;
            *delay_on = BLINK_DELAY;
            *delay_off = BLINK_DELAY;

            State::Blink
        })
        .write(dev)
    }
}

#[pin_data]
struct StatusLedHandler {
    #[pin]
    blink: Mutex<bool>,
}

impl StatusLedHandler {
    const SUBLEDS: &[MultiColorSubLed] = &[
        MultiColorSubLed::new(led::Color::Green).initial_intensity(1),
        MultiColorSubLed::new(led::Color::Orange),
    ];
}

#[vtable]
impl LedOps for StatusLedHandler {
    type Bus = serdev::Device<Bound>;
    type Mode = led::MultiColor;
    const BLOCKING: bool = true;
    const MAX_BRIGHTNESS: u32 = 1;

    fn brightness_set(
        &self,
        dev: &Self::Bus,
        classdev: &led::MultiColorDevice<Self>,
        brightness: u32,
    ) -> Result<()> {
        let mut blink = self.blink.lock();
        if brightness == 0 {
            *blink = false;
        }

        let (color, subled_brightness) = if classdev.subleds()[1].intensity == 0 {
            (StatusLedColor::Green, classdev.subleds()[0].brightness)
        } else {
            (StatusLedColor::Orange, classdev.subleds()[1].brightness)
        };

        Command::StatusLed(
            color,
            if subled_brightness == 0 {
                State::Off
            } else if *blink {
                State::Blink
            } else {
                State::On
            },
        )
        .write(dev)
    }

    fn blink_set(
        &self,
        dev: &Self::Bus,
        classdev: &led::MultiColorDevice<Self>,
        delay_on: &mut usize,
        delay_off: &mut usize,
    ) -> Result<()> {
        let mut blink = self.blink.lock();
        *blink = true;

        let (color, subled_intensity) = if classdev.subleds()[1].intensity == 0 {
            (StatusLedColor::Green, classdev.subleds()[0].intensity)
        } else {
            (StatusLedColor::Orange, classdev.subleds()[1].intensity)
        };
        Command::StatusLed(
            color,
            if *delay_on == 0 && *delay_off != 0 {
                *blink = false;
                State::Off
            } else if subled_intensity == 0 {
                State::Off
            } else if *delay_on != 0 && *delay_off == 0 {
                *blink = false;
                State::On
            } else {
                *delay_on = BLINK_DELAY;
                *delay_off = BLINK_DELAY;

                State::Blink
            },
        )
        .write(dev)
    }
}
