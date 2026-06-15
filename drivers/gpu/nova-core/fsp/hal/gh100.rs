// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

use kernel::io::Io;

use crate::{
    driver::Bar0,
    fsp::hal::FspHal,
    regs, //
};

struct Gh100;

/// Returns whether FSP secure boot is done on Hopper/GB10x.
pub(super) fn fsp_boot_done_gh100(bar: Bar0<'_>) -> bool {
    bar.read(regs::gh100::NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE)
        .fsp_boot_complete()
        == regs::NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE_STATUS_SUCCESS
}

impl FspHal for Gh100 {
    fn fsp_boot_done(&self, bar: Bar0<'_>) -> bool {
        fsp_boot_done_gh100(bar)
    }

    fn cot_version(&self) -> u16 {
        1
    }
}

const GH100: Gh100 = Gh100;
pub(super) const GH100_HAL: &dyn FspHal = &GH100;
