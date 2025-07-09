// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! Sample DebugFS exporting platform driver
//!
//! To successfully probe this driver with ACPI, use an ssdt that looks like
//!
//! ```dsl
//! DefinitionBlock ("", "SSDT", 2, "TEST", "VIRTACPI", 0x00000001)
//!{
//!    Scope (\_SB)
//!    {
//!        Device (T432)
//!        {
//!            Name (_HID, "LNUXDEBF")  // ACPI hardware ID to match
//!            Name (_UID, 1)
//!            Name (_STA, 0x0F)        // Device present, enabled
//!            Name (_DSD, Package () { // Sample attribute
//!                ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
//!                Package() {
//!                    Package(2) {"compatible", "sample-debugfs"}
//!                }
//!            })
//!            Name (_CRS, ResourceTemplate ()
//!            {
//!                Memory32Fixed (ReadWrite, 0xFED00000, 0x1000)
//!            })
//!        }
//!    }
//!}
//! ```

use core::sync::atomic::AtomicUsize;
use core::sync::atomic::Ordering;
use kernel::c_str;
use kernel::debugfs::{Dir, File};
use kernel::new_mutex;
use kernel::prelude::*;
use kernel::sync::Mutex;

use kernel::{acpi, device::Core, of, platform, str::CString, types::ARef};

kernel::module_platform_driver! {
    type: Wrapper,
    name: "rust_debugfs",
    authors: ["Matthew Maurer"],
    description: "Rust DebugFS usage sample",
    license: "GPL",
}

// This data structure would be unlikely to be there in a real driver - it's to hook up mutation
// that would normally be driven by whatever the driver was actually servicing and show how that
// would work. We're assuming here that those methods would have access to a `&RustDebugFs`.
#[pin_data]
struct Wrapper {
    _dir: Dir,
    #[pin]
    _wrapped: File<File<RustDebugFs>>,
}

#[pin_data]
struct RustDebugFs {
    pdev: ARef<platform::Device>,
    // As we only hold these for drop effect (to remove the directory/files) we have a leading
    // underscore to indicate to the compiler that we don't expect to use this field directly.
    _debugfs: Dir,
    #[pin]
    _compatible: File<CString>,
    #[pin]
    counter: File<File<AtomicUsize>>,
    #[pin]
    inner: File<Mutex<Inner>>,
}

#[derive(Debug)]
struct Inner {
    x: u32,
    y: u32,
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <Wrapper as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("test,rust-debugfs-device")), ())]
);

kernel::acpi_device_table!(
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <Wrapper as platform::Driver>::IdInfo,
    [(acpi::DeviceId::new(c_str!("LNUXDEBF")), ())]
);

impl platform::Driver for Wrapper {
    type IdInfo = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        _info: Option<&Self::IdInfo>,
    ) -> Result<Pin<KBox<Self>>> {
        KBox::try_pin_init(Wrapper::new(RustDebugFs::new(pdev)), GFP_KERNEL)
    }
}

impl Wrapper {
    /// This builds two debugfs files that would be unusual to exist in the real world to emulate
    /// actions taken servicing the device. They trigger their action when the debugfs file is
    /// opened.
    fn build_control<I: PinInit<RustDebugFs, Error>>(
        dir: &Dir,
        init: I,
    ) -> impl PinInit<File<File<RustDebugFs>>, Error> + use<'_, I> {
        let swap = dir.fmt_file(c_str!("swap"), init, &|sample, fmt| {
            let mut guard = sample.inner.lock();
            let x = guard.x;
            guard.x = guard.y;
            guard.y = x;
            writeln!(fmt, "Swapped!")
        });

        dir.fmt_file(c_str!("add_counter"), swap, &|sample, fmt| {
            let mut inner = sample.inner.lock();
            inner.x += sample.counter.load(Ordering::Relaxed) as u32;
            writeln!(fmt, "Counter added!")
        })
    }

    fn new<I: PinInit<RustDebugFs, Error>>(init: I) -> impl PinInit<Self, Error> + use<I> {
        let dir = Dir::new(c_str!("sample_control"));
        try_pin_init! {
            Self {
                _wrapped <- Wrapper::build_control(&dir, init),
                _dir: dir,
            } ? Error
        }
    }
}

impl RustDebugFs {
    fn build_counter(dir: &Dir) -> impl PinInit<File<File<AtomicUsize>>> + use<'_> {
        let counter = dir.fmt_file(c_str!("counter"), AtomicUsize::new(0), &|counter, fmt| {
            writeln!(fmt, "{}", counter.load(Ordering::Relaxed))
        });
        dir.fmt_file(c_str!("inc_counter"), counter, &|counter, fmt| {
            writeln!(fmt, "{}", counter.fetch_add(1, Ordering::Relaxed))
        })
    }

    fn build_inner(dir: &Dir) -> impl PinInit<File<Mutex<Inner>>> + use<'_> {
        dir.fmt_file(
            c_str!("pair"),
            new_mutex!(Inner { x: 3, y: 10 }),
            &|i, fmt| writeln!(fmt, "{:?}", *i.lock()),
        )
    }

    fn new(pdev: &platform::Device<Core>) -> impl PinInit<Self, Error> + use<'_> {
        let debugfs = Dir::new(c_str!("sample_debugfs"));
        let dev = pdev.as_ref();

        try_pin_init! {
            Self {
                _compatible <- debugfs.fmt_file(
                    c_str!("compatible"),
                    dev.fwnode()
                        .ok_or(ENOENT)?
                        .property_read::<CString>(c_str!("compatible"))
                        .required_by(dev)?,
                    &|cs, w| writeln!(w, "{cs:?}"),
                ),
                counter <- Self::build_counter(&debugfs),
                inner <- Self::build_inner(&debugfs),
                _debugfs: debugfs,
                pdev: pdev.into(),
            }
        }
    }
}
