// SPDX-License-Identifier: GPL-2.0

use kernel::{device, devres::Devres, error::code::*, fmt, pci, prelude::*, sync::Arc};

use crate::driver::Bar0;
use crate::falcon::{gsp::Gsp as GspFalcon, sec2::Sec2 as Sec2Falcon, Falcon};
use crate::fb::SysmemFlush;
use crate::gfw;
use crate::gsp::Gsp;
use crate::regs;

macro_rules! define_chipset {
    ({ $($variant:ident = $value:expr),* $(,)* }) =>
    {
        /// Enum representation of the GPU chipset.
        #[derive(fmt::Debug, Copy, Clone, PartialOrd, Ord, PartialEq, Eq)]
        pub(crate) enum Chipset {
            $($variant = $value),*,
        }

        impl Chipset {
            pub(crate) const ALL: &'static [Chipset] = &[
                $( Chipset::$variant, )*
            ];

            ::kernel::macros::paste!(
            /// Returns the name of this chipset, in lowercase.
            ///
            /// # Examples
            ///
            /// ```
            /// let chipset = Chipset::GA102;
            /// assert_eq!(chipset.name(), "ga102");
            /// ```
            pub(crate) const fn name(&self) -> &'static str {
                match *self {
                $(
                    Chipset::$variant => stringify!([<$variant:lower>]),
                )*
                }
            }
            );
        }

        // TODO[FPRI]: replace with something like derive(FromPrimitive)
        impl TryFrom<u32> for Chipset {
            type Error = kernel::error::Error;

            fn try_from(value: u32) -> Result<Self, Self::Error> {
                match value {
                    $( $value => Ok(Chipset::$variant), )*
                    _ => Err(ENODEV),
                }
            }
        }
    }
}

define_chipset!({
    // Turing
    TU102 = 0x162,
    TU104 = 0x164,
    TU106 = 0x166,
    TU117 = 0x167,
    TU116 = 0x168,
    // Ampere
    GA100 = 0x170,
    GA102 = 0x172,
    GA103 = 0x173,
    GA104 = 0x174,
    GA106 = 0x176,
    GA107 = 0x177,
    // Ada
    AD102 = 0x192,
    AD103 = 0x193,
    AD104 = 0x194,
    AD106 = 0x196,
    AD107 = 0x197,
});

impl Chipset {
    pub(crate) fn arch(&self) -> Architecture {
        match self {
            Self::TU102 | Self::TU104 | Self::TU106 | Self::TU117 | Self::TU116 => {
                Architecture::Turing
            }
            Self::GA100 | Self::GA102 | Self::GA103 | Self::GA104 | Self::GA106 | Self::GA107 => {
                Architecture::Ampere
            }
            Self::AD102 | Self::AD103 | Self::AD104 | Self::AD106 | Self::AD107 => {
                Architecture::Ada
            }
        }
    }
}

// TODO
//
// The resulting strings are used to generate firmware paths, hence the
// generated strings have to be stable.
//
// Hence, replace with something like strum_macros derive(Display).
//
// For now, redirect to fmt::Debug for convenience.
impl fmt::Display for Chipset {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self:?}")
    }
}

/// Enum representation of the GPU generation.
#[derive(fmt::Debug)]
pub(crate) enum Architecture {
    Turing = 0x16,
    Ampere = 0x17,
    Ada = 0x19,
}

impl TryFrom<u8> for Architecture {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self> {
        match value {
            0x16 => Ok(Self::Turing),
            0x17 => Ok(Self::Ampere),
            0x19 => Ok(Self::Ada),
            _ => Err(ENODEV),
        }
    }
}

/// Structure holding the resources required to operate the GPU.
#[pin_data]
pub(crate) struct Gpu {
    chipset: Chipset,
    /// MMIO mapping of PCI BAR 0
    bar: Arc<Devres<Bar0>>,
    /// System memory page required for flushing all pending GPU-side memory writes done through
    /// PCIE into system memory, via sysmembar (A GPU-initiated HW memory-barrier operation).
    sysmem_flush: SysmemFlush,
    /// GSP falcon instance, used for GSP boot up and cleanup.
    gsp_falcon: Falcon<GspFalcon>,
    /// SEC2 falcon instance, used for GSP boot up and cleanup.
    sec2_falcon: Falcon<Sec2Falcon>,
    /// GSP runtime data. Temporarily an empty placeholder.
    #[pin]
    gsp: Gsp,
}

impl Gpu {
    pub(crate) fn new<'a>(
        pdev: &'a pci::Device<device::Bound>,
        devres_bar: Arc<Devres<Bar0>>,
        bar: &'a Bar0,
    ) -> impl PinInit<Self, Error> + 'a {
        let boot0 = regs::NV_PMC_BOOT_0::read(bar);

        // "next-gen" GPUs (some time after Blackwell) will zero out boot0, and put the architecture
        // details in boot42 instead. Avoid reading boot42 unless we are in that case.
        let boot42 = if boot0.is_next_gen() {
            Some(regs::NV_PMC_BOOT_42::read(bar))
        } else {
            None
        };

        try_pin_init!(Self {
            chipset: {
                // Some brief notes about boot0 and boot42, in chronological order:
                //
                // NV04 through Volta:
                //
                //    Not supported by Nova. boot0 is necessary and sufficient to identify these
                //    GPUs. boot42 may not even exist on some of these GPUs.
                //
                // Turing through Blackwell:
                //
                //     Supported by both Nouveau and Nova. boot0 is still necessary and sufficient
                //     to identify these GPUs. boot42 exists on these GPUs but we don't need to use
                //     it.
                //
                // Future "next-gen" GPUs:
                //
                //    Only supported by Nova. Boot42 has the architecture details, boot0 is zeroed
                //    out.

                // NV04, the very first NVIDIA GPU to be supported on Linux, is identified by a
                // specific bit pattern in boot0. Although Nova does not support NV04 (see above),
                // it is possible to confuse NV04 with a "next-gen" GPU. Therefore, return early if
                // we specifically detect NV04, thus simplifying the remaining selection logic.
                if boot0.is_nv04() {
                    Err(ENODEV)?
                }

                // Now that we know it is something more recent than NV04, use boot42 if we
                // previously determined that boot42 was both valid and relevant, and boot0
                // otherwise.
                let (chipset, major_rev, minor_rev) = if let Some(boot42) = boot42 {
                    (
                        boot42.chipset()?,
                        boot42.major_revision(),
                        boot42.minor_revision(),
                    )
                } else {
                    // Current/older GPU: use BOOT0
                    (
                        boot0.chipset()?,
                        boot0.major_revision(),
                        boot0.minor_revision(),
                    )
                };

                dev_info!(
                    pdev.as_ref(),
                    "NVIDIA (Chipset: {}, Architecture: {:?}, Revision: {:x}.{:x})\n",
                    chipset,
                    chipset.arch(),
                    major_rev,
                    minor_rev
                );
                chipset
            },

            // We must wait for GFW_BOOT completion before doing any significant setup on the GPU.
            _: {
                gfw::wait_gfw_boot_completion(bar)
                    .inspect_err(|_| dev_err!(pdev.as_ref(), "GFW boot did not complete"))?;
            },

            sysmem_flush: SysmemFlush::register(pdev.as_ref(), bar, *chipset)?,

            gsp_falcon: Falcon::new(
                pdev.as_ref(),
                *chipset,
                bar,
                *chipset > Chipset::GA100,
            )
            .inspect(|falcon| falcon.clear_swgen0_intr(bar))?,

            sec2_falcon: Falcon::new(pdev.as_ref(), *chipset, bar, true)?,

            gsp <- Gsp::new(),

            _: {
                gsp.boot(pdev, bar, *chipset, gsp_falcon, sec2_falcon)?
            },

            bar: devres_bar,
        })
    }

    /// Called when the corresponding [`Device`](device::Device) is unbound.
    ///
    /// Note: This method must only be called from `Driver::unbind`.
    pub(crate) fn unbind(&self, dev: &device::Device<device::Core>) {
        kernel::warn_on!(self
            .bar
            .access(dev)
            .inspect(|bar| self.sysmem_flush.unregister(bar))
            .is_err());
    }
}
