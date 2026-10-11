// SPDX-License-Identifier: GPL-2.0

//! Rust MIDI minimal sample.

use kernel::{
    faux,
    prelude::*,
    snd,
};

impl kernel::Module for RustMidiDeviceModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_debug!("Initializing sound card\n");
        let reg = faux::Registration::new(c"rust-midi-device", None)?;
        let fdev = reg.as_ref();
        let mut card = snd::SoundCard::new(
            fdev.as_ref(),
            c"RustMIDI",
            kernel::module::this_module::<RustMidiDeviceModule>(),
        )?;
        card.set_driver(c"rust-midi")?;
        card.set_shortname(c"RustMIDI")?;

        // Register the sndcard to ALSA.
        card.register()?;

        Ok(Self { _reg: reg })
    }
}

struct RustMidiDeviceModule {
    _reg: faux::Registration,
}

module! {
    type: RustMidiDeviceModule,
    name: "rust_midi_device",
    authors: ["Rust for Linux Contributors"],
    description: "Rust MIDI device sample",
    license: "GPL",
}
