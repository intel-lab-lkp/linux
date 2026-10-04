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
//! Load `rust_driver_sriov_vf` with `modprobe` to load both the PF and VF drivers.
//! The VF module depends on the PF module through [`submit_request()`].
//!
//! [igb]: https://www.qemu.org/docs/master/system/devices/igb.html
//! [vIOMMU]: https://wiki.qemu.org/Features/VT-d

use kernel::{
    device::{
        Bound,
        Core, //
    },
    new_mutex,
    pci,
    prelude::*,
    sync::Mutex,
    types::CovariantForLt, //
};

struct SamplePfDriver;

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
        let request = requests.checked_add(1).ok_or(EOVERFLOW)?;
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

/// Submits a request to the PF associated with a bound VF.
// Keep this call out of line so the VF module has a symbol dependency on the PF module.
#[inline(never)]
pub fn submit_request(vf: &pci::Device<Bound>) -> Result<u64> {
    vf.vf_registration_data::<PfApiForLt>()?.submit(vf)
}

#[pin_data]
struct PfDriverData<'bound> {
    #[pin]
    registration: pci::VfRegistration<'bound, PfApiForLt>,
    pdev: &'bound pci::Device<Bound>,
}

impl<'bound> PfDriverData<'bound> {
    fn api(self: Pin<&Self>) -> Pin<&PfApi<'bound>> {
        // SAFETY: `registration` is structurally pinned.
        unsafe { self.map_unchecked(|data| &data.registration) }.data()
    }
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

#[vtable]
impl pci::Driver for SamplePfDriver {
    type IdInfo = ();
    type Data<'bound> = PfDriverData<'bound>;

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PF_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        try_pin_init!(PfDriverData {
            _: {
                dev_info!(
                    pdev,
                    "Probe Rust SR-IOV PF sample (PCI ID: {}, 0x{:x}).\n",
                    pdev.vendor_id(),
                    pdev.device_id()
                );
            },
            // SAFETY: The registration is embedded in the PF driver's private data.
            registration <- unsafe {
                pci::VfRegistration::new(
                    pdev,
                    try_pin_init!(PfApi {
                        pdev,
                        requests <- new_mutex!(0),
                    }),
                )
            },
            _: {
                pdev.enable_device_mem()?;
                pdev.set_master();
            },
            pdev,
        })
    }

    fn sriov_enable<'a>(
        dev: &'a pci::Device<Core<'_>>,
        data: Pin<&Self::Data<'a>>,
        token: pci::SriovEnable<'a>,
    ) -> Result<pci::SriovEnabled<'a>> {
        dev_info!(
            data.pdev,
            "Enable SR-IOV (PCI ID: {}, 0x{:x}).\n",
            data.pdev.vendor_id(),
            data.pdev.device_id()
        );

        let num_vfs = token.num_vfs();
        let enabled = token.enable(num_vfs)?;
        assert_eq!(dev.num_vf(), num_vfs);
        let api = data.api();
        dev_info!(dev, "PF handled {} VF requests.\n", *api.requests.lock());
        Ok(enabled)
    }

    fn sriov_disable<'a>(
        dev: &'a pci::Device<Core<'_>>,
        data: Pin<&Self::Data<'a>>,
        token: pci::SriovDisable<'a>,
    ) -> Result {
        dev_info!(
            data.pdev,
            "Disable SR-IOV (PCI ID: {}, 0x{:x}).\n",
            data.pdev.vendor_id(),
            data.pdev.device_id()
        );

        token.disable();
        assert_eq!(dev.num_vf(), 0);
        Ok(())
    }
}

kernel::module_pci_driver! {
    type: SamplePfDriver,
    name: "rust_driver_sriov_pf",
    authors: ["Peter Colberg"],
    description: "Rust SR-IOV PF driver",
    license: "GPL v2",
}
