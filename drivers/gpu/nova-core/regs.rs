// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

use kernel::{
    io::register,
    sizes::SizeConstants, //
};
use pin_init::Zeroable;

use crate::{
    driver::NovaRegisters,
    mm::tlb::TlbAckMode, //
};

// PBUS

register! {
    base: NovaRegisters;

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
    base: NovaRegisters;

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
    base: NovaRegisters;

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

    use crate::driver::NovaRegisters;

    // FUSE

    register! {
        base: NovaRegisters;

        pub(crate) NV_FUSE_STATUS_OPT_DISPLAY(u32) @ 0x00021c04 {
            0:0     display_disabled => bool;
        }
    }
}

pub(crate) mod ga100 {
    use kernel::io::register;

    use crate::driver::NovaRegisters;

    // FUSE

    register! {
        base: NovaRegisters;

        pub(crate) NV_FUSE_STATUS_OPT_DISPLAY(u32) @ 0x00820c04 {
            0:0     display_disabled => bool;
        }
    }
}

pub(crate) const NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE_STATUS_SUCCESS: u32 = 0xff;

pub(crate) mod gh100 {
    use kernel::io::register;

    use crate::driver::NovaRegisters;

    // PTHERM

    register! {
        base: NovaRegisters;

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

    use crate::driver::NovaRegisters;

    // PTHERM

    register! {
        base: NovaRegisters;

        pub(crate) NV_THERM_I2CS_SCRATCH(u32) @ 0x00ad00bc {
            31:0    data;
        }

        // Alias to `NV_THERM_I2CS_SCRATCH` when used to check for FSP boot completion.
        pub(crate) NV_THERM_I2CS_SCRATCH_FSP_BOOT_COMPLETE(u32) => NV_THERM_I2CS_SCRATCH {
            31:0    fsp_boot_complete;
        }
    }
}

// MMU TLB

register! {
    base: NovaRegisters;

    /// TLB flush register: PDB address lower bits.
    pub(crate) NV_TLB_FLUSH_PDB_LO(u32) @ 0x00b830a0 {
        /// PDB address bits [39:8].
        31:0    pdb_lo => u32;
    }

    /// TLB flush register: PDB address higher bits.
    pub(crate) NV_TLB_FLUSH_PDB_HI(u32) @ 0x00b830a4 {
        /// PDB address bits [47:40].
        7:0     pdb_hi => u8;
    }

    /// TLB flush control register.
    pub(crate) NV_TLB_FLUSH_CTRL(u32) @ 0x00b830b0 {
        /// Invalidate every VA in the PDB selected by `NV_TLB_FLUSH_PDB_LO/HI`.
        0:0     all_va => bool;
        /// Invalidate TLBs for all PDBs (ignores `NV_TLB_FLUSH_PDB_LO/HI`).
        1:1     all_pdb => bool;
        /// Restrict the flush to the HUB MMU's TLBs; skip broadcasting to the
        /// per-GPC L2 TLBs.
        ///
        /// The GPU MMU has a two-level TLB hierarchy:
        /// 1. The *HUB MMU* sits at the top and serves memory requests from
        ///    "host-side" engines: the host/channel interface, copy engines,
        ///    display, and BAR1/BAR2 accesses.
        /// 2. Each GPC (Graphics Processing Cluster — the block that houses
        ///    shader cores / SMs) has its own L2 TLB that serves requests from
        ///    the compute and graphics engines inside the cluster.
        ///
        /// When set, only the HUB TLBs are invalidated. This is a performance
        /// optimization for flushes that only affect HUB-side mappings (e.g.
        /// BAR1/BAR2 windows), where fanning the invalidation out to every
        /// GPC's L2 TLB would be wasted work. Must be false when flushing
        /// mappings that may be cached by compute/graphics engines.
        2:2     hubtlb_only => bool;
        /// Invalidation acknowledgment scope. See [`TlbAckMode`] for details.
        8:7     ack ?=> TlbAckMode;
        /// Write 1 to kick off the flush. Hardware clears this bit when the
        /// flush completes; reads as 1 while the flush is in progress.
        31:31   trigger => bool;
    }
}

impl NV_TLB_FLUSH_PDB_LO {
    /// Create a register value from a PDB address.
    ///
    /// Extracts bits [39:8] of the address and shifts it right by 8 bits.
    pub(crate) fn from_pdb_addr(addr: u64) -> Self {
        Self::zeroed().with_pdb_lo(((addr >> 8) & 0xFFFF_FFFF) as u32)
    }
}

impl NV_TLB_FLUSH_PDB_HI {
    /// Create a register value from a PDB address.
    ///
    /// Extracts bits [47:40] of the address and shifts it right by 40 bits.
    pub(crate) fn from_pdb_addr(addr: u64) -> Self {
        Self::zeroed().with_pdb_hi(((addr >> 40) & 0xFF) as u8)
    }
}
