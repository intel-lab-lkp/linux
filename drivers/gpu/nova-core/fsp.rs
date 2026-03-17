// SPDX-License-Identifier: GPL-2.0

//! FSP (Firmware System Processor) interface for Hopper/Blackwell GPUs.
//!
//! Hopper/Blackwell use a simplified firmware boot sequence: FMC --> FSP --> GSP.
//! Unlike Turing/Ampere/Ada, there is NO SEC2 (Security Engine 2) usage.
//! FSP handles secure boot directly using FMC firmware + Chain of Trust.

use kernel::{
    device,
    io::poll::read_poll_timeout,
    prelude::*,
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

    /// Send message to FSP and wait for response.
    #[expect(dead_code)]
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
