// SPDX-License-Identifier: GPL-2.0

//! Rust MIDI minimal sample.
//!
//! This driver demonstrates usage of the kernel::snd subsystem:
//! - Create a rawmidi device
//! - Print received event
//! - Periodically send note events
//!
//! Its goal is to simulate a real device driver usage.

use core::sync::atomic::{AtomicBool, Ordering};
use kernel::{
    faux,
    prelude::*,
    snd,
    sync::Arc,
    workqueue,
    workqueue::{DelayedWork, WorkItem},
};

// The internal state data to perform a hardware simulation.
#[pin_data]
struct RustMidiDevice {
    // toggle sent note on and off.
    note_playing: AtomicBool,
    #[pin]
    // substream to send note.
    input_substream: snd::ActiveMidiSubstream,
    #[pin]
    // delayed work to send note periodically.
    timer_work: DelayedWork<RustMidiDevice>,
}

kernel::impl_has_delayed_work! {
    impl HasDelayedWork<Self> for RustMidiDevice { self.timer_work }
}

impl RustMidiDevice {
    // Initialize the simulation internal state.
    fn new() -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(RustMidiDevice {
                note_playing: AtomicBool::new(false),
                input_substream <- snd::ActiveMidiSubstream::new(),
                timer_work <- kernel::new_delayed_work!("RustMidiDevice::timer_work"),
            }),
            GFP_KERNEL,
        )
    }
}

impl WorkItem for RustMidiDevice {
    type Pointer = Arc<Self>;

    // Send a note event, as-if coming from external interrupt.
    fn run(this: Self::Pointer) {
        this.input_substream.with_active(|substream| {
            let playing = this.note_playing.fetch_not(Ordering::Relaxed);
            let velocity = if playing { 0x0 } else { 0x7F };
            let event = [0x90, 0x42, velocity];
            match substream.receive(&event) {
                Ok(count) => pr_info!("worker submitted {} ({:?})", count, event),
                Err(err) => pr_err!("worker submit failed: {:?}", err),
            };
        });
        pr_debug!("Periodic worker completed!");

        // Re-enqueue continuously every 500 jiffies
        let _ = workqueue::system().enqueue_delayed(this, 500);
    }
}

// Implement midi input ops: store the substream for external context.
impl snd::RawMidiInput for RustMidiDevice {
    fn close(&self, _substream: &snd::RawMidiSubstream) -> Result<()> {
        pr_debug!("input close!\n");
        // note: maybe input_substream should be managed by the binding,
        // otherwise the user may forget to call clear when the device is closed.
        self.input_substream.clear();
        Ok(())
    }
    fn trigger(&self, substream: &snd::RawMidiSubstream, up: bool) {
        pr_debug!("input trigger up:{up}!\n");
        if up {
            self.input_substream.store(substream);
        } else {
            self.input_substream.clear();
        }
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
        let state = RustMidiDevice::new()?;

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

        pr_debug!("Kick off the background worker\n");
        let _ = workqueue::system().enqueue_delayed(state.clone(), 0);

        Ok(Self { _reg: reg, state })
    }
}

struct RustMidiDeviceModule {
    state: Arc<RustMidiDevice>,
    _reg: faux::Registration,
}

impl Drop for RustMidiDeviceModule {
    fn drop(&mut self) {
        // Safety: the timer_work is a valid delayed work. It seems necessary to cancel the work
        // to avoid a panic when unloading the module. This is just for simulation purpose.
        // TODO: add a safe cancel helper to kernel::workqueue and remove unsafe bindings here
        unsafe {
            let dwork_ptr = core::ptr::from_ref(&self.state.timer_work) as *mut _;
            kernel::bindings::cancel_delayed_work_sync(dwork_ptr);
        }
    }
}

module! {
    type: RustMidiDeviceModule,
    name: "rust_midi_device",
    authors: ["Rust for Linux Contributors"],
    description: "Rust MIDI device sample",
    license: "GPL",
}
