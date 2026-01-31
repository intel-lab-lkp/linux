// SPDX-License-Identifier: GPL-2.0

//! Rust PCI driver sample (based on QEMU's `pci-testdev`).
//!
//! To make this driver probe, QEMU must be run with `-device pci-testdev`.

use kernel::{
    bindings,
    device::Bound,
    device::Core,
    devres::Devres,
    io::Io,
    pci,
    prelude::*,
    sync::aref::ARef, //
};

struct Regs;

impl Regs {
    const TEST: usize = 0x0;
    const OFFSET: usize = 0x4;
    const DATA: usize = 0x8;
    const COUNT: usize = 0xC;
    const END: usize = 0x10;
}

type Bar0 = pci::Bar<{ Regs::END }>;

#[derive(Copy, Clone, Debug)]
struct TestIndex(u8);

impl TestIndex {
    const NO_EVENTFD: Self = Self(0);
}

#[pin_data(PinnedDrop)]
struct SampleDriver {
    pdev: ARef<pci::Device>,
    #[pin]
    bar: Devres<Bar0>,
    index: TestIndex,
}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <SampleDriver as pci::Driver>::IdInfo,
    [(
        pci::DeviceId::from_id(pci::Vendor::REDHAT, 0x5),
        TestIndex::NO_EVENTFD
    )]
);

impl SampleDriver {
    fn testdev(index: &TestIndex, bar: &Bar0) -> Result<u32> {
        // Select the test.
        bar.write8(index.0, Regs::TEST);

        let offset = bar.read32(Regs::OFFSET) as usize;
        let data = bar.read8(Regs::DATA);

        // Write `data` to `offset` to increase `count` by one.
        //
        // Note that we need `try_write8`, since `offset` can't be checked at compile-time.
        bar.try_write8(data, offset)?;

        Ok(bar.read32(Regs::COUNT))
    }

    fn config_space(pdev: &pci::Device<Bound>) {
        let config = pdev.config_space();

        // TODO: use the register!() macro for defining PCI configuration space registers once it
        // has been move out of nova-core.
        dev_info!(
            pdev.as_ref(),
            "pci-testdev config space read8 rev ID: {:x}\n",
            config.read8(0x8)
        );

        dev_info!(
            pdev.as_ref(),
            "pci-testdev config space read16 vendor ID: {:x}\n",
            config.read16(0)
        );

        dev_info!(
            pdev.as_ref(),
            "pci-testdev config space read32 BAR 0: {:x}\n",
            config.read32(0x10)
        );

        for (name, id) in [
            ("PM", bindings::PCI_CAP_ID_PM as u8),
            ("MSI", bindings::PCI_CAP_ID_MSI as u8),
            ("PCIe", bindings::PCI_CAP_ID_EXP as u8),
        ] {
            if let Some(pos) = pdev.find_capability(id) {
                dev_info!(pdev.as_ref(), "pci-testdev {name} cap @ 0x{:02x}\n", pos);
            } else {
                dev_info!(pdev.as_ref(), "pci-testdev has no {name} capability\n");
            }
        }

        // Best-effort self-check to exercise the `Some(offset)` path:
        // If the device advertises a standard capability list, read the first capability ID
        // directly from config space and verify that `find_capability()` returns an offset.
        let status = config.read16(bindings::PCI_STATUS as usize);
        if (status & bindings::PCI_STATUS_CAP_LIST as u16) != 0 {
            let pos = config.read8(bindings::PCI_CAPABILITY_LIST as usize);
            if pos != 0 {
                let id = config.read8(pos as usize + bindings::PCI_CAP_LIST_ID as usize);
                match pdev.find_capability(id) {
                    Some(found) => dev_info!(
                        pdev.as_ref(),
                        "pci-testdev selfcheck: cap id 0x{:02x} @ 0x{:02x} (find -> 0x{:02x})\n",
                        id,
                        pos,
                        found
                    ),
                    None => dev_info!(
                        pdev.as_ref(),
                        "pci-testdev selfcheck: cap id 0x{:02x} @ 0x{:02x} (find -> none)\n",
                        id,
                        pos
                    ),
                }
            } else {
                dev_info!(pdev.as_ref(), "pci-testdev selfcheck: empty cap list\n");
            }
        } else {
            dev_info!(pdev.as_ref(), "pci-testdev selfcheck: no cap list\n");
        }

        for (name, id) in [
            ("DSN", bindings::PCI_EXT_CAP_ID_DSN as u16),
            ("SR-IOV", bindings::PCI_EXT_CAP_ID_SRIOV as u16),
        ] {
            if let Some(pos) = pdev.find_ext_capability(id) {
                dev_info!(
                    pdev.as_ref(),
                    "pci-testdev {name} ext cap @ 0x{:04x}\n",
                    pos
                );
            } else {
                dev_info!(pdev.as_ref(), "pci-testdev has no {name} ext capability\n");
            }
        }

        // Best-effort self-check for extended capabilities.
        //
        // If the device has PCIe extended configuration space, verify that
        // `find_ext_capability()` can find the ID from the first extended
        // capability header (which is located right after the 256-byte legacy
        // configuration space).
        if let Ok(config_ext) = pdev.config_space_extended() {
            let hdr = config_ext.read32(bindings::PCI_CFG_SPACE_SIZE as usize);
            if hdr != 0 && hdr != u32::MAX {
                let id = (hdr & 0xffff) as u16;
                match pdev.find_ext_capability(id) {
                    Some(found) => dev_info!(
                        pdev.as_ref(),
                        "pci-testdev selfcheck: ext cap id 0x{:04x} @ 0x{:04x} (find -> 0x{:04x})\n",
                        id,
                        bindings::PCI_CFG_SPACE_SIZE,
                        found
                    ),
                    None => dev_info!(
                        pdev.as_ref(),
                        "pci-testdev selfcheck: ext cap id 0x{:04x} @ 0x{:04x} (find -> none)\n",
                        id,
                        bindings::PCI_CFG_SPACE_SIZE
                    ),
                }
            } else {
                dev_info!(
                    pdev.as_ref(),
                    "pci-testdev selfcheck: no ext cap header @ 0x{:04x}\n",
                    bindings::PCI_CFG_SPACE_SIZE
                );
            }
        } else {
            dev_info!(
                pdev.as_ref(),
                "pci-testdev selfcheck: no ext config space\n"
            );
        }
    }
}

impl pci::Driver for SampleDriver {
    type IdInfo = TestIndex;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &pci::Device<Core>, info: &Self::IdInfo) -> impl PinInit<Self, Error> {
        pin_init::pin_init_scope(move || {
            let vendor = pdev.vendor_id();
            dev_dbg!(
                pdev,
                "Probe Rust PCI driver sample (PCI ID: {}, 0x{:x}).\n",
                vendor,
                pdev.device_id()
            );

            pdev.enable_device_mem()?;
            pdev.set_master();

            Ok(try_pin_init!(Self {
                bar <- pdev.iomap_region_sized::<{ Regs::END }>(0, c"rust_driver_pci"),
                index: *info,
                _: {
                    let bar = bar.access(pdev.as_ref())?;

                    dev_info!(
                        pdev,
                        "pci-testdev data-match count: {}\n",
                        Self::testdev(info, bar)?
                    );
                    Self::config_space(pdev);
                },
                pdev: pdev.into(),
            }))
        })
    }

    fn unbind(pdev: &pci::Device<Core>, this: Pin<&Self>) {
        if let Ok(bar) = this.bar.access(pdev.as_ref()) {
            // Reset pci-testdev by writing a new test index.
            bar.write8(this.index.0, Regs::TEST);
        }
    }
}

#[pinned_drop]
impl PinnedDrop for SampleDriver {
    fn drop(self: Pin<&mut Self>) {
        dev_dbg!(self.pdev, "Remove Rust PCI driver sample.\n");
    }
}

kernel::module_pci_driver! {
    type: SampleDriver,
    name: "rust_driver_pci",
    authors: ["Danilo Krummrich"],
    description: "Rust PCI driver",
    license: "GPL v2",
}
