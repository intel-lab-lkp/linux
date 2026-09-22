// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2025 Google LLC.

//! Sample DebugFS exporting platform driver
//!
//! To successfully probe this driver with ACPI, use an ssdt that looks like
//!
//! ```dsl
//! DefinitionBlock ("", "SSDT", 2, "TEST", "VIRTACPI", 0x00000001)
//! {
//!    Scope (\_SB)
//!    {
//!        Device (T432)
//!        {
//!            Name (_HID, "LNUXBEEF")  // ACPI hardware ID to match
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
//! }
//! ```

use core::str::FromStr;
use kernel::{
    acpi,
    c_str,
    debugfs::{
        Dir,
        File, //
    },
    device::Core,
    new_mutex,
    of,
    platform,
    prelude::*,
    sizes::*,
    str::CString,
    sync::{
        aref::ARef,
        atomic::{
            Atomic,
            Relaxed, //
        },
        Mutex,
    }, //
};

kernel::module_platform_driver! {
    type: RustDebugFs,
    name: "rust_debugfs",
    authors: ["Matthew Maurer"],
    description: "Rust DebugFS usage sample",
    license: "GPL",
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
    counter: File<Atomic<usize>>,
    #[pin]
    inner: File<Mutex<Inner>>,
    #[pin]
    array_blob: File<Mutex<[u8; 4]>>,
    #[pin]
    vector_blob: File<Mutex<KVec<u8>>>,
    #[pin]
    primitive_values: PrimitiveValues,
}

#[derive(Debug)]
struct Inner {
    x: u32,
    y: u32,
}

struct PrimitiveValues {
    u8: u8,
    u16: u16,
    u32: u32,
    u64: u64,
    x8: u8,
    x16: u16,
    x32: u32,
    x64: u64,
    size_t: usize,
    bool: bool,
}

impl PrimitiveValues {
    fn new() -> Self {
        Self {
            u8: 8,
            u16: 16,
            u32: 32,
            u64: 64,
            x8: 0x88,
            x16: 0x1616,
            x32: 0x32323232,
            x64: 0x6464646464646464,
            size_t: 12345678,
            bool: true,
        }
    }
}

impl FromStr for Inner {
    type Err = Error;
    fn from_str(s: &str) -> Result<Self> {
        let mut parts = s.split_whitespace();
        let x = parts
            .next()
            .ok_or(EINVAL)?
            .parse::<u32>()
            .map_err(|_| EINVAL)?;
        let y = parts
            .next()
            .ok_or(EINVAL)?
            .parse::<u32>()
            .map_err(|_| EINVAL)?;
        if parts.next().is_some() {
            return Err(EINVAL);
        }
        Ok(Inner { x, y })
    }
}

kernel::acpi_device_table!(
    ACPI_TABLE,
    <RustDebugFs as platform::Driver>::IdInfo,
    [(acpi::DeviceId::new(c"LNUXBEEF"), ())]
);

impl platform::Driver for RustDebugFs {
    type IdInfo = ();
    type Data<'bound> = Self;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = None;
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self, Error> + 'bound {
        RustDebugFs::new(pdev).pin_chain(|this| {
            this.counter.store(91, Relaxed);
            {
                let mut guard = this.inner.lock();
                guard.x = guard.y;
                guard.y = 42;
            }

            Ok(())
        })
    }
}

impl RustDebugFs {
    fn build_counter(dir: &Dir) -> impl PinInit<File<Atomic<usize>>> + '_ {
        dir.read_write_file(c"counter", Atomic::<usize>::new(0))
    }

    fn build_inner(dir: &Dir) -> impl PinInit<File<Mutex<Inner>>> + '_ {
        dir.read_write_file(c"pair", new_mutex!(Inner { x: 3, y: 10 }))
    }

    fn new<'a, 'b>(
        pdev: &'a platform::Device<Core<'b>>,
    ) -> impl PinInit<Self, Error> + use<'a, 'b> {
        let debugfs = Dir::new(c"sample_debugfs");
        let dev = pdev.as_ref();

        try_pin_init! {
            Self {
                _compatible <- debugfs.read_only_file(
                    c"compatible",
                    dev.fwnode()
                        .ok_or(ENOENT)?
                        .property_read::<CString>(c"compatible")
                        .required_by(dev)?,
                ),
                counter <- Self::build_counter(&debugfs),
                inner <- Self::build_inner(&debugfs),
                array_blob <- debugfs.read_write_binary_file(
                    c"array_blob",
                    new_mutex!([0x62, 0x6c, 0x6f, 0x62]),
                ),
                vector_blob <- debugfs.read_write_binary_file(
                    c"vector_blob",
                    new_mutex!(kernel::kvec!(0x42; SZ_4K)?),
                ),
                primitive_values <- PrimitiveValues::new(),
                _debugfs: debugfs,
                pdev: pdev.into(),
            }
        }
        .pin_chain(|this| {
            // SAFETY: primitive values live until the module is dropped
            unsafe {
                this._debugfs.create_u8(
                    c_str!("u8"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.u8) as *mut _,
                );
                this._debugfs.create_u16(
                    c_str!("u16"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.u16) as *mut _,
                );
                this._debugfs.create_u32(
                    c_str!("u32"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.u32) as *mut _,
                );
                this._debugfs.create_u64(
                    c_str!("u64"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.u64) as *mut _,
                );
                this._debugfs.create_x8(
                    c_str!("x8"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.x8) as *mut _,
                );
                this._debugfs.create_x16(
                    c_str!("x16"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.x16) as *mut _,
                );
                this._debugfs.create_x32(
                    c_str!("x32"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.x32) as *mut _,
                );
                this._debugfs.create_x64(
                    c_str!("x64"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.x64) as *mut _,
                );
                this._debugfs.create_bool(
                    c_str!("bool"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.bool) as *mut _,
                );
                this._debugfs.create_size_t(
                    c_str!("usize"),
                    0644,
                    core::ptr::addr_of!(this.primitive_values.size_t) as *mut _,
                );
            }

            Ok(())
        })
    }
}
