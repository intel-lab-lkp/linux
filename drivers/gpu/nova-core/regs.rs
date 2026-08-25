// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

use kernel::{
    io::register,
    sizes::SizeConstants, //
};

// PBUS

register! {
    pub(crate) NV_PBUS_SW_SCRATCH(u32)[64] @ 0x00001400 {}
}

// PGC6 register space.
//
// `GC6` is a GPU low-power state where VRAM is in self-refresh and the GPU is powered down (except
// for power rails needed to keep self-refresh working and important registers and hardware
// blocks).
//
// These scratch registers remain powered on even in a low-power state and have a designated group
// number.

register! {
    /// Boot Sequence Interface (BSI) register used to determine
    /// if GSP reload/resume has completed during the boot process.
    pub(crate) NV_PGC6_BSI_SECURE_SCRATCH_14(u32) @ 0x001180f8 {
        26:26   boot_stage_3_handoff => bool;
    }

    /// Privilege level mask register. It dictates whether the host CPU has privilege to access the
    /// `PGC6_AON_SECURE_SCRATCH_GROUP_05` register (which it needs to read GFW_BOOT).
    pub(crate) NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK(u32) @ 0x00118128 {
        /// Set after FWSEC lowers its protection level.
        0:0     read_protection_level0 => bool;
    }

    /// OpenRM defines this as a register array, but doesn't specify its size and only uses its
    /// first element. Be conservative until we know the actual size or need to use more registers.
    pub(crate) NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(u32)[1] @ 0x00118234 {}

    /// Scratch group 05 register 0 used as GFW boot progress indicator.
    pub(crate) NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT(u32)
        => NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0] {
        /// Progress of GFW boot (0xff means completed).
        7:0    progress;
    }

    pub(crate) NV_PGC6_AON_SECURE_SCRATCH_GROUP_42(u32) @ 0x001183a4 {
        31:0    value;
    }

    /// Scratch group 42 register used as framebuffer size.
    pub(crate) NV_USABLE_FB_SIZE_IN_MB(u32) => NV_PGC6_AON_SECURE_SCRATCH_GROUP_42 {
        /// Usable framebuffer size, in megabytes.
        31:0    value;
    }
}

impl NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT {
    /// Returns `true` if GFW boot is completed.
    pub(crate) fn completed(self) -> bool {
        self.progress() == 0xff
    }
}

impl NV_USABLE_FB_SIZE_IN_MB {
    /// Returns the usable framebuffer size, in bytes.
    pub(crate) fn usable_fb_size(self) -> u64 {
        u64::from(self.value()) * u64::SZ_1M
    }
}

// FUSE

pub(crate) const NV_FUSE_OPT_FPF_SIZE: usize = 16;

register! {
    pub(crate) NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION(u32)[NV_FUSE_OPT_FPF_SIZE] @ 0x00824100 {
        15:0    data => u16;
    }

    pub(crate) NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION(u32)[NV_FUSE_OPT_FPF_SIZE] @ 0x00824140 {
        15:0    data => u16;
    }

    pub(crate) NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION(u32)[NV_FUSE_OPT_FPF_SIZE] @ 0x008241c0 {
        15:0    data => u16;
    }
}

// The modules below provide registers that are not identical on all supported chips. They should
// only be used in HAL modules.

pub(crate) mod gm107 {
    use kernel::io::register;

    // FUSE

    register! {
        pub(crate) NV_FUSE_STATUS_OPT_DISPLAY(u32) @ 0x00021c04 {
            0:0     display_disabled => bool;
        }
    }
}

pub(crate) mod ga100 {
    use kernel::io::register;

    // FUSE

    register! {
        pub(crate) NV_FUSE_STATUS_OPT_DISPLAY(u32) @ 0x00820c04 {
            0:0     display_disabled => bool;
        }
    }
}

pub(crate) const NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE_STATUS_SUCCESS: u32 = 0xff;

pub(crate) mod gh100 {
    use kernel::io::register;

    // PTHERM

    register! {
        pub(crate) NV_THERM_I2CS_SCRATCH(u32) @ 0x000200bc {
            31:0    data;
        }

        // Alias to `NV_THERM_I2CS_SCRATCH` when used to check for FSP boot completion.
        pub(crate) NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE(u32) => NV_THERM_I2CS_SCRATCH {
            31:0    fsp_boot_complete;
        }
    }
}

pub(crate) mod gb202 {
    use kernel::io::register;

    // PTHERM

    register! {
        pub(crate) NV_THERM_I2CS_SCRATCH(u32) @ 0x00ad00bc {
            31:0    data;
        }

        // Alias to `NV_THERM_I2CS_SCRATCH` when used to check for FSP boot completion.
        pub(crate) NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE(u32) => NV_THERM_I2CS_SCRATCH {
            31:0    fsp_boot_complete;
        }
    }
}
