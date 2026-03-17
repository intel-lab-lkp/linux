// SPDX-License-Identifier: GPL-2.0

use kernel::{
    auxiliary,
    device::Core,
    devres::Devres,
    dma::Device,
    pci,
    pci::{
        Class,
        ClassMask,
        Vendor, //
    },
    prelude::*,
    sizes::SZ_16M,
    sync::{
        atomic::{
            Atomic,
            Relaxed, //
        },
        Arc,
    },
};

use crate::gpu::{
    Gpu,
    Spec, //
};

/// Counter for generating unique auxiliary device IDs.
static AUXILIARY_ID_COUNTER: Atomic<u32> = Atomic::new(0);

#[pin_data]
pub(crate) struct NovaCore {
    #[pin]
    pub(crate) gpu: Gpu,
    #[pin]
    _reg: Devres<auxiliary::Registration>,
}

const BAR0_SIZE: usize = SZ_16M;

pub(crate) type Bar0 = pci::Bar<BAR0_SIZE>;

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <NovaCore as pci::Driver>::IdInfo,
    [
        // Modern NVIDIA GPUs will show up as either VGA or 3D controllers.
        (
            pci::DeviceId::from_class_and_vendor(
                Class::DISPLAY_VGA,
                ClassMask::ClassSubclass,
                Vendor::NVIDIA
            ),
            ()
        ),
        (
            pci::DeviceId::from_class_and_vendor(
                Class::DISPLAY_3D,
                ClassMask::ClassSubclass,
                Vendor::NVIDIA
            ),
            ()
        ),
    ]
);

impl pci::Driver for NovaCore {
    type IdInfo = ();
    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &pci::Device<Core>, _info: &Self::IdInfo) -> impl PinInit<Self, Error> {
        pin_init::pin_init_scope(move || {
            dev_dbg!(pdev, "Probe Nova Core GPU driver.\n");

            pdev.enable_device_mem()?;
            pdev.set_master();

            let bar = Arc::pin_init(
                pdev.iomap_region_sized::<BAR0_SIZE>(0, c"nova-core/bar0"),
                GFP_KERNEL,
            )?;
            let spec = Spec::new(pdev.as_ref(), bar.access(pdev.as_ref())?)?;
            dev_info!(pdev, "NVIDIA ({})\n", spec);

            // SAFETY: No concurrent DMA allocations or mappings can be made because
            // the device is still being probed and therefore isn't being used by
            // other threads of execution.
            unsafe { pdev.dma_set_mask_and_coherent(spec.chipset().arch().dma_mask())? };

            Ok(try_pin_init!(Self {
                gpu <- Gpu::new(pdev, bar.clone(), bar.access(pdev.as_ref())?, spec),
                _reg <- auxiliary::Registration::new(
                    pdev.as_ref(),
                    c"nova-drm",
                    // TODO[XARR]: Use XArray or perhaps IDA for proper ID allocation/recycling. For
                    // now, use a simple atomic counter that never recycles IDs.
                    AUXILIARY_ID_COUNTER.fetch_add(1, Relaxed),
                    crate::MODULE_NAME
                ),
            }))
        })
    }

    fn unbind(pdev: &pci::Device<Core>, this: Pin<&Self>) {
        this.gpu.unbind(pdev.as_ref());
    }
}
