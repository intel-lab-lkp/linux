// SPDX-License-Identifier: GPL-2.0

//! Main driver module.

use kernel::{
    auxiliary,
    device::Core,
    dma::Device,
    dma::DmaMask,
    pci,
    pci::{
        Class,
        ClassMask,
        Vendor, //
    },
    prelude::*,
    sizes::SZ_16M,
    sync::atomic::{
        Atomic,
        Relaxed, //
    },
    types::ForLt,
};

use crate::gpu::{
    Chipset,
    Gpu, //
};

/// Counter for generating unique auxiliary device IDs.
static AUXILIARY_ID_COUNTER: Atomic<u32> = Atomic::new(0);

/// Data passed to the auxiliary device registration, for the sibling driver to use.
pub struct AuxData<'bound> {
    gpu: &'bound Gpu<'bound>,
}

impl AuxData<'_> {
    /// Returns the chipset of this GPU.
    pub fn chipset(&self) -> Chipset {
        self.gpu.spec.chipset
    }
}

/// Driver-associated data.
#[pin_data]
pub struct NovaCore<'bound> {
    // Fields are dropped in declaration order: unregister the auxiliary device before dropping
    // `gpu`, and drop `gpu` before `bar` because `AuxData` borrows `gpu` and `Gpu` borrows `bar`.
    #[allow(clippy::type_complexity)]
    _reg: auxiliary::Registration<'bound, ForLt!(AuxData<'_>)>,
    #[pin]
    pub(crate) gpu: Gpu<'bound>,
    bar: pci::Bar<'bound, BAR0_SIZE>,
}

pub(crate) struct NovaCoreDriver;

const BAR0_SIZE: usize = SZ_16M;

// For now we only support Ampere which can use up to 47-bit DMA addresses.
//
// TODO: Add an abstraction for this to support newer GPUs which may support
// larger DMA addresses. Limiting these GPUs to smaller address widths won't
// have any adverse affects, unless installed on systems which require larger
// DMA addresses. These systems should be quite rare.
const GPU_DMA_BITS: u32 = 47;

pub(crate) type Bar0 = kernel::io::Mmio<BAR0_SIZE>;

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <NovaCoreDriver as pci::Driver>::IdInfo,
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

impl pci::Driver for NovaCoreDriver {
    type IdInfo = ();
    type Data<'bound> = NovaCore<'bound>;
    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe<'bound>(
        pdev: &'bound pci::Device<Core<'_>>,
        _info: &'bound Self::IdInfo,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound {
        pin_init::pin_init_scope(move || {
            dev_dbg!(pdev, "Probe Nova Core GPU driver.\n");

            pdev.enable_device_mem()?;
            pdev.set_master();

            // SAFETY: No concurrent DMA allocations or mappings can be made because
            // the device is still being probed and therefore isn't being used by
            // other threads of execution.
            unsafe { pdev.dma_set_mask_and_coherent(DmaMask::new::<GPU_DMA_BITS>())? };

            Ok(try_pin_init!(&this in NovaCore {
                bar: pdev.iomap_region_sized::<BAR0_SIZE>(0, c"nova-core/bar0")?,
                // TODO: Use `&bar` self-referential pin-init syntax once available.
                //
                // SAFETY: `bar` is initialized before this expression is evaluated
                // (`try_pin_init!()` initializes fields in declaration order), lives at a pinned
                // stable address, and is dropped after `gpu` (struct field drop order).
                gpu <- Gpu::new(pdev, unsafe { &*core::ptr::from_ref(bar) }),
                // SAFETY: `NovaCore` is dropped when the device is unbound; i.e. `mem::forget()` is
                // never called on it.
                _reg: unsafe {
                    auxiliary::Registration::new_with_lt(
                        pdev.as_ref(),
                        c"nova-drm",
                        // TODO[XARR]: Use XArray or perhaps IDA for proper ID allocation/recycling.
                        // For now, use a simple atomic counter that never recycles IDs.
                        AUXILIARY_ID_COUNTER.fetch_add(1, Relaxed),
                        crate::MODULE_NAME,
                        AuxData {
                            // TODO: Use `&gpu` self-referential pin-init syntax once available.
                            //
                            // SAFETY: `this.gpu` is initialized before this expression is
                            // evaluated, lives at a pinned stable address, and is dropped after
                            // `_reg` (struct field drop order).
                            gpu: &*core::ptr::from_ref(&this.as_ref().gpu),
                        },
                    )?
                },
            }))
        })
    }
}
