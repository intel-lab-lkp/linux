// SPDX-License-Identifier: GPL-2.0

//! Virtual Function driver for the Rust SR-IOV sample.

use kernel::{
    device::{
        Bound,
        Core, //
    },
    pci,
    prelude::*, //
};

struct SampleVfDriver;

#[pin_data]
struct VfDriverData<'bound> {
    pdev: &'bound pci::Device<Bound>,
}

kernel::pci_device_table!(
    VF_TABLE,
    <SampleVfDriver as pci::Driver>::IdInfo,
    [(
        // E1000_DEV_ID_82576_VF
        pci::DeviceId::from_id(pci::Vendor::INTEL, 0x10ca),
        ()
    )]
);

#[vtable]
impl pci::Driver for SampleVfDriver {
    type IdInfo = ();
    type Data<'bound> = VfDriverData<'bound>;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &VF_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        try_pin_init!(VfDriverData {
            _: {
                dev_info!(
                    pdev,
                    "Probe Rust SR-IOV VF sample (PCI ID: {}, 0x{:x}).\n",
                    pdev.vendor_id(),
                    pdev.device_id()
                );
            },
            _: {
                let request = rust_driver_sriov_pf::submit_request(pdev)?;
                dev_info!(pdev, "Submitted request {} through PF data.\n", request);
            },
            _: {
                pdev.enable_device_mem()?;
                pdev.set_master();
            },
            pdev,
        })
    }
}

kernel::module_pci_driver! {
    type: SampleVfDriver,
    name: "rust_driver_sriov_vf",
    authors: ["Peter Colberg"],
    description: "Rust SR-IOV VF driver",
    license: "GPL v2",
}
