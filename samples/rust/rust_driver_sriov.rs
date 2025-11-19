// SPDX-License-Identifier: GPL-2.0

//! Rust SR-IOV driver sample based on QEMU's 82576 ([igb]) emulation.
//!
//! To make this driver probe, QEMU must be run with `-device igb`.
//!
//! Further, enable [vIOMMU] with interrupt remapping using, e.g.,
//!
//! `-M q35,accel=kvm,kernel-irqchip=split -device intel-iommu,intremap=on,caching-mode=on`
//!
//! and append `intel_iommu=on` to the guest kernel arguments.
//!
//! [igb]: https://www.qemu.org/docs/master/system/devices/igb.html
//! [vIOMMU]: https://wiki.qemu.org/Features/VT-d

use kernel::{device::Core, pci, prelude::*, sync::aref::ARef};

#[pin_data(PinnedDrop)]
struct SampleDriver {
    pdev: ARef<pci::Device>,
}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <SampleDriver as pci::Driver>::IdInfo,
    [
        // E1000_DEV_ID_82576
        (pci::DeviceId::from_id(pci::Vendor::INTEL, 0x10c9), ()),
        // E1000_DEV_ID_82576_VF
        (pci::DeviceId::from_id(pci::Vendor::INTEL, 0x10ca), ())
    ]
);

#[vtable]
impl pci::Driver for SampleDriver {
    type IdInfo = ();

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &pci::Device<Core>, _info: &Self::IdInfo) -> impl PinInit<Self, Error> {
        pin_init::pin_init_scope(move || {
            dev_info!(
                pdev.as_ref(),
                "Probe Rust SR-IOV driver sample (PCI ID: {}, 0x{:x}).\n",
                pdev.vendor_id(),
                pdev.device_id()
            );

            if pdev.is_virtfn() {
                let physfn = pdev.physfn()?;
                assert!(physfn.is_physfn());
                dev_info!(
                    pdev.as_ref(),
                    "Parent device is PF (PCI ID: {}, 0x{:x}).\n",
                    physfn.vendor_id(),
                    physfn.device_id()
                );
            }

            pdev.enable_device_mem()?;
            pdev.set_master();

            Ok(try_pin_init!(Self { pdev: pdev.into() }))
        })
    }

    fn sriov_configure(pdev: &pci::Device<Core>, nr_virtfn: i32) -> Result<i32> {
        assert!(pdev.is_physfn());

        if nr_virtfn == 0 {
            dev_info!(
                pdev.as_ref(),
                "Disable SR-IOV (PCI ID: {}, 0x{:x}).\n",
                pdev.vendor_id(),
                pdev.device_id()
            );
            pdev.disable_sriov();
        } else {
            dev_info!(
                pdev.as_ref(),
                "Enable SR-IOV (PCI ID: {}, 0x{:x}).\n",
                pdev.vendor_id(),
                pdev.device_id()
            );
            pdev.enable_sriov(nr_virtfn)?;
        }

        assert_eq!(pdev.num_vf(), nr_virtfn);
        Ok(nr_virtfn)
    }
}

#[pinned_drop]
impl PinnedDrop for SampleDriver {
    fn drop(self: Pin<&mut Self>) {
        dev_info!(self.pdev.as_ref(), "Remove Rust SR-IOV driver sample.\n");
    }
}

kernel::module_pci_driver! {
    type: SampleDriver,
    name: "rust_driver_sriov",
    authors: ["Peter Colberg"],
    description: "Rust SR-IOV driver",
    license: "GPL v2",
}
