// SPDX-License-Identifier: GPL-2.0

//! FSP (Firmware System Processor) interface for Hopper/Blackwell GPUs.
//!
//! Hopper/Blackwell use a simplified firmware boot sequence: FMC --> FSP --> GSP.
//! Unlike Turing/Ampere/Ada, there is NO SEC2 (Security Engine 2) usage.
//! FSP handles secure boot directly using FMC firmware + Chain of Trust.

use kernel::{
    device,
    dma::CoherentAllocation,
    io::poll::read_poll_timeout,
    prelude::*,
    ptr::{
        Alignable,
        Alignment, //
    },
    sizes::{SZ_1M, SZ_2M},
    time::Delta,
    transmute::{
        AsBytes,
        FromBytes, //
    },
};

use crate::regs::FSP_BOOT_COMPLETE_SUCCESS;

/// FSP message timeout in milliseconds.
const FSP_MSG_TIMEOUT_MS: i64 = 2000;

/// FSP secure boot completion timeout in milliseconds.
const FSP_SECURE_BOOT_TIMEOUT_MS: i64 = 4000;

/// MCTP (Management Component Transport Protocol) header values for FSP communication.
pub(crate) mod mctp {
    pub(super) const HEADER_SOM: u32 = 1; // Start of Message
    pub(super) const HEADER_EOM: u32 = 1; // End of Message
    pub(super) const HEADER_SEID: u32 = 0; // Source Endpoint ID
    pub(super) const HEADER_SEQ: u32 = 0; // Sequence number

    // MCTP header bit positions
    pub(super) const HEADER_SOM_SHIFT: u32 = 31;
    pub(super) const HEADER_EOM_SHIFT: u32 = 30;
    pub(super) const HEADER_SEQ_SHIFT: u32 = 28;
    pub(super) const HEADER_SEID_SHIFT: u32 = 16;

    // NVDM header bit positions
    pub(super) const NVDM_VENDOR_ID_SHIFT: u32 = 8;
    pub(super) const NVDM_TYPE_SHIFT: u32 = 24;

    pub(super) const MSG_TYPE_VENDOR_PCI: u32 = 0x7e;
    pub(super) const VENDOR_ID_NV: u32 = 0x10de;
    pub(super) const NVDM_TYPE_COT: u32 = 0x14;
    pub(super) const NVDM_TYPE_FSP_RESPONSE: u32 = 0x15;
}

/// GSP FMC boot parameters structure.
/// This is what FSP expects to receive for booting GSP-RM.
/// GSP FMC initialization parameters.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
struct GspFmcInitParams {
    /// CC initialization "registry keys"
    regkeys: u32,
}

// SAFETY: GspFmcInitParams is a simple C struct with only primitive types.
unsafe impl AsBytes for GspFmcInitParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspFmcInitParams {}

/// GSP ACR (Authenticated Code RAM) boot parameters.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
struct GspAcrBootGspRmParams {
    /// Physical memory aperture through which gspRmDescPa is accessed
    target: u32,
    /// Size in bytes of the GSP-RM descriptor structure
    gsp_rm_desc_size: u32,
    /// Physical offset in the target aperture of the GSP-RM descriptor structure
    gsp_rm_desc_offset: u64,
    /// Physical offset in FB to set the start of the WPR containing GSP-RM
    wpr_carveout_offset: u64,
    /// Size in bytes of the WPR containing GSP-RM
    wpr_carveout_size: u32,
    /// Whether to boot GSP-RM or GSP-Proxy through ACR
    b_is_gsp_rm_boot: u32,
}

// SAFETY: GspAcrBootGspRmParams is a simple C struct with only primitive types.
unsafe impl AsBytes for GspAcrBootGspRmParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspAcrBootGspRmParams {}

/// GSP RM boot parameters.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
struct GspRmParams {
    /// Physical memory aperture through which bootArgsOffset is accessed
    target: u32,
    /// Physical offset in the memory aperture that will be passed to GSP-RM
    boot_args_offset: u64,
}

// SAFETY: GspRmParams is a simple C struct with only primitive types.
unsafe impl AsBytes for GspRmParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspRmParams {}

/// GSP SPDM (Security Protocol and Data Model) parameters.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
struct GspSpdmParams {
    /// Physical Memory Aperture through which all addresses are accessed
    target: u32,
    /// Physical offset in the memory aperture where SPDM payload buffer is stored
    payload_buffer_offset: u64,
    /// Size of the above payload buffer
    payload_buffer_size: u32,
}

// SAFETY: GspSpdmParams is a simple C struct with only primitive types.
unsafe impl AsBytes for GspSpdmParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspSpdmParams {}

/// Complete GSP FMC boot parameters structure.
/// This is what FSP expects to receive - NOT a raw libos address!
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub(crate) struct GspFmcBootParams {
    init_params: GspFmcInitParams,
    boot_gsp_rm_params: GspAcrBootGspRmParams,
    gsp_rm_params: GspRmParams,
    gsp_spdm_params: GspSpdmParams,
}

// SAFETY: GspFmcBootParams is composed of C structs with only primitive types.
unsafe impl AsBytes for GspFmcBootParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspFmcBootParams {}

/// Size constraints for FSP security signatures (in bytes).
const FSP_HASH_SIZE: usize = 48; // SHA-384 hash
const FSP_PKEY_SIZE: usize = 384; // RSA public key
const FSP_SIG_SIZE: usize = 384; // RSA signature

/// Structure to hold FMC signatures.
#[derive(Debug, Clone, Copy)]
pub(crate) struct FmcSignatures {
    hash384: [u8; FSP_HASH_SIZE], // SHA-384 hash (48 bytes)
    public_key: [u8; 384],        // RSA public key (384 bytes)
    signature: [u8; 384],         // RSA signature (384 bytes)
}

impl Default for FmcSignatures {
    fn default() -> Self {
        Self {
            hash384: [0u8; FSP_HASH_SIZE],
            public_key: [0u8; 384],
            signature: [0u8; 384],
        }
    }
}

/// FSP Command Response payload structure.
/// NVDM_PAYLOAD_COMMAND_RESPONSE structure.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct NvdmPayloadCommandResponse {
    task_id: u32,
    command_nvdm_type: u32,
    error_code: u32,
}

/// NVDM (NVIDIA Device Management) COT (Chain of Trust) payload structure.
/// This is the main message payload sent to FSP for Chain of Trust.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct NvdmPayloadCot {
    version: u16,               // offset 0x0, size 2
    size: u16,                  // offset 0x2, size 2
    gsp_fmc_sysmem_offset: u64, // offset 0x4, size 8
    frts_sysmem_offset: u64,    // offset 0xC, size 8
    frts_sysmem_size: u32,      // offset 0x14, size 4
    frts_vidmem_offset: u64,    // offset 0x18, size 8
    frts_vidmem_size: u32,      // offset 0x20, size 4
    // Authentication related fields
    hash384: [u8; FSP_HASH_SIZE],     // offset 0x24, size 48 (0x30)
    public_key: [u8; FSP_PKEY_SIZE],  // offset 0x54, size 384 (0x180)
    signature: [u8; FSP_SIG_SIZE],    // offset 0x1D4, size 384 (0x180)
    gsp_boot_args_sysmem_offset: u64, // offset 0x354, size 8
}

/// Complete FSP message structure with MCTP and NVDM headers.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct FspMessage {
    mctp_header: u32,
    nvdm_header: u32,
    cot: NvdmPayloadCot,
}

// SAFETY: FspMessage is a packed C struct with only integral fields.
unsafe impl AsBytes for FspMessage {}

/// Complete FSP response structure with MCTP and NVDM headers.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct FspResponse {
    mctp_header: u32,
    nvdm_header: u32,
    response: NvdmPayloadCommandResponse,
}

// SAFETY: FspResponse is a packed C struct with only integral fields.
unsafe impl FromBytes for FspResponse {}

/// Trait implemented by types representing a message to send to FSP.
///
/// This provides [`Fsp::send_sync_fsp`] with the information it needs to send
/// a given message, following the same pattern as GSP's `CommandToGsp`.
pub(crate) trait MessageToFsp: AsBytes {
    /// NVDM type identifying this message to FSP.
    const NVDM_TYPE: u32;
}

impl MessageToFsp for FspMessage {
    const NVDM_TYPE: u32 = mctp::NVDM_TYPE_COT;
}

/// FSP interface for Hopper/Blackwell GPUs.
pub(crate) struct Fsp;

impl Fsp {
    /// Wait for FSP secure boot completion.
    ///
    /// Polls the thermal scratch register until FSP signals boot completion
    /// or timeout occurs.
    pub(crate) fn wait_secure_boot(
        dev: &device::Device<device::Bound>,
        bar: &crate::driver::Bar0,
        arch: crate::gpu::Architecture,
    ) -> Result<()> {
        let timeout = Delta::from_millis(FSP_SECURE_BOOT_TIMEOUT_MS);

        read_poll_timeout(
            || crate::regs::read_fsp_boot_complete_status(bar, arch),
            |&status| {
                dev_dbg!(
                    dev,
                    "FSP I2CS scratch register status: {:#x} (expected: {:#x})\n",
                    status,
                    FSP_BOOT_COMPLETE_SUCCESS
                );
                status == FSP_BOOT_COMPLETE_SUCCESS
            },
            Delta::ZERO,
            timeout,
        )
        .map_err(|_| {
            let final_status =
                crate::regs::read_fsp_boot_complete_status(bar, arch).unwrap_or(0xDEADBEEF);
            dev_err!(
                dev,
                "FSP secure boot completion timeout - final status: {:#x}\n",
                final_status
            );
            ETIMEDOUT
        })
        .map(|_| ())
    }

    /// Extract FMC firmware signatures for Chain of Trust verification.
    ///
    /// Extracts real cryptographic signatures from FMC ELF32 firmware sections.
    /// Returns signatures in a heap-allocated structure to prevent stack overflow.
    pub(crate) fn extract_fmc_signatures_static(
        dev: &device::Device<device::Bound>,
        fmc_fw_data: &[u8],
    ) -> Result<KBox<FmcSignatures>> {
        // Extract hash section (SHA-384)
        let hash_section = crate::firmware::elf_section(fmc_fw_data, "hash")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'hash' section\n"))?;

        // Extract public key section (RSA public key)
        let pkey_section = crate::firmware::elf_section(fmc_fw_data, "publickey")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'publickey' section\n"))?;

        // Extract signature section (RSA signature)
        let sig_section = crate::firmware::elf_section(fmc_fw_data, "signature")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'signature' section\n"))?;

        // Validate section sizes - hash must be exactly 48 bytes
        if hash_section.len() != FSP_HASH_SIZE {
            dev_err!(
                dev,
                "FMC hash section size {} != expected {}\n",
                hash_section.len(),
                FSP_HASH_SIZE
            );
            return Err(EINVAL);
        }

        // Public key and signature can be smaller than the fixed array sizes.
        if pkey_section.len() > FSP_PKEY_SIZE {
            dev_err!(
                dev,
                "FMC publickey section size {} > maximum {}\n",
                pkey_section.len(),
                FSP_PKEY_SIZE
            );
            return Err(EINVAL);
        }

        if sig_section.len() > FSP_SIG_SIZE {
            dev_err!(
                dev,
                "FMC signature section size {} > maximum {}\n",
                sig_section.len(),
                FSP_SIG_SIZE
            );
            return Err(EINVAL);
        }

        // Allocate signature structure on heap to avoid stack overflow
        let mut signatures = KBox::new(FmcSignatures::default(), GFP_KERNEL)?;

        // Copy hash section directly (48 bytes exactly)
        signatures.hash384.copy_from_slice(hash_section);

        // Copy public key section (up to 384 bytes, zero-padded)
        signatures.public_key[..pkey_section.len()].copy_from_slice(pkey_section);

        // Copy signature section (up to 384 bytes, zero-padded)
        signatures.signature[..sig_section.len()].copy_from_slice(sig_section);

        Ok(signatures)
    }

    /// Creates FMC boot parameters structure for FSP.
    ///
    /// This structure tells FSP how to boot GSP-RM with the correct memory layout.
    pub(crate) fn create_fmc_boot_params(
        dev: &device::Device<device::Bound>,
        wpr_meta_addr: u64,
        wpr_meta_size: u32,
        libos_addr: u64,
    ) -> Result<kernel::dma::CoherentAllocation<GspFmcBootParams>> {
        const GSP_DMA_TARGET_COHERENT_SYSTEM: u32 = 1;
        const GSP_DMA_TARGET_NONCOHERENT_SYSTEM: u32 = 2;

        let fmc_boot_params = CoherentAllocation::<GspFmcBootParams>::alloc_coherent(
            dev,
            1,
            GFP_KERNEL | __GFP_ZERO,
        )?;

        // Configure ACR boot parameters (WPR metadata location) using dma_write! macro
        kernel::dma_write!(
            fmc_boot_params[0].boot_gsp_rm_params.target = GSP_DMA_TARGET_COHERENT_SYSTEM
        )?;
        kernel::dma_write!(
            fmc_boot_params[0].boot_gsp_rm_params.gsp_rm_desc_offset = wpr_meta_addr
        )?;
        kernel::dma_write!(fmc_boot_params[0].boot_gsp_rm_params.gsp_rm_desc_size = wpr_meta_size)?;

        // Blackwell FSP expects wpr_carveout_offset and wpr_carveout_size to be zero;
        // it obtains WPR info from other sources.
        kernel::dma_write!(fmc_boot_params[0].boot_gsp_rm_params.b_is_gsp_rm_boot = 1)?;

        // Configure RM parameters (libos location) using dma_write! macro
        kernel::dma_write!(
            fmc_boot_params[0].gsp_rm_params.target = GSP_DMA_TARGET_NONCOHERENT_SYSTEM
        )?;
        kernel::dma_write!(fmc_boot_params[0].gsp_rm_params.boot_args_offset = libos_addr)?;

        Ok(fmc_boot_params)
    }

    /// Boot GSP FMC with pre-extracted signatures.
    ///
    /// This version takes pre-extracted signatures and FMC image data.
    /// Used when signatures are extracted separately from the full ELF file.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn boot_gsp_fmc_with_signatures(
        dev: &device::Device<device::Bound>,
        bar: &crate::driver::Bar0,
        chipset: crate::gpu::Chipset,
        fmc_image_fw: &crate::dma::DmaObject, // Contains only the image section
        fmc_boot_params: &kernel::dma::CoherentAllocation<GspFmcBootParams>,
        total_reserved_size: u64,
        resume: bool,
        fsp_falcon: &crate::falcon::Falcon<crate::falcon::fsp::Fsp>,
        signatures: &FmcSignatures,
    ) -> Result<()> {
        dev_dbg!(dev, "Starting FSP boot sequence for {}\n", chipset);

        // Build FSP Chain of Trust message
        let fmc_addr = fmc_image_fw.dma_handle(); // Now points to image data only
        let fmc_boot_params_addr = fmc_boot_params.dma_handle();

        // frts_offset is relative to FB end: FRTS_location = FB_END - frts_offset
        let frts_offset = if !resume {
            let mut frts_reserved_size =
                if let Some(heap_size) = crate::fb::hal::fb_hal(chipset).non_wpr_heap_size() {
                    u64::from(heap_size)
                } else {
                    total_reserved_size
                };

            // Add PMU reserved size
            frts_reserved_size += u64::from(crate::fb::PMU_RESERVED_SIZE);

            frts_reserved_size
                .align_up(Alignment::new::<SZ_2M>())
                .unwrap_or(frts_reserved_size)
        } else {
            0
        };
        let frts_size = if !resume { SZ_1M as u32 } else { 0 };

        // Build the FSP message
        let msg = KBox::new(
            FspMessage {
                mctp_header: (mctp::HEADER_SOM << mctp::HEADER_SOM_SHIFT)
                    | (mctp::HEADER_EOM << mctp::HEADER_EOM_SHIFT)
                    | (mctp::HEADER_SEID << mctp::HEADER_SEID_SHIFT)
                    | (mctp::HEADER_SEQ << mctp::HEADER_SEQ_SHIFT),

                nvdm_header: (mctp::MSG_TYPE_VENDOR_PCI)
                    | (mctp::VENDOR_ID_NV << mctp::NVDM_VENDOR_ID_SHIFT)
                    | (mctp::NVDM_TYPE_COT << mctp::NVDM_TYPE_SHIFT),

                cot: NvdmPayloadCot {
                    version: chipset.fsp_cot_version(),
                    size: core::mem::size_of::<NvdmPayloadCot>() as u16,
                    gsp_fmc_sysmem_offset: fmc_addr,
                    frts_sysmem_offset: 0,
                    frts_sysmem_size: 0,
                    frts_vidmem_offset: frts_offset,
                    frts_vidmem_size: frts_size,
                    hash384: signatures.hash384,
                    public_key: signatures.public_key,
                    signature: signatures.signature,
                    gsp_boot_args_sysmem_offset: fmc_boot_params_addr,
                },
            },
            GFP_KERNEL,
        )?;

        // Send COT message to FSP and wait for response
        Self::send_sync_fsp(dev, bar, fsp_falcon, &*msg)?;

        dev_dbg!(dev, "FSP Chain of Trust completed successfully\n");
        Ok(())
    }

    /// Send message to FSP and wait for response.
    fn send_sync_fsp<M>(
        dev: &device::Device<device::Bound>,
        bar: &crate::driver::Bar0,
        fsp_falcon: &crate::falcon::Falcon<crate::falcon::fsp::Fsp>,
        msg: &M,
    ) -> Result<()>
    where
        M: MessageToFsp,
    {
        // Send message
        fsp_falcon.send_msg(bar, msg.as_bytes())?;

        // Wait for response
        let timeout = Delta::from_millis(FSP_MSG_TIMEOUT_MS);
        let packet_size = read_poll_timeout(
            || Ok(fsp_falcon.poll_msgq(bar)),
            |&size| size > 0,
            Delta::ZERO,
            timeout,
        )
        .map_err(|_| {
            dev_err!(dev, "FSP response timeout\n");
            ETIMEDOUT
        })?;

        // Receive response
        let packet_size = packet_size as usize;
        let mut response_buf = KVec::<u8>::new();
        response_buf.resize(packet_size, 0, GFP_KERNEL)?;
        fsp_falcon.recv_msg(bar, &mut response_buf, packet_size)?;

        // Parse response
        if response_buf.len() < core::mem::size_of::<FspResponse>() {
            dev_err!(dev, "FSP response too small: {}\n", response_buf.len());
            return Err(EIO);
        }

        let response = FspResponse::from_bytes(&response_buf[..]).ok_or(EIO)?;

        // Copy packed struct fields to avoid alignment issues
        let mctp_header = response.mctp_header;
        let nvdm_header = response.nvdm_header;
        let command_nvdm_type = response.response.command_nvdm_type;
        let error_code = response.response.error_code;

        // Validate MCTP header
        let mctp_som = (mctp_header >> 31) & 1;
        let mctp_eom = (mctp_header >> 30) & 1;
        if mctp_som != 1 || mctp_eom != 1 {
            dev_err!(
                dev,
                "Unexpected MCTP header in FSP reply: {:#x}\n",
                mctp_header
            );
            return Err(EIO);
        }

        // Validate NVDM header
        let nvdm_msg_type = nvdm_header & 0x7f;
        let nvdm_vendor_id = (nvdm_header >> 8) & 0xffff;
        let nvdm_type_resp = (nvdm_header >> 24) & 0xff;

        if nvdm_msg_type != mctp::MSG_TYPE_VENDOR_PCI
            || nvdm_vendor_id != mctp::VENDOR_ID_NV
            || nvdm_type_resp != mctp::NVDM_TYPE_FSP_RESPONSE
        {
            dev_err!(
                dev,
                "Unexpected NVDM header in FSP reply: {:#x}\n",
                nvdm_header
            );
            return Err(EIO);
        }

        // Check command type matches
        if command_nvdm_type != M::NVDM_TYPE {
            dev_err!(
                dev,
                "Expected NVDM type {:#x} in reply, got {:#x}\n",
                M::NVDM_TYPE,
                command_nvdm_type
            );
            return Err(EIO);
        }

        // Check for errors
        if error_code != 0 {
            dev_err!(
                dev,
                "NVDM command {:#x} failed with error {:#x}\n",
                M::NVDM_TYPE,
                error_code
            );
            return Err(EIO);
        }

        Ok(())
    }
}
