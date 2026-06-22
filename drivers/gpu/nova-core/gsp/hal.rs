// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

mod ga102;
mod gh100;
mod tu102;

use kernel::{
    dma::Coherent,
    prelude::*, //
};

use crate::{
    fb::FbLayout,
    firmware::gsp::GspFirmware,
    gpu::Chipset,
    gsp::{
        Gsp,
        GspBootContext,
        GspBootMethod,
        GspFwWprMeta, //
    },
};

/// Trait for types containing the resources and code required to fully reset the GSP.
///
/// The GSP unload code might run in a situation where we cannot load firmware dynamically (e.g.
/// because we are in shutdown and the file system is not accessible anymore). Thus, the firmware
/// required for unloading is prepared at load time, and stored here until it needs to be run.
pub(super) trait UnloadBundle: Send {
    /// Performs the steps required to properly reset the GSP after it has been stopped.
    fn run(&self, ctx: &GspBootContext<'_>) -> Result;
}

/// Trait implemented by GSP HALs.
pub(super) trait GspHal: Send {
    /// Performs the GSP boot process, loading and running the required firmwares as needed.
    ///
    /// Returns two things:
    ///
    /// - The `Result` of the boot process itself,
    /// - The `UnloadBundle` to use with [`Gsp::unload`], or `Err` if the bundle could not be
    ///   created.
    ///
    /// Note that the two returned values are independent: it is possible for the boot process to
    /// succeed while the unload bundle couldn't be created. In this case, the GSP won't be able to
    /// unload properly and a full GPU reset is required before the GSP can be booted again.
    fn boot(
        &self,
        gsp: &Gsp,
        ctx: &GspBootContext<'_>,
        fb_layout: &FbLayout,
        wpr_meta: &Coherent<GspFwWprMeta>,
    ) -> (Result, Result<crate::gsp::UnloadBundle>);

    /// Performs HAL-specific post-GSP boot tasks.
    ///
    /// This method is called by the GSP boot code after the GSP is confirmed to be running, and
    /// after the initialization commands have been pushed onto its queue.
    fn post_boot(&self, _gsp: &Gsp, _ctx: &GspBootContext<'_>, _gsp_fw: &GspFirmware) -> Result {
        Ok(())
    }
}

/// Returns the GSP HAL to be used for `chipset`.
pub(super) fn gsp_hal(chipset: Chipset) -> &'static dyn GspHal {
    // The GSP HAL is entirely determined by the boot method.
    match chipset.gsp_boot_method() {
        GspBootMethod::Sec2 {
            needs_fwsec_bootloader: true,
        } => tu102::TU102_HAL,
        GspBootMethod::Sec2 {
            needs_fwsec_bootloader: false,
        } => ga102::GA102_HAL,
        GspBootMethod::Fsp => gh100::GH100_HAL,
    }
}
