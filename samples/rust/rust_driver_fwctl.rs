// SPDX-License-Identifier: GPL-2.0

//! Rust fwctl API test (based on QEMU's `pci-testdev`).
//!
//! To make this driver probe, QEMU must be run with `-device pci-testdev`.

use kernel::{
    bindings, device::Core, fwctl, fwctl::FwCtlOps, fwctl::FwCtlUCtx, pci, prelude::*,
    sync::aref::ARef,
};

struct FwCtlSampleUCtx {
    _drvdata: u32,
}

struct FwCtlSampleOps;

impl FwCtlOps for FwCtlSampleOps {
    type UCtx = FwCtlSampleUCtx;

    const DEVICE_TYPE: u32 = bindings::fwctl_device_type_FWCTL_DEVICE_TYPE_RUST_FWCTL_TEST as u32;

    fn open_uctx(uctx: &mut FwCtlUCtx<FwCtlSampleUCtx>) -> Result<(), Error> {
        let dev = uctx.get_parent_device();

        dev_info!(dev, "fwctl test driver: open_uctx().\n");
        Ok(())
    }

    fn close_uctx(uctx: &mut FwCtlUCtx<FwCtlSampleUCtx>) {
        let dev = uctx.get_parent_device();

        dev_info!(dev, "fwctl test driver: close_uctx().\n");
    }

    fn info(uctx: &mut FwCtlUCtx<FwCtlSampleUCtx>) -> Result<KVec<u8>, Error> {
        let dev = uctx.get_parent_device();

        dev_info!(dev, "fwctl test driver: info().\n");

        let mut infobuf = KVec::<u8>::new();
        infobuf.push(0xef, GFP_KERNEL)?;
        infobuf.push(0xbe, GFP_KERNEL)?;
        infobuf.push(0xad, GFP_KERNEL)?;
        infobuf.push(0xde, GFP_KERNEL)?;

        Ok(infobuf)
    }

    fn fw_rpc(
        uctx: &mut FwCtlUCtx<FwCtlSampleUCtx>,
        scope: u32,
        rpc_in: &mut [u8],
        _out_len: *mut usize,
    ) -> Result<Option<KVec<u8>>, Error> {
        let dev = uctx.get_parent_device();

        dev_info!(dev, "fwctl test driver: fw_rpc() scope {}.\n", scope);

        if rpc_in.len() != 4 {
            return Err(EINVAL);
        }

        dev_info!(
            dev,
            "fwctl test driver: inbuf len{} bytes[0-3] {:x} {:x} {:x} {:x}.\n",
            rpc_in.len(),
            rpc_in[0],
            rpc_in[1],
            rpc_in[2],
            rpc_in[3]
        );

        let mut outbuf = KVec::<u8>::new();
        outbuf.push(0xef, GFP_KERNEL)?;
        outbuf.push(0xbe, GFP_KERNEL)?;
        outbuf.push(0xad, GFP_KERNEL)?;
        outbuf.push(0xde, GFP_KERNEL)?;

        Ok(Some(outbuf))
    }
}

#[pin_data]
struct FwCtlSampleDriver {
    pdev: ARef<pci::Device>,
    #[pin]
    fwctl: fwctl::Registration<FwCtlSampleOps>,
}

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <FwCtlSampleDriver as pci::Driver>::IdInfo,
    [(pci::DeviceId::from_id(pci::Vendor::REDHAT, 0x5), ())]
);

impl pci::Driver for FwCtlSampleDriver {
    type IdInfo = ();
    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &pci::Device<Core>, _info: &Self::IdInfo) -> Result<Pin<KBox<Self>>> {
        dev_info!(pdev.as_ref(), "Probe fwctl test driver.\n");

        let drvdata = KBox::pin_init(
            try_pin_init!(Self {
                pdev: pdev.into(),
                fwctl <- fwctl::Registration::<FwCtlSampleOps>::new(pdev.as_ref())?,
            }),
            GFP_KERNEL,
        )?;

        Ok(drvdata)
    }
}

kernel::module_pci_driver! {
    type: FwCtlSampleDriver,
    name: "rust_driver_fwctl",
    authors: ["Zhi Wang"],
    description: "Rust fwctl test",
    license: "GPL v2",
}
