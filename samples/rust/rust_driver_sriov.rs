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

use kernel::{
    device::{
        Bound,
        Core, //
    },
    driver,
    new_mutex,
    pci,
    prelude::*,
    sync::{
        aref::ARef,
        Mutex, //
    },
    types::CovariantForLt,
    InPlaceModule, //
};

const PF_DRIVER_NAME: &CStr = c"rust_driver_sriov_pf";
const VF_DRIVER_NAME: &CStr = c"rust_driver_sriov_vf";

struct SamplePfDriver;
struct SampleVfDriver;

#[pin_data]
struct PfApi<'bound> {
    pdev: &'bound pci::Device<Bound>,
    #[pin]
    requests: Mutex<u64>,
}

type PfApiForLt = CovariantForLt!(PfApi<'_>);

impl PfApi<'_> {
    fn submit(self: Pin<&Self>, vf: &pci::Device<Bound>) -> Result<u64> {
        let mut requests = self.requests.lock();
        let request = (*requests).checked_add(1).ok_or(EOVERFLOW)?;
        *requests = request;
        drop(requests);

        dev_info!(
            self.pdev,
            "Handle PF request {} from VF devfn {:#x}.\n",
            request,
            vf.dev_id()
        );

        Ok(request)
    }
}

#[pin_data(PinnedDrop)]
struct PfDriverData<'bound> {
    // Keep the device alive until the registration stops exposing `PfApi::pdev`.
    #[pin]
    _registration: pci::VfRegistration<'bound, PfApiForLt>,
    pdev: ARef<pci::Device>,
}

#[pin_data(PinnedDrop)]
struct VfDriverData {
    pdev: ARef<pci::Device>,
}

kernel::pci_device_table!(
    PF_TABLE,
    <SamplePfDriver as pci::Driver>::IdInfo,
    [(
        // E1000_DEV_ID_82576
        pci::DeviceId::from_id(pci::Vendor::INTEL, 0x10c9),
        ()
    )]
);

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
impl pci::Driver for SamplePfDriver {
    type IdInfo = ();
    type Data<'bound> = PfDriverData<'bound>;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PF_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
            dev_info!(
                pdev,
                "Probe Rust SR-IOV PF sample (PCI ID: {}, 0x{:x}).\n",
                pdev.vendor_id(),
                pdev.device_id()
            );

            pdev.enable_device_mem()?;
            pdev.set_master();

            Ok(try_pin_init!(PfDriverData {
                // SAFETY:
                // - probe has exclusive access to this PF before SR-IOV is enabled;
                // - the registration is pinned in the PF driver data and dropped before `pdev`;
                // - no other registration is created for this PF; and
                // - VFs are enabled only after probe by `sriov_configure`.
                _registration <- unsafe {
                    pci::VfRegistration::new(
                        pdev,
                        try_pin_init!(PfApi {
                            pdev,
                            requests <- new_mutex!(0),
                        }),
                    )
                },
                pdev: pdev.into(),
            }))
        })
    }

    fn sriov_configure<'bound>(
        dev: &'bound pci::Device<Core<'_>>,
        this: Pin<&Self::Data<'bound>>,
        nr_virtfn: i32,
    ) -> Result<i32> {
        if nr_virtfn == 0 {
            dev_info!(
                this.pdev,
                "Disable SR-IOV (PCI ID: {}, 0x{:x}).\n",
                this.pdev.vendor_id(),
                this.pdev.device_id()
            );
            dev.disable_sriov();
        } else {
            dev_info!(
                this.pdev,
                "Enable SR-IOV (PCI ID: {}, 0x{:x}).\n",
                this.pdev.vendor_id(),
                this.pdev.device_id()
            );
            dev.enable_sriov(nr_virtfn)?;
        }

        assert_eq!(dev.num_vf(), nr_virtfn);
        Ok(nr_virtfn)
    }
}

#[vtable]
impl pci::Driver for SampleVfDriver {
    type IdInfo = ();
    type Data<'bound> = VfDriverData;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &VF_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
            dev_info!(
                pdev,
                "Probe Rust SR-IOV VF sample (PCI ID: {}, 0x{:x}).\n",
                pdev.vendor_id(),
                pdev.device_id()
            );

            let pdev_bound: &'bound pci::Device<Bound> = pdev;
            let pf_api = pdev_bound.vf_registration_data::<PfApiForLt>()?;

            pdev.enable_device_mem()?;
            pdev.set_master();

            let request = pf_api.submit(pdev)?;
            dev_info!(pdev, "Submitted request {} through PF data.\n", request);

            Ok(try_pin_init!(VfDriverData { pdev: pdev.into() }))
        })
    }
}

#[pinned_drop]
impl PinnedDrop for PfDriverData<'_> {
    fn drop(self: Pin<&mut Self>) {
        dev_info!(self.pdev, "Remove Rust SR-IOV PF sample.\n");
    }
}

#[pinned_drop]
impl PinnedDrop for VfDriverData {
    fn drop(self: Pin<&mut Self>) {
        dev_info!(self.pdev, "Remove Rust SR-IOV VF sample.\n");
    }
}

#[pin_data]
struct SampleModule {
    // Keep the VF driver registered while PF removal tears down its VFs.
    #[pin]
    _pf: driver::Registration<pci::Adapter<SamplePfDriver>>,
    #[pin]
    _vf: driver::Registration<pci::Adapter<SampleVfDriver>>,
}

impl InPlaceModule for SampleModule {
    fn init(module: &'static ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            // The VF driver must be ready before the PF can enable VFs.
            _vf <- driver::Registration::new(VF_DRIVER_NAME, module),
            _pf <- driver::Registration::new(PF_DRIVER_NAME, module),
        })
    }
}

module! {
    type: SampleModule,
    name: "rust_driver_sriov",
    authors: ["Peter Colberg"],
    description: "Rust SR-IOV driver",
    license: "GPL v2",
}
