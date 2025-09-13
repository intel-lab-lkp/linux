// SPDX-License-Identifier: GPL-2.0

use core::ops::Range;

use kernel::prelude::*;
use kernel::sizes::*;
use kernel::sync::aref::ARef;
use kernel::{dev_warn, device};

use crate::dma::DmaObject;
use crate::driver::Bar0;
use crate::firmware::gsp::GspFirmware;
use crate::gpu::Chipset;
use crate::gsp;
use crate::regs;

mod hal;

/// Type holding the sysmem flush memory page, a page of memory to be written into the
/// `NV_PFB_NISO_FLUSH_SYSMEM_ADDR*` registers and used to maintain memory coherency.
///
/// A system memory page is required for `sysmembar`, which is a GPU-initiated hardware
/// memory-barrier operation that flushes all pending GPU-side memory writes that were done through
/// PCIE to system memory. It is required for falcons to be reset as the reset operation involves a
/// reset handshake. When the falcon acknowledges a reset, it writes into system memory. To ensure
/// this write is visible to the host and prevent driver timeouts, the falcon must perform a
/// sysmembar operation to flush its writes.
///
/// Because of this, the sysmem flush memory page must be registered as early as possible during
/// driver initialization, and before any falcon is reset.
///
/// Users are responsible for manually calling [`Self::unregister`] before dropping this object,
/// otherwise the GPU might still use it even after it has been freed.
pub(crate) struct SysmemFlush {
    /// Chipset we are operating on.
    chipset: Chipset,
    device: ARef<device::Device>,
    /// Keep the page alive as long as we need it.
    page: DmaObject,
}

impl SysmemFlush {
    /// Allocate a memory page and register it as the sysmem flush page.
    pub(crate) fn register(
        dev: &device::Device<device::Bound>,
        bar: &Bar0,
        chipset: Chipset,
    ) -> Result<Self> {
        let page = DmaObject::new(dev, kernel::page::PAGE_SIZE)?;

        hal::fb_hal(chipset).write_sysmem_flush_page(bar, page.dma_handle())?;

        Ok(Self {
            chipset,
            device: dev.into(),
            page,
        })
    }

    /// Unregister the managed sysmem flush page.
    ///
    /// In order to gracefully tear down the GPU, users must make sure to call this method before
    /// dropping the object.
    pub(crate) fn unregister(&self, bar: &Bar0) {
        let hal = hal::fb_hal(self.chipset);

        if hal.read_sysmem_flush_page(bar) == self.page.dma_handle() {
            let _ = hal.write_sysmem_flush_page(bar, 0).inspect_err(|e| {
                dev_warn!(
                    &self.device,
                    "failed to unregister sysmem flush page: {:?}",
                    e
                )
            });
        } else {
            // Another page has been registered after us for some reason - warn as this is a bug.
            dev_warn!(
                &self.device,
                "attempt to unregister a sysmem flush page that is not active\n"
            );
        }
    }
}

/// Layout of the GPU framebuffer memory.
///
/// Contains ranges of GPU memory reserved for a given purpose during the GSP boot process.
#[derive(Debug)]
#[expect(dead_code)]
pub(crate) struct FbLayout {
    /// Range of the framebuffer. Starts at `0`.
    pub(crate) fb: Range<u64>,
    /// VGA workspace, small area of reserved memory at the end of the framebuffer.
    pub(crate) vga_workspace: Range<u64>,
    /// FRTS range.
    pub(crate) frts: Range<u64>,
    /// Memory area containing the GSP bootloader image.
    pub(crate) boot: Range<u64>,
    /// Memory area containing the GSP firmware image.
    pub(crate) elf: Range<u64>,
    /// WPR2 heap.
    pub(crate) wpr2_heap: Range<u64>,
    // WPR2 region range, starting with an instance of `GspFwWprMeta`.
    pub(crate) wpr2: Range<u64>,
    pub(crate) heap: Range<u64>,
    pub(crate) vf_partition_count: u8,
}

impl FbLayout {
    /// Computes the FB layout for `chipset`, for running the `bl` GSP bootloader and `gsp` GSP
    /// firmware.
    pub(crate) fn new(chipset: Chipset, bar: &Bar0, gsp_fw: &GspFirmware) -> Result<Self> {
        let hal = hal::fb_hal(chipset);

        let fb = {
            let fb_size = hal.vidmem_size(bar);

            0..fb_size
        };

        let vga_workspace = {
            let vga_base = {
                const NV_PRAMIN_SIZE: u64 = SZ_1M as u64;
                let base = fb.end - NV_PRAMIN_SIZE;

                if hal.supports_display(bar) {
                    match regs::NV_PDISP_VGA_WORKSPACE_BASE::read(bar).vga_workspace_addr() {
                        Some(addr) => {
                            if addr < base {
                                const VBIOS_WORKSPACE_SIZE: u64 = SZ_128K as u64;

                                // Point workspace address to end of framebuffer.
                                fb.end - VBIOS_WORKSPACE_SIZE
                            } else {
                                addr
                            }
                        }
                        None => base,
                    }
                } else {
                    base
                }
            };

            vga_base..fb.end
        };

        let frts = {
            const FRTS_DOWN_ALIGN: u64 = SZ_128K as u64;
            const FRTS_SIZE: u64 = SZ_1M as u64;
            // TODO[NUMM]: replace with `align_down` once it lands.
            let frts_base = (vga_workspace.start & !(FRTS_DOWN_ALIGN - 1)) - FRTS_SIZE;

            frts_base..frts_base + FRTS_SIZE
        };

        let boot = {
            const BOOTLOADER_DOWN_ALIGN: Alignment = Alignment::new::<SZ_4K>();
            let bootloader_size = gsp_fw.bootloader.ucode.size() as u64;
            let bootloader_base = (frts.start - bootloader_size).align_down(BOOTLOADER_DOWN_ALIGN);

            bootloader_base..bootloader_base + bootloader_size
        };

        let elf = {
            const ELF_DOWN_ALIGN: Alignment = Alignment::new::<SZ_64K>();
            let elf_size = gsp_fw.size as u64;
            let elf_addr = (boot.start - elf_size).align_down(ELF_DOWN_ALIGN);

            elf_addr..elf_addr + elf_size
        };

        let wpr2_heap = {
            const WPR2_HEAP_DOWN_ALIGN: Alignment = Alignment::new::<SZ_1M>();
            let wpr2_heap_size =
                crate::gsp::LibosParams::from_chipset(chipset).wpr_heap_size(chipset, fb.end);
            let wpr2_heap_addr = (elf.start - wpr2_heap_size).align_down(WPR2_HEAP_DOWN_ALIGN);

            wpr2_heap_addr..(elf.start).align_down(WPR2_HEAP_DOWN_ALIGN)
        };

        let wpr2 = {
            const WPR2_DOWN_ALIGN: Alignment = Alignment::new::<SZ_1M>();
            let wpr2_addr = (wpr2_heap.start - size_of::<gsp::GspFwWprMeta>() as u64)
                .align_down(WPR2_DOWN_ALIGN);

            wpr2_addr..frts.end
        };

        let heap = {
            const HEAP_SIZE: u64 = SZ_1M as u64;

            wpr2.start - HEAP_SIZE..wpr2.start
        };

        Ok(Self {
            fb,
            vga_workspace,
            frts,
            boot,
            elf,
            wpr2_heap,
            wpr2,
            heap,
            vf_partition_count: 0,
        })
    }
}
