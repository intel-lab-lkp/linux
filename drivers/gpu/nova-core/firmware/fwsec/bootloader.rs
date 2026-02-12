// SPDX-License-Identifier: GPL-2.0

//! Bootloader support for the FWSEC firmware.
//!
//! On Turing, the FWSEC firmware is not loaded directly, but is instead loaded through a small
//! bootloader program that performs the required DMA operations. This bootloader itself needs to
//! be loaded using PIO.

use kernel::{
    alloc::KVec,
    device::{
        self,
        Device, //
    },
    prelude::*,
    sizes,
    transmute::{
        AsBytes,
        FromBytes, //
    },
};

use crate::{
    driver::Bar0,
    falcon::{
        gsp::Gsp,
        Falcon,
        FalconBromParams,
        FalconDmaLoadable,
        FalconEngine,
        FalconFbifMemType,
        FalconFbifTarget,
        FalconFirmware,
        FalconPioDmemLoadTarget,
        FalconPioImemLoadTarget,
        FalconPioLoadable, //
    },
    firmware::{
        fwsec::FwsecFirmware,
        request_firmware,
        BinHdr,
        FIRMWARE_VERSION, //
    },
    gpu::Chipset,
    num::{
        self,
        FromSafeCast, //
    },
    regs,
};

/// Descriptor used by RM to figure out the requirements of the boot loader.
#[repr(C)]
#[derive(Debug, Clone)]
pub(crate) struct BootloaderDesc {
    /// Starting tag of bootloader.
    pub(crate) start_tag: u32,
    /// DMEM offset where [`BootloaderDmemDescV2`] is to be loaded.
    pub(crate) dmem_load_off: u32,
    /// Offset of code section in the image.
    pub(crate) code_off: u32,
    /// Size of code section in the image.
    pub(crate) code_size: u32,
    /// Offset of data section in the image.
    pub(crate) data_off: u32,
    /// Size of data section in the image.
    pub(crate) data_size: u32,
}
// SAFETY: any byte sequence is valid for this struct.
unsafe impl FromBytes for BootloaderDesc {}

/// Structure used by the boot-loader to load the rest of the code.
///
/// This has to be filled by the GPU driver and copied into DMEM at offset
/// [`BootloaderDesc.dmem_load_off`].
#[repr(C, packed)]
#[derive(Debug, Clone)]
pub(crate) struct BootloaderDmemDescV2 {
    /// Reserved, should always be first element.
    pub(crate) reserved: [u32; 4],
    /// 16B signature for secure code, 0s if no secure code.
    pub(crate) signature: [u32; 4],
    /// DMA context used by the bootloader while loading code/data.
    pub(crate) ctx_dma: u32,
    /// 256B-aligned physical FB address where code is located.
    pub(crate) code_dma_base: u64,
    /// Offset from `code_dma_base` where the non-secure code is located (must be multiple of 256).
    pub(crate) non_sec_code_off: u32,
    /// Size of the non-secure code part.
    pub(crate) non_sec_code_size: u32,
    /// Offset from `code_dma_base` where the secure code is located (must be multiple of 256).
    pub(crate) sec_code_off: u32,
    /// Size of the secure code part.
    pub(crate) sec_code_size: u32,
    /// Code entry point invoked by the bootloader after code is loaded.
    pub(crate) code_entry_point: u32,
    /// 256B-aligned physical FB address where data is located.
    pub(crate) data_dma_base: u64,
    /// Size of data block (should be multiple of 256B).
    pub(crate) data_size: u32,
    /// Arguments to be passed to the target firmware being loaded.
    pub(crate) argc: u32,
    /// Number of arguments to be passed to the target firmware being loaded.
    pub(crate) argv: u32,
}
// SAFETY: This struct doesn't contain uninitialized bytes and doesn't have interior mutability.
unsafe impl AsBytes for BootloaderDmemDescV2 {}

/// Wrapper for [`FwsecFirmware`] that includes the bootloader performing the actual load
/// operation.
pub(crate) struct FwsecFirmwareWithBl {
    /// Firmware that the bootloader will load.
    firmware: FwsecFirmware,
    /// Descriptor to be loaded into DMEM for the bootloader to read.
    dmem_desc: BootloaderDmemDescV2,
    /// Code of the bootloader to be loaded into non-secure IMEM.
    ucode: KVec<u8>,
    /// Range-validated start offset of the firmware code in IMEM.
    imem_dst_start: u16,
    /// Range-validated `desc.start_tag`.
    start_tag: u16,
}

impl FwsecFirmwareWithBl {
    /// Loads the bootloader firmware for `dev` and `chipset`, and wrap `firmware` so it can be
    /// loaded using it.
    pub(crate) fn new(
        firmware: FwsecFirmware,
        dev: &Device<device::Bound>,
        chipset: Chipset,
    ) -> Result<Self> {
        let fw = request_firmware(dev, chipset, "gen_bootloader", FIRMWARE_VERSION)?;
        let hdr = fw
            .data()
            .get(0..size_of::<BinHdr>())
            .and_then(BinHdr::from_bytes_copy)
            .ok_or(EINVAL)?;

        let desc = {
            let desc_offset = usize::from_safe_cast(hdr.header_offset);

            fw.data()
                .get(desc_offset..)
                .and_then(BootloaderDesc::from_bytes_copy_prefix)
                .ok_or(EINVAL)?
                .0
        };

        let ucode = {
            let ucode_start = usize::from_safe_cast(hdr.data_offset);
            let code_size = usize::from_safe_cast(desc.code_size);

            let mut ucode = KVec::new();
            ucode.extend_from_slice(
                fw.data()
                    .get(ucode_start..ucode_start + code_size)
                    .ok_or(EINVAL)?,
                GFP_KERNEL,
            )?;

            ucode
        };

        let dmem_desc = {
            let imem_sec = FalconDmaLoadable::imem_sec_load_params(&firmware);
            let imem_ns = FalconDmaLoadable::imem_ns_load_params(&firmware).ok_or(EINVAL)?;
            let dmem = FalconDmaLoadable::dmem_load_params(&firmware);

            BootloaderDmemDescV2 {
                reserved: [0; 4],
                signature: [0; 4],
                ctx_dma: 4, // FALCON_DMAIDX_PHYS_SYS_NCOH
                code_dma_base: firmware.dma_handle(),
                non_sec_code_off: imem_ns.dst_start,
                non_sec_code_size: imem_ns.len,
                sec_code_off: imem_sec.dst_start,
                sec_code_size: imem_sec.len,
                code_entry_point: 0,
                data_dma_base: firmware.dma_handle() + u64::from(dmem.src_start),
                data_size: dmem.len,
                argc: 0,
                argv: 0,
            }
        };

        // The bootloader's code must be loaded in the area right below the first 64K of IMEM.
        const BOOTLOADER_LOAD_CEILING: u32 = num::usize_into_u32::<{ sizes::SZ_64K }>();
        let imem_dst_start = BOOTLOADER_LOAD_CEILING
            .checked_sub(desc.code_size)
            .ok_or(EOVERFLOW)?;

        Ok(Self {
            firmware,
            dmem_desc,
            ucode,
            imem_dst_start: u16::try_from(imem_dst_start)?,
            start_tag: u16::try_from(desc.start_tag)?,
        })
    }

    /// Loads the bootloader into `falcon` and execute it.
    ///
    /// The bootloader will load the FWSEC firmware and then execute it. This function returns
    /// after FWSEC has reached completion.
    pub(crate) fn run(
        &self,
        dev: &Device<device::Bound>,
        falcon: &Falcon<Gsp>,
        bar: &Bar0,
    ) -> Result<()> {
        // Reset falcon, load the firmware, and run it.
        falcon
            .reset(bar)
            .inspect_err(|e| dev_err!(dev, "Failed to reset GSP falcon: {:?}\n", e))?;
        falcon
            .pio_load(bar, self)
            .inspect_err(|e| dev_err!(dev, "Failed to load FWSEC firmware: {:?}\n", e))?;

        // Configure DMA index for the bootloader to fetch the FWSEC firmware from system memory.
        regs::NV_PFALCON_FBIF_TRANSCFG::update(
            bar,
            &Gsp::ID,
            usize::from_safe_cast(self.dmem_desc.ctx_dma),
            |v| {
                v.set_target(FalconFbifTarget::CoherentSysmem)
                    .set_mem_type(FalconFbifMemType::Physical)
            },
        );

        let (mbox0, _) = falcon
            .boot(bar, Some(0), None)
            .inspect_err(|e| dev_err!(dev, "Failed to boot FWSEC firmware: {:?}\n", e))?;
        if mbox0 != 0 {
            dev_err!(dev, "FWSEC firmware returned error {}\n", mbox0);
            Err(EIO)
        } else {
            Ok(())
        }
    }
}

impl FalconFirmware for FwsecFirmwareWithBl {
    type Target = Gsp;

    fn brom_params(&self) -> FalconBromParams {
        self.firmware.brom_params()
    }

    fn boot_addr(&self) -> u32 {
        // On V2 platforms, the boot address is extracted from the generic bootloader, because the
        // gbl is what actually copies FWSEC into memory, so that is what needs to be booted.
        u32::from(self.start_tag) << 8
    }
}

impl FalconPioLoadable for FwsecFirmwareWithBl {
    fn imem_sec_load_params(&self) -> Option<FalconPioImemLoadTarget<'_>> {
        None
    }

    fn imem_ns_load_params(&self) -> Option<FalconPioImemLoadTarget<'_>> {
        Some(FalconPioImemLoadTarget {
            data: self.ucode.as_ref(),
            dst_start: self.imem_dst_start,
            secure: false,
            start_tag: self.start_tag,
        })
    }

    fn dmem_load_params(&self) -> FalconPioDmemLoadTarget<'_> {
        FalconPioDmemLoadTarget {
            data: self.dmem_desc.as_bytes(),
            dst_start: 0,
        }
    }
}
