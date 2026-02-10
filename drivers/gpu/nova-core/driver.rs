// SPDX-License-Identifier: GPL-2.0

use core::sync::atomic::{
    AtomicU32,
    Ordering, //
};

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
    sync::Arc, //
};

use crate::gpu::{
    Gpu,
    Spec, //
};

/// Counter for generating unique auxiliary device IDs.
static AUXILIARY_ID_COUNTER: AtomicU32 = AtomicU32::new(0);

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

            let devres_bar = Arc::pin_init(
                pdev.iomap_region_sized::<BAR0_SIZE>(0, c"nova-core/bar0"),
                GFP_KERNEL,
            )?;

            // Read the GPU spec early to determine the correct DMA address width.
            // Hopper/Blackwell+ support 52-bit DMA addresses, earlier architectures use 47-bit.
            let spec = Spec::new(pdev.as_ref(), devres_bar.access(pdev.as_ref())?)?;
            dev_info!(pdev.as_ref(), "NVIDIA ({})\n", spec);

            // SAFETY: No concurrent DMA allocations or mappings can be made because
            // the device is still being probed and therefore isn't being used by
            // other threads of execution.
            unsafe { pdev.dma_set_mask_and_coherent(spec.chipset().arch().dma_mask())? };

            // TODO[XARR]: Use XArray for proper ID allocation/recycling. Until then, use a simple
            // atomic counter which never recycles IDs. A unique ID is required for multi-GPU
            // systems, because without it, probe() would fail for all but the first GPU.
            let aux_id = AUXILIARY_ID_COUNTER.fetch_add(1, Ordering::Relaxed);

            Ok(try_pin_init!(Self {
                gpu <- Gpu::new(pdev, devres_bar.clone(), devres_bar.access(pdev.as_ref())?, spec),
                _reg <- auxiliary::Registration::new(
                    pdev.as_ref(),
                    c"nova-drm",
                    aux_id,
                    crate::MODULE_NAME
                ),
            }))
        })
    }

    fn unbind(pdev: &pci::Device<Core>, this: Pin<&Self>) {
        this.gpu.unbind(pdev.as_ref());
    }
}
