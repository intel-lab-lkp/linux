// SPDX-License-Identifier: GPL-2.0

// TODO: remove this once the code is fully functional
#![expect(dead_code)]

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

use crate::regs::FSP_BOOT_COMPLETE_SUCCESS;

/// FSP secure boot completion timeout in milliseconds.
const FSP_SECURE_BOOT_TIMEOUT_MS: i64 = 4000;

/// MCTP (Management Component Transport Protocol) header values for FSP communication.
pub(crate) mod mctp {
    pub(super) const HEADER_SOM: u32 = 1; // Start of Message
    pub(super) const HEADER_EOM: u32 = 1; // End of Message
    pub(super) const HEADER_SEID: u32 = 0; // Source Endpoint ID
    pub(super) const HEADER_SEQ: u32 = 0; // Sequence number

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

/// Size constraints for FSP security signatures.
const FSP_HASH_SIZE: usize = 48; // SHA-384 hash
const FSP_PKEY_SIZE: usize = 97; // Public key size for GB202 (not 384!)
const FSP_SIG_SIZE: usize = 96; // Signature size for GB202 (not 384!)

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
}
