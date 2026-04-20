// SPDX-License-Identifier: GPL-2.0

use kernel::{
    device::Bound,
    error::Result,
    serdev, //
};

use crate::led;

#[expect(
    clippy::enum_variant_names,
    reason = "future variants will not end with Led"
)]
pub(crate) enum Command {
    PowerLed(led::State),
    StatusLed(led::StatusLedColor, led::State),
    AlertLed(led::State),
    UsbLed(led::State),
    EsataLed(led::State),
}

impl Command {
    pub(crate) fn write(self, dev: &serdev::Device<Bound>) -> Result {
        dev.write_all(
            match self {
                Self::PowerLed(led::State::On) => &[0x34],
                Self::PowerLed(led::State::Blink) => &[0x35],
                Self::PowerLed(led::State::Off) => &[0x36],

                Self::StatusLed(_, led::State::Off) => &[0x37],
                Self::StatusLed(led::StatusLedColor::Green, led::State::On) => &[0x38],
                Self::StatusLed(led::StatusLedColor::Green, led::State::Blink) => &[0x39],
                Self::StatusLed(led::StatusLedColor::Orange, led::State::On) => &[0x3A],
                Self::StatusLed(led::StatusLedColor::Orange, led::State::Blink) => &[0x3B],

                Self::AlertLed(led::State::On) => &[0x4C, 0x41, 0x31],
                Self::AlertLed(led::State::Blink) => &[0x4C, 0x41, 0x32],
                Self::AlertLed(led::State::Off) => &[0x4C, 0x41, 0x33],

                Self::UsbLed(led::State::On) => &[0x40],
                Self::UsbLed(led::State::Blink) => &[0x41],
                Self::UsbLed(led::State::Off) => &[0x42],

                Self::EsataLed(led::State::On) => &[0x4C, 0x45, 0x31],
                Self::EsataLed(led::State::Blink) => &[0x4C, 0x45, 0x32],
                Self::EsataLed(led::State::Off) => &[0x4C, 0x45, 0x33],
            },
            serdev::Timeout::Max,
        )?;
        dev.wait_until_sent(serdev::Timeout::Max);
        Ok(())
    }
}
