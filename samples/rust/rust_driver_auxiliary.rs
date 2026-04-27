// SPDX-License-Identifier: GPL-2.0

//! Rust auxiliary driver sample (based on a PCI driver for QEMU's `pci-testdev`).
//!
//! To make this driver probe, QEMU must be run with `-device pci-testdev`.

use kernel::{
    auxiliary,
    device::{
        Bound,
        Core, //
    },
    devres::Devres,
    driver,
    pci,
    prelude::*,
    types::ForLt,
    InPlaceModule, //
};

const MODULE_NAME: &CStr = <LocalModule as kernel::ModuleMetadata>::NAME;
const AUXILIARY_NAME: &CStr = c"auxiliary";

struct AuxiliaryDriver;

kernel::auxiliary_device_table!(
    AUX_TABLE,
    MODULE_AUX_TABLE,
    <AuxiliaryDriver as auxiliary::Driver<'_>>::IdInfo,
    [(auxiliary::DeviceId::new(MODULE_NAME, AUXILIARY_NAME), ())]
);

impl<'a> auxiliary::Driver<'a> for AuxiliaryDriver {
    type IdInfo = ();

    const ID_TABLE: auxiliary::IdTable<Self::IdInfo> = &AUX_TABLE;

    fn probe(
        adev: &'a auxiliary::Device<Core>,
        _info: &'a Self::IdInfo,
    ) -> impl PinInit<Self, Error> + 'a {
        dev_info!(
            adev,
            "Probing auxiliary driver for auxiliary device with id={}\n",
            adev.id()
        );

        ParentDriver::connect(adev)?;

        Ok(Self)
    }
}

struct Data<'a> {
    index: u32,
    parent: &'a pci::Device<Bound>,
}

#[allow(clippy::type_complexity)]
struct ParentDriver {
    _reg0: Devres<auxiliary::Registration<ForLt!(Data<'_>)>>,
    _reg1: Devres<auxiliary::Registration<ForLt!(Data<'_>)>>,
}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <ParentDriver as pci::Driver<'_>>::IdInfo,
    [(pci::DeviceId::from_id(pci::Vendor::REDHAT, 0x5), ())]
);

impl<'a> pci::Driver<'a> for ParentDriver {
    type IdInfo = ();

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(
        pdev: &'a pci::Device<Core>,
        _info: &'a Self::IdInfo,
    ) -> impl PinInit<Self, Error> + 'a {
        Ok(Self {
            _reg0: auxiliary::Registration::new(
                pdev.as_ref(),
                AUXILIARY_NAME,
                0,
                MODULE_NAME,
                Data {
                    index: 0,
                    parent: pdev,
                },
            )?,
            _reg1: auxiliary::Registration::new(
                pdev.as_ref(),
                AUXILIARY_NAME,
                1,
                MODULE_NAME,
                Data {
                    index: 1,
                    parent: pdev,
                },
            )?,
        })
    }
}

impl ParentDriver {
    fn connect(adev: &auxiliary::Device<Bound>) -> Result {
        let data = adev.registration_data::<ForLt!(Data<'_>)>()?;
        let pdev = data.parent;

        dev_info!(
            pdev,
            "Connect auxiliary {} with parent: VendorID={}, DeviceID={:#x}\n",
            adev.id(),
            pdev.vendor_id(),
            pdev.device_id()
        );

        dev_info!(
            pdev,
            "Connected to auxiliary device with index {}.\n",
            data.index
        );

        Ok(())
    }
}

#[pin_data]
struct SampleModule {
    #[pin]
    #[allow(clippy::type_complexity)]
    _pci_driver: driver::Registration<pci::Adapter<ForLt!(ParentDriver)>>,
    #[pin]
    #[allow(clippy::type_complexity)]
    _aux_driver: driver::Registration<auxiliary::Adapter<ForLt!(AuxiliaryDriver)>>,
}

impl InPlaceModule for SampleModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _pci_driver <- driver::Registration::new(MODULE_NAME, module),
            _aux_driver <- driver::Registration::new(MODULE_NAME, module),
        })
    }
}

module! {
    type: SampleModule,
    name: "rust_driver_auxiliary",
    authors: ["Danilo Krummrich"],
    description: "Rust auxiliary driver",
    license: "GPL v2",
}
