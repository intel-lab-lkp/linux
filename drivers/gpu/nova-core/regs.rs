// SPDX-License-Identifier: GPL-2.0

// Required to retain the original register names used by OpenRM, which are all capital snake case
// but are mapped to types.
#![allow(non_camel_case_types)]

#[macro_use]
pub(crate) mod macros;

use crate::falcon::{
    DmaTrfCmdSize, FalconCoreRev, FalconCoreRevSubversion, FalconFbifMemType, FalconFbifTarget,
    FalconModSelAlgo, FalconSecurityModel, PFalcon2Base, PFalconBase, PeregrineCoreSelect,
};
use crate::gpu::{Architecture, Chipset};
use kernel::prelude::*;

// PMC

register!(NV_PMC_BOOT_0 @ 0x00000000, "Basic revision information about the GPU" {
    3:0     minor_revision, "Minor revision of the chip";
    7:4     major_revision, "Major revision of the chip";
    8:8     architecture_1, "MSB of the architecture";
    23:20   implementation, "Implementation version of the architecture";
    28:24   architecture_0, "Lower bits of the architecture";
});

impl NV_PMC_BOOT_0 {
    /// Combines `architecture_0` and `architecture_1` to obtain the architecture of the chip.
    pub(crate) fn architecture(self) -> Result<Architecture> {
        Architecture::try_from(
            u8::from(self.architecture_0())
                | (u8::from(self.architecture_1()) << Self::ARCHITECTURE_0_RANGE.len()),
        )
    }

    /// Combines `architecture` and `implementation` to obtain a code unique to the chipset.
    pub(crate) fn chipset(self) -> Result<Chipset> {
        self.architecture()
            .map(|arch| {
                ((arch as u32) << Self::IMPLEMENTATION_RANGE.len())
                    | u32::from(self.implementation())
            })
            .and_then(Chipset::try_from)
    }
}

// PBUS

register!(NV_PBUS_SW_SCRATCH @ 0x00001400[64]  {});

register!(NV_PBUS_SW_SCRATCH_0E_FRTS_ERR => NV_PBUS_SW_SCRATCH[0xe],
    "scratch register 0xe used as FRTS firmware error code" {
    31:16   frts_err_code;
});

// PFB

// The following two registers together hold the physical system memory address that is used by the
// GPU to perform sysmembar operations (see `fb::SysmemFlush`).

register!(NV_PFB_NISO_FLUSH_SYSMEM_ADDR @ 0x00100c10 {
    31:0    adr_39_08;
});

register!(NV_PFB_NISO_FLUSH_SYSMEM_ADDR_HI @ 0x00100c40 {
    23:0    adr_63_40;
});

register!(NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE @ 0x00100ce0 {
    3:0     lower_scale;
    9:4     lower_mag;
    30:30   ecc_mode_enabled => bool;
});

impl NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE {
    /// Returns the usable framebuffer size, in bytes.
    pub(crate) fn usable_fb_size(self) -> u64 {
        let size = (u64::from(self.lower_mag()) << u64::from(self.lower_scale()))
            * kernel::sizes::SZ_1M as u64;

        if self.ecc_mode_enabled() {
            // Remove the amount of memory reserved for ECC (one per 16 units).
            size / 16 * 15
        } else {
            size
        }
    }
}

register!(NV_PFB_PRI_MMU_WPR2_ADDR_LO@0x001fa824  {
    31:4    lo_val, "Bits 12..40 of the lower (inclusive) bound of the WPR2 region";
});

impl NV_PFB_PRI_MMU_WPR2_ADDR_LO {
    /// Returns the lower (inclusive) bound of the WPR2 region.
    pub(crate) fn lower_bound(self) -> u64 {
        u64::from(self.lo_val()) << 12
    }
}

register!(NV_PFB_PRI_MMU_WPR2_ADDR_HI@0x001fa828  {
    31:4    hi_val, "Bits 12..40 of the higher (exclusive) bound of the WPR2 region";
});

impl NV_PFB_PRI_MMU_WPR2_ADDR_HI {
    /// Returns the higher (exclusive) bound of the WPR2 region.
    ///
    /// A value of zero means the WPR2 region is not set.
    pub(crate) fn higher_bound(self) -> u64 {
        u64::from(self.hi_val()) << 12
    }
}

// PGC6 register space.
//
// `GC6` is a GPU low-power state where VRAM is in self-refresh and the GPU is powered down (except
// for power rails needed to keep self-refresh working and important registers and hardware
// blocks).
//
// These scratch registers remain powered on even in a low-power state and have a designated group
// number.

// Privilege level mask register. It dictates whether the host CPU has privilege to access the
// `PGC6_AON_SECURE_SCRATCH_GROUP_05` register (which it needs to read GFW_BOOT).
register!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK @ 0x00118128,
          "Privilege level mask register" {
    0:0     read_protection_level0 => bool, "Set after FWSEC lowers its protection level";
});

// OpenRM defines this as a register array, but doesn't specify its size and only uses its first
// element. Be conservative until we know the actual size or need to use more registers.
register!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05 @ 0x00118234[1] {});

register!(
    NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT => NV_PGC6_AON_SECURE_SCRATCH_GROUP_05[0],
    "Scratch group 05 register 0 used as GFW boot progress indicator" {
        7:0    progress, "Progress of GFW boot (0xff means completed)";
    }
);

impl NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT {
    /// Returns `true` if GFW boot is completed.
    pub(crate) fn completed(self) -> bool {
        self.progress() == 0xff
    }
}

register!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_42 @ 0x001183a4 {
    31:0    value;
});

register!(
    NV_USABLE_FB_SIZE_IN_MB => NV_PGC6_AON_SECURE_SCRATCH_GROUP_42,
    "Scratch group 42 register used as framebuffer size" {
        31:0    value, "Usable framebuffer size, in megabytes";
    }
);

impl NV_USABLE_FB_SIZE_IN_MB {
    /// Returns the usable framebuffer size, in bytes.
    pub(crate) fn usable_fb_size(self) -> u64 {
        u64::from(self.value()) * kernel::sizes::SZ_1M as u64
    }
}

// PDISP

register!(NV_PDISP_VGA_WORKSPACE_BASE @ 0x00625f04 {
    3:3     status_valid => bool, "Set if the `addr` field is valid";
    31:8    addr, "VGA workspace base address divided by 0x10000";
});

impl NV_PDISP_VGA_WORKSPACE_BASE {
    /// Returns the base address of the VGA workspace, or `None` if none exists.
    pub(crate) fn vga_workspace_addr(self) -> Option<u64> {
        if self.status_valid() {
            Some(u64::from(self.addr()) << 16)
        } else {
            None
        }
    }
}

// FUSE

pub(crate) const NV_FUSE_OPT_FPF_SIZE: usize = 16;

register!(NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION @ 0x00824100[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

register!(NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION @ 0x00824140[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

register!(NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION @ 0x008241c0[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

// PFALCON

register!(NV_PFALCON_FALCON_IRQSCLR @ PFalconBase[0x00000004] {
    4:4     halt => bool;
    6:6     swgen0 => bool;
});

register!(NV_PFALCON_FALCON_MAILBOX0 @ PFalconBase[0x00000040] {
    31:0    value => u32;
});

register!(NV_PFALCON_FALCON_MAILBOX1 @ PFalconBase[0x00000044] {
    31:0    value => u32;
});

register!(NV_PFALCON_FALCON_RM @ PFalconBase[0x00000084] {
    31:0    value => u32;
});

register!(NV_PFALCON_FALCON_HWCFG2 @ PFalconBase[0x000000f4] {
    10:10   riscv => bool;
    12:12   mem_scrubbing => bool, "Set to 0 after memory scrubbing is completed";
    31:31   reset_ready => bool, "Signal indicating that reset is completed (GA102+)";
});

impl NV_PFALCON_FALCON_HWCFG2 {
    /// Returns `true` if memory scrubbing is completed.
    pub(crate) fn mem_scrubbing_done(self) -> bool {
        !self.mem_scrubbing()
    }
}

register!(NV_PFALCON_FALCON_CPUCTL @ PFalconBase[0x00000100] {
    1:1     startcpu => bool;
    4:4     halted => bool;
    6:6     alias_en => bool;
});

register!(NV_PFALCON_FALCON_BOOTVEC @ PFalconBase[0x00000104] {
    31:0    value => u32;
});

register!(NV_PFALCON_FALCON_DMACTL @ PFalconBase[0x0000010c] {
    0:0     require_ctx => bool;
    1:1     dmem_scrubbing => bool;
    2:2     imem_scrubbing => bool;
    6:3     dmaq_num;
    7:7     secure_stat => bool;
});

register!(NV_PFALCON_FALCON_DMATRFBASE @ PFalconBase[0x00000110] {
    31:0    base => u32;
});

register!(NV_PFALCON_FALCON_DMATRFMOFFS @ PFalconBase[0x00000114] {
    23:0    offs;
});

register!(NV_PFALCON_FALCON_DMATRFCMD @ PFalconBase[0x00000118] {
    0:0     full => bool;
    1:1     idle => bool;
    3:2     sec;
    4:4     imem => bool;
    5:5     is_write => bool;
    10:8    size ?=> DmaTrfCmdSize;
    14:12   ctxdma;
    16:16   set_dmtag;
});

register!(NV_PFALCON_FALCON_DMATRFFBOFFS @ PFalconBase[0x0000011c] {
    31:0    offs => u32;
});

register!(NV_PFALCON_FALCON_DMATRFBASE1 @ PFalconBase[0x00000128] {
    8:0     base;
});

register!(NV_PFALCON_FALCON_HWCFG1 @ PFalconBase[0x0000012c] {
    3:0     core_rev ?=> FalconCoreRev, "Core revision";
    5:4     security_model ?=> FalconSecurityModel, "Security model";
    7:6     core_rev_subversion => FalconCoreRevSubversion, "Core revision subversion";
});

register!(NV_PFALCON_FALCON_CPUCTL_ALIAS @ PFalconBase[0x00000130] {
    1:1     startcpu => bool;
});

// Actually known as `NV_PSEC_FALCON_ENGINE` and `NV_PGSP_FALCON_ENGINE` depending on the falcon
// instance.
register!(NV_PFALCON_FALCON_ENGINE @ PFalconBase[0x000003c0] {
    0:0     reset => bool;
});

register!(NV_PFALCON_FBIF_TRANSCFG @ PFalconBase[0x00000600[8]] {
    1:0     target ?=> FalconFbifTarget;
    2:2     mem_type => FalconFbifMemType;
});

register!(NV_PFALCON_FBIF_CTL @ PFalconBase[0x00000624] {
    7:7     allow_phys_no_ctx => bool;
});

/* PFALCON2 */

register!(NV_PFALCON2_FALCON_MOD_SEL @ PFalcon2Base[0x00000180] {
    7:0     algo ?=> FalconModSelAlgo;
});

register!(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID @ PFalcon2Base[0x00000198] {
    7:0    ucode_id => u8;
});

register!(NV_PFALCON2_FALCON_BROM_ENGIDMASK @ PFalcon2Base[0x0000019c] {
    31:0    value => u32;
});

// OpenRM defines this as a register array, but doesn't specify its size and only uses its first
// element. Be conservative until we know the actual size or need to use more registers.
register!(NV_PFALCON2_FALCON_BROM_PARAADDR @ PFalcon2Base[0x00000210[1]] {
    31:0    value => u32;
});

// PRISCV

register!(NV_PRISCV_RISCV_BCR_CTRL @ PFalconBase[0x00001668] {
    0:0     valid => bool;
    4:4     core_select => PeregrineCoreSelect;
    8:8     br_fetch => bool;
});

// The modules below provide registers that are not identical on all supported chips. They should
// only be used in HAL modules.

pub(crate) mod gm107 {
    // FUSE

    register!(NV_FUSE_STATUS_OPT_DISPLAY @ 0x00021c04 {
        0:0     display_disabled => bool;
    });
}

pub(crate) mod ga100 {
    // FUSE

    register!(NV_FUSE_STATUS_OPT_DISPLAY @ 0x00820c04 {
        0:0     display_disabled => bool;
    });
}
