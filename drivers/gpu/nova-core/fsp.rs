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

/// FSP secure boot completion timeout in milliseconds.
///
/// GB20x requires a longer timeout than Hopper/GB10x.
const fn fsp_secure_boot_timeout_ms(arch: crate::gpu::Architecture) -> i64 {
    match arch {
        crate::gpu::Architecture::BlackwellGB20x => 5000,
        _ => 4000,
    }
}

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

        let timeout = Delta::from_millis(fsp_secure_boot_timeout_ms(arch));

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
}
