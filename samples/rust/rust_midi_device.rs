// SPDX-License-Identifier: GPL-2.0

//! Rust MIDI minimal sample.
//!
//! This driver demonstrates usage of the kernel::snd subsystem:
//! - Create a rawmidi device
//! - Print received event

use kernel::{
    faux,
    prelude::*,
    snd,
    sync::Arc,
};

struct RustMidiDevice;

impl snd::RawMidiInput for RustMidiDevice {
    fn trigger(&self, _substream: &snd::RawMidiSubstream, up: bool) {
        pr_debug!("input trigger up:{up}!\n");
    }
}

// Implement midi output ops: just display transmitted event.
impl snd::RawMidiOutput for RustMidiDevice {
    fn trigger(&self, substream: &snd::RawMidiSubstream, up: bool) {
        pr_debug!("output trigger up:{up}!\n");
        if up {
            let mut buf = [0; 64];
            match substream.transmit(&mut buf) {
                Ok(count) => pr_info!("transmit got: {:?}\n", buf[..count]),
                Err(err) => pr_err!("transmit failed: {err:?}"),
            };
        }
    }
}

impl kernel::Module for RustMidiDeviceModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        let state = Arc::new(RustMidiDevice, GFP_KERNEL)?;

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

        pr_debug!("Initializing rawmidi card\n");
        let mut rmidi = snd::RawMidi::new(&card, c"RustRMIDI", 0, 1, 1, state.clone())?;
        rmidi.set_name(c"RustRMIDI")?;

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
