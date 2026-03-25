// SPDX-License-Identifier: GPL-2.0

//! FSP (Firmware System Processor) interface for Hopper/Blackwell GPUs.
//!
//! Hopper/Blackwell use a simplified firmware boot sequence: FMC --> FSP --> GSP.
//! Unlike Turing/Ampere/Ada, there is NO SEC2 (Security Engine 2) usage.
//! FSP handles secure boot directly using FMC firmware + Chain of Trust.

use kernel::{
    device,
    dma::{
        Coherent,
        CoherentBox, //
    },
    io::poll::read_poll_timeout,
    prelude::*,
    ptr::{
        Alignable,
        Alignment, //
    },
    sizes::{
        SZ_1M,
        SZ_2M, //
    },
    time::Delta,
    transmute::{
        AsBytes,
        FromBytes, //
    },
};

use crate::regs;

use crate::mctp::{
    MctpHeader,
    NvdmHeader,
    NvdmType, //
};

/// FSP Chain of Trust protocol version.
///
/// Hopper (GH100) uses version 1, Blackwell uses version 2.
#[derive(Debug, Clone, Copy)]
pub(crate) struct FspCotVersion(u16);

impl FspCotVersion {
    /// Create a new FSP COT version.
    pub(crate) const fn new(version: u16) -> Self {
        Self(version)
    }

    /// Return the raw protocol version number for the wire format.
    pub(crate) const fn raw(self) -> u16 {
        self.0
    }
}

/// FSP message timeout in milliseconds.
const FSP_MSG_TIMEOUT_MS: i64 = 2000;

/// FSP secure boot completion timeout in milliseconds.
const FSP_SECURE_BOOT_TIMEOUT_MS: i64 = 4000;

/// GSP FMC initialization parameters.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
struct GspFmcInitParams {
    /// CC initialization "registry keys".
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
    /// Physical memory aperture through which gspRmDescPa is accessed.
    target: u32,
    /// Size in bytes of the GSP-RM descriptor structure.
    gsp_rm_desc_size: u32,
    /// Physical offset in the target aperture of the GSP-RM descriptor structure.
    gsp_rm_desc_offset: u64,
    /// Physical offset in FB to set the start of the WPR containing GSP-RM.
    wpr_carveout_offset: u64,
    /// Size in bytes of the WPR containing GSP-RM.
    wpr_carveout_size: u32,
    /// Whether to boot GSP-RM or GSP-Proxy through ACR.
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
    /// Physical memory aperture through which bootArgsOffset is accessed.
    target: u32,
    /// Physical offset in the memory aperture that will be passed to GSP-RM.
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
    /// Physical memory aperture through which all addresses are accessed.
    target: u32,
    /// Physical offset in the memory aperture where SPDM payload buffer is stored.
    payload_buffer_offset: u64,
    /// Size of the above payload buffer.
    payload_buffer_size: u32,
}

// SAFETY: GspSpdmParams is a simple C struct with only primitive types.
unsafe impl AsBytes for GspSpdmParams {}
// SAFETY: All bit patterns are valid for the primitive fields.
unsafe impl FromBytes for GspSpdmParams {}

/// Complete GSP FMC boot parameters passed to FSP.
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

/// Size constraints for FSP security signatures (Hopper/Blackwell).
const FSP_HASH_SIZE: usize = 48; // SHA-384 hash
const FSP_PKEY_SIZE: usize = 384; // RSA-3072 public key
const FSP_SIG_SIZE: usize = 384; // RSA-3072 signature

/// Structure to hold FMC signatures.
#[derive(Debug, Clone, Copy)]
pub(crate) struct FmcSignatures {
    hash384: [u8; FSP_HASH_SIZE],
    public_key: [u8; FSP_PKEY_SIZE],
    signature: [u8; FSP_SIG_SIZE],
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
    version: u16,
    size: u16,
    gsp_fmc_sysmem_offset: u64,
    frts_sysmem_offset: u64,
    frts_sysmem_size: u32,
    frts_vidmem_offset: u64,
    frts_vidmem_size: u32,
    hash384: [u8; FSP_HASH_SIZE],
    public_key: [u8; FSP_PKEY_SIZE],
    signature: [u8; FSP_SIG_SIZE],
    gsp_boot_args_sysmem_offset: u64,
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
    const NVDM_TYPE: u32 = NvdmType::Cot as u32;
}

/// Bundled arguments for FMC boot via FSP Chain of Trust.
pub(crate) struct FmcBootArgs<'a> {
    chipset: crate::gpu::Chipset,
    fmc_image_fw: &'a crate::dma::DmaObject,
    fmc_boot_params: Coherent<GspFmcBootParams>,
    resume: bool,
    signatures: &'a FmcSignatures,
}

impl<'a> FmcBootArgs<'a> {
    /// Build FMC boot arguments, allocating the DMA-coherent boot parameter
    /// structure that FSP will read.
    #[expect(dead_code)]
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn new(
        dev: &device::Device<device::Bound>,
        chipset: crate::gpu::Chipset,
        fmc_image_fw: &'a crate::dma::DmaObject,
        wpr_meta_addr: u64,
        wpr_meta_size: u32,
        libos_addr: u64,
        resume: bool,
        signatures: &'a FmcSignatures,
    ) -> Result<Self> {
        // `GSP_DMA_TARGET_*` is not in the current Rust bindings yet.
        const GSP_DMA_TARGET_COHERENT_SYSTEM: u32 = 1;
        const GSP_DMA_TARGET_NONCOHERENT_SYSTEM: u32 = 2;

        let mut fmc_boot_params = CoherentBox::<GspFmcBootParams>::zeroed(dev, GFP_KERNEL)?;

        // Blackwell FSP expects wpr_carveout_offset and wpr_carveout_size to be zero;
        // it obtains WPR info from other sources.
        fmc_boot_params.boot_gsp_rm_params = GspAcrBootGspRmParams {
            target: GSP_DMA_TARGET_COHERENT_SYSTEM,
            gsp_rm_desc_size: wpr_meta_size,
            gsp_rm_desc_offset: wpr_meta_addr,
            b_is_gsp_rm_boot: 1,
            ..Default::default()
        };

        fmc_boot_params.gsp_rm_params = GspRmParams {
            target: GSP_DMA_TARGET_NONCOHERENT_SYSTEM,
            boot_args_offset: libos_addr,
        };

        let fmc_boot_params: Coherent<GspFmcBootParams> = fmc_boot_params.into();

        Ok(Self {
            chipset,
            fmc_image_fw,
            fmc_boot_params,
            resume,
            signatures,
        })
    }

    /// DMA address of the FMC boot parameters, needed after boot for lockdown
    /// release polling.
    #[expect(dead_code)]
    pub(crate) fn boot_params_dma_handle(&self) -> u64 {
        self.fmc_boot_params.dma_handle()
    }
}
/// FSP interface for Hopper/Blackwell GPUs.
pub(crate) struct Fsp;

impl Fsp {
    /// Wait for FSP secure boot completion.
    ///
    /// Polls the thermal scratch register until FSP signals boot completion
    /// or timeout occurs.
    #[expect(dead_code)]
    pub(crate) fn wait_secure_boot(
        dev: &device::Device<device::Bound>,
        bar: &crate::driver::Bar0,
        arch: crate::gpu::Architecture,
    ) -> Result {
        debug_assert!(
            regs::read_fsp_boot_complete_status(bar, arch).is_some(),
            "wait_secure_boot called on non-FSP architecture"
        );

        let timeout = Delta::from_millis(FSP_SECURE_BOOT_TIMEOUT_MS);

        read_poll_timeout(
            || regs::read_fsp_boot_complete_status(bar, arch).ok_or(ENOTSUPP),
            |&status| status == regs::FSP_BOOT_COMPLETE_SUCCESS,
            Delta::from_millis(10),
            timeout,
        )
        .map_err(|_| {
            dev_err!(dev, "FSP secure boot completion timeout\n");
            ETIMEDOUT
        })
        .map(|_| ())
    }

    /// Extract FMC firmware signatures for Chain of Trust verification.
    ///
    /// Extracts real cryptographic signatures from FMC ELF32 firmware sections.
    /// Returns signatures in a heap-allocated structure to prevent stack overflow.
    #[expect(dead_code)]
    pub(crate) fn extract_fmc_signatures(
        dev: &device::Device<device::Bound>,
        fmc_fw_data: &[u8],
    ) -> Result<KBox<FmcSignatures>> {
        let hash_section = crate::firmware::elf_section(fmc_fw_data, "hash")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'hash' section\n"))?;

        let pkey_section = crate::firmware::elf_section(fmc_fw_data, "publickey")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'publickey' section\n"))?;

        let sig_section = crate::firmware::elf_section(fmc_fw_data, "signature")
            .ok_or(EINVAL)
            .inspect_err(|_| dev_err!(dev, "FMC firmware missing 'signature' section\n"))?;

        if hash_section.len() != FSP_HASH_SIZE {
            dev_err!(
                dev,
                "FMC hash section size {} != expected {}\n",
                hash_section.len(),
                FSP_HASH_SIZE
            );
            return Err(EINVAL);
        }

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

        let mut signatures = KBox::new(
            FmcSignatures {
                hash384: [0u8; FSP_HASH_SIZE],
                public_key: [0u8; FSP_PKEY_SIZE],
                signature: [0u8; FSP_SIG_SIZE],
            },
            GFP_KERNEL,
        )?;

        signatures.hash384.copy_from_slice(hash_section);
        signatures.public_key[..pkey_section.len()].copy_from_slice(pkey_section);
        signatures.signature[..sig_section.len()].copy_from_slice(sig_section);

        Ok(signatures)
    }

    /// Boot GSP FMC via FSP Chain of Trust.
    ///
    /// Builds the COT message from the pre-configured [`FmcBootArgs`], sends it
    /// to FSP, and waits for the response.
    #[expect(dead_code)]
    pub(crate) fn boot_fmc(
        dev: &device::Device<device::Bound>,
        bar: &crate::driver::Bar0,
        fsp_falcon: &crate::falcon::Falcon<crate::falcon::fsp::Fsp>,
        args: &FmcBootArgs<'_>,
    ) -> Result {
        dev_dbg!(dev, "Starting FSP boot sequence for {}\n", args.chipset);

        let fmc_addr = args.fmc_image_fw.dma_handle();
        let fmc_boot_params_addr = args.fmc_boot_params.dma_handle();

        // frts_offset is relative to FB end: FRTS_location = FB_END - frts_offset
        let frts_offset = if !args.resume {
            let frts_reserved_size = crate::fb::calc_non_wpr_heap_size(args.chipset)
                .checked_add(u64::from(crate::fb::PMU_RESERVED_SIZE))
                .ok_or(EINVAL)?;

            frts_reserved_size
                .align_up(Alignment::new::<SZ_2M>())
                .ok_or(EINVAL)?
        } else {
            0
        };
        let frts_size: u32 = if !args.resume { SZ_1M as u32 } else { 0 };

        let msg = KBox::new(
            FspMessage {
                mctp_header: MctpHeader::single_packet().raw(),
                nvdm_header: NvdmHeader::new(NvdmType::Cot).raw(),

                cot: NvdmPayloadCot {
                    version: args.chipset.fsp_cot_version().ok_or(ENOTSUPP)?.raw(),
                    size: u16::try_from(core::mem::size_of::<NvdmPayloadCot>())
                        .map_err(|_| EINVAL)?,
                    gsp_fmc_sysmem_offset: fmc_addr,
                    frts_sysmem_offset: 0,
                    frts_sysmem_size: 0,
                    frts_vidmem_offset: frts_offset,
                    frts_vidmem_size: frts_size,
                    hash384: args.signatures.hash384,
                    public_key: args.signatures.public_key,
                    signature: args.signatures.signature,
                    gsp_boot_args_sysmem_offset: fmc_boot_params_addr,
                },
            },
            GFP_KERNEL,
        )?;

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
    ) -> Result
    where
        M: MessageToFsp,
    {
        fsp_falcon.send_msg(bar, msg.as_bytes())?;

        let timeout = Delta::from_millis(FSP_MSG_TIMEOUT_MS);
        let packet_size = read_poll_timeout(
            || Ok(fsp_falcon.poll_msgq(bar)),
            |&size| size > 0,
            Delta::from_millis(10),
            timeout,
        )
        .map_err(|_| {
            dev_err!(dev, "FSP response timeout\n");
            ETIMEDOUT
        })?;

        let packet_size = packet_size as usize;
        let mut response_buf = KVec::<u8>::new();
        response_buf.resize(packet_size, 0, GFP_KERNEL)?;
        fsp_falcon.recv_msg(bar, &mut response_buf, packet_size)?;

        if response_buf.len() < core::mem::size_of::<FspResponse>() {
            dev_err!(dev, "FSP response too small: {}\n", response_buf.len());
            return Err(EIO);
        }

        let response = FspResponse::from_bytes(&response_buf[..]).ok_or(EIO)?;

        let mctp_header: MctpHeader = response.mctp_header.into();
        let nvdm_header: NvdmHeader = response.nvdm_header.into();
        let command_nvdm_type = response.response.command_nvdm_type;
        let error_code = response.response.error_code;

        if !mctp_header.is_single_packet() {
            dev_err!(
                dev,
                "Unexpected MCTP header in FSP reply: {:#x}\n",
                mctp_header.raw()
            );
            return Err(EIO);
        }

        if !nvdm_header.validate(NvdmType::FspResponse) {
            dev_err!(
                dev,
                "Unexpected NVDM header in FSP reply: {:#x}\n",
                nvdm_header.raw()
            );
            return Err(EIO);
        }

        if command_nvdm_type != M::NVDM_TYPE {
            dev_err!(
                dev,
                "Expected NVDM type {:#x} in reply, got {:#x}\n",
                M::NVDM_TYPE,
                command_nvdm_type
            );
            return Err(EIO);
        }

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
