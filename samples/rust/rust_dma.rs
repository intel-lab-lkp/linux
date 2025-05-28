// SPDX-License-Identifier: GPL-2.0

//! Rust DMA api test (based on QEMU's `pci-testdev`).
//!
//! To make this driver probe, QEMU must be run with `-device pci-testdev`.

use kernel::{
    bindings, device::Core, dma::CoherentAllocation, page::*, pci, prelude::*, scatterlist::*,
    types::ARef,
};

struct DmaSampleDriver {
    pdev: ARef<pci::Device>,
    ca: CoherentAllocation<MyStruct>,
    _sgt: DeviceSGTable,
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

    fn probe(pdev: &pci::Device<Core>, _info: &Self::IdInfo) -> Result<Pin<KBox<Self>>> {
        dev_info!(pdev.as_ref(), "Probe DMA test driver.\n");

        let ca: CoherentAllocation<MyStruct> =
            CoherentAllocation::alloc_coherent(pdev.as_ref(), TEST_VALUES.len(), GFP_KERNEL)?;

        || -> Result {
            for (i, value) in TEST_VALUES.into_iter().enumerate() {
                kernel::dma_write!(ca[i] = MyStruct::new(value.0, value.1));
            }

            Ok(())
        }()?;

        let mut len = 0;
        let sgt = SGTable::alloc_table(TEST_VALUES.len(), GFP_KERNEL)?;
        let sgt = sgt.init(|iter| {
            for sg in iter {
                sg.set_page(&Page::alloc_page(GFP_KERNEL)?, PAGE_SIZE as u32, 0);
                len += 1;
            }
            Ok(())
        })?;
        assert_eq!(len, TEST_VALUES.len());
        let sgt = sgt.dma_map(pdev.as_ref(), kernel::dma::DmaDataDirection::DmaToDevice)?;

        let drvdata = KBox::new(
            Self {
                pdev: pdev.into(),
                ca,
                // Save object to excercise the destructor.
                _sgt: sgt,
            },
            GFP_KERNEL,
        )?;

        Ok(drvdata.into())
    }
}

impl Drop for DmaSampleDriver {
    fn drop(&mut self) {
        dev_info!(self.pdev.as_ref(), "Unload DMA test driver.\n");
        assert_eq!(self._sgt.iter().count(), TEST_VALUES.len());

        let _ = || -> Result {
            for (i, value) in TEST_VALUES.into_iter().enumerate() {
                assert_eq!(kernel::dma_read!(self.ca[i].h), value.0);
                assert_eq!(kernel::dma_read!(self.ca[i].b), value.1);
            }
            Ok(())
        }();
    }
}

kernel::module_pci_driver! {
    type: DmaSampleDriver,
    name: "rust_dma",
    authors: ["Abdiel Janulgue"],
    description: "Rust DMA test",
    license: "GPL v2",
}
