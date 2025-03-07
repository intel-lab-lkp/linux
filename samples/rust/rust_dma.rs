// SPDX-License-Identifier: GPL-2.0

//! Rust DMA api test (based on QEMU's `pci-testdev`).
//!
//! To make this driver probe, QEMU must be run with `-device pci-testdev`.

use kernel::{bindings, dma::CoherentAllocation, pci, prelude::*};

struct DmaSampleDriver {
    pdev: pci::Device,
    ca: kernel::devres::Devres<CoherentAllocation<MyStruct>>,
}

const TEST_VALUES: [(u32, u32); 5] = [
    (0xa, 0xb),
    (0xc, 0xd),
    (0xe, 0xf),
    (0xab, 0xba),
    (0xcd, 0xef),
];

struct MyStruct {
    h: u32,
    b: u32,
}

impl MyStruct {
    fn new(h: u32, b: u32) -> Self {
        Self { h, b }
    }
}
// SAFETY: All bit patterns are acceptable values for `MyStruct`.
unsafe impl kernel::transmute::AsBytes for MyStruct {}
// SAFETY: Instances of `MyStruct` have no uninitialized portions.
unsafe impl kernel::transmute::FromBytes for MyStruct {}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <DmaSampleDriver as pci::Driver>::IdInfo,
    [(
        pci::DeviceId::from_id(bindings::PCI_VENDOR_ID_REDHAT, 0x5),
        ()
    )]
);

impl pci::Driver for DmaSampleDriver {
    type IdInfo = ();
    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &mut pci::Device, _info: &Self::IdInfo) -> Result<Pin<KBox<Self>>> {
        let dev = pdev.as_mut();

        dev.dma_set_mask_and_coherent(kernel::dma::dma_bit_mask(64))?;

        dev_info!(dev, "Probe DMA test driver.\n");

        let ca: kernel::devres::Devres<CoherentAllocation<MyStruct>> =
            CoherentAllocation::alloc_coherent(dev, TEST_VALUES.len(), GFP_KERNEL)?;

        || -> Result {
            let reg = ca.try_access().ok_or(ENXIO)?;

            for (i, value) in TEST_VALUES.into_iter().enumerate() {
                kernel::dma_write!(reg[i] = MyStruct::new(value.0, value.1));
            }

            Ok(())
        }()?;

        let drvdata = KBox::new(
            Self {
                pdev: pdev.clone(),
                ca,
            },
            GFP_KERNEL,
        )?;

        Ok(drvdata.into())
    }
}

impl Drop for DmaSampleDriver {
    fn drop(&mut self) {
        dev_info!(self.pdev.as_ref(), "Unload DMA test driver.\n");

        let _ = || -> Result {
            let reg = self.ca.try_access().ok_or(ENXIO)?;
            for (i, value) in TEST_VALUES.into_iter().enumerate() {
                assert_eq!(kernel::dma_read!(reg[i].h), value.0);
                assert_eq!(kernel::dma_read!(reg[i].b), value.1);
            }
            Ok(())
        }();
    }
}

kernel::module_pci_driver! {
    type: DmaSampleDriver,
    name: "rust_dma",
    author: "Abdiel Janulgue",
    description: "Rust DMA test",
    license: "GPL v2",
}
