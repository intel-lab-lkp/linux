// SPDX-License-Identifier: GPL-2.0

use kernel::prelude::*;

use crate::{
    falcon::{
        DmaTrfCmdSize,
        FalconCoreRev,
        FalconCoreRevSubversion,
        FalconFbifMemType,
        FalconFbifTarget,
        FalconModSelAlgo,
        FalconSecurityModel,
        PFalcon2Base,
        PFalconBase,
        PeregrineCoreSelect, //
    },
    gpu::{
        Architecture,
        Chipset, //
    },
    num::FromSafeCast,
};

// All nova-core registers are 32-bit and `pub(crate)`. Wrap the `register!` macro to avoid
// repeating this information for every register.
macro_rules! nv_reg {
    ($(#[$attr:meta])* $name:ident $($tail:tt)*) => {
        ::kernel::register!($(#[$attr])* pub(crate) $name(u32) $($tail)*);
    };
}

// PMC

nv_reg!(NV_PMC_BOOT_0 @ 0x00000000, "Basic revision information about the GPU" {
    3:0     minor_revision, "Minor revision of the chip";
    7:4     major_revision, "Major revision of the chip";
    8:8     architecture_1, "MSB of the architecture";
    23:20   implementation, "Implementation version of the architecture";
    28:24   architecture_0, "Lower bits of the architecture";
});

impl NV_PMC_BOOT_0 {
    pub(crate) fn is_older_than_fermi(self) -> bool {
        // From https://github.com/NVIDIA/open-gpu-doc/tree/master/manuals :
        const NV_PMC_BOOT_0_ARCHITECTURE_GF100: u32 = 0xc;

        // Older chips left arch1 zeroed out. That, combined with an arch0 value that is less than
        // GF100, means "older than Fermi".
        self.architecture_1() == 0 && self.architecture_0() < NV_PMC_BOOT_0_ARCHITECTURE_GF100
    }
}

nv_reg!(NV_PMC_BOOT_42 @ 0x00000a00, "Extended architecture information" {
    15:12   minor_revision, "Minor revision of the chip";
    19:16   major_revision, "Major revision of the chip";
    23:20   implementation, "Implementation version of the architecture";
    29:24   architecture ?=> Architecture, "Architecture value";
});

impl NV_PMC_BOOT_42 {
    /// Combines `architecture` and `implementation` to obtain a code unique to the chipset.
    pub(crate) fn chipset(self) -> Result<Chipset> {
        self.architecture()
            .map(|arch| {
                ((arch as u32) << Self::IMPLEMENTATION_RANGE.len())
                    | u32::from(self.implementation())
            })
            .and_then(Chipset::try_from)
    }

    /// Returns the raw architecture value from the register.
    fn architecture_raw(self) -> u8 {
        ((self.0 >> Self::ARCHITECTURE_RANGE.start()) & ((1 << Self::ARCHITECTURE_RANGE.len()) - 1))
            as u8
    }
}

impl kernel::fmt::Display for NV_PMC_BOOT_42 {
    fn fmt(&self, f: &mut kernel::fmt::Formatter<'_>) -> kernel::fmt::Result {
        write!(
            f,
            "boot42 = 0x{:08x} (architecture 0x{:x}, implementation 0x{:x})",
            self.0,
            self.architecture_raw(),
            self.implementation()
        )
    }
}

// PBUS

nv_reg!(NV_PBUS_SW_SCRATCH @ 0x00001400[64]  {});

nv_reg!(NV_PBUS_SW_SCRATCH_0E_FRTS_ERR => NV_PBUS_SW_SCRATCH[0xe],
    "scratch register 0xe used as FRTS firmware error code" {
    31:16   frts_err_code;
});

// PFB

// The following two registers together hold the physical system memory address that is used by the
// GPU to perform sysmembar operations (see `fb::SysmemFlush`).

nv_reg!(NV_PFB_NISO_FLUSH_SYSMEM_ADDR @ 0x00100c10 {
    31:0    adr_39_08;
});

nv_reg!(NV_PFB_NISO_FLUSH_SYSMEM_ADDR_HI @ 0x00100c40 {
    23:0    adr_63_40;
});

nv_reg!(NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE @ 0x00100ce0 {
    3:0     lower_scale;
    9:4     lower_mag;
    30:30   ecc_mode_enabled => bool;
});

nv_reg!(NV_PGSP_QUEUE_HEAD @ 0x00110c00 {
    31:0    address;
});

impl NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE {
    /// Returns the usable framebuffer size, in bytes.
    pub(crate) fn usable_fb_size(self) -> u64 {
        let size = (u64::from(self.lower_mag()) << u64::from(self.lower_scale()))
            * u64::from_safe_cast(kernel::sizes::SZ_1M);

        if self.ecc_mode_enabled() {
            // Remove the amount of memory reserved for ECC (one per 16 units).
            size / 16 * 15
        } else {
            size
        }
    }
}

nv_reg!(NV_PFB_PRI_MMU_WPR2_ADDR_LO@0x001fa824  {
    31:4    lo_val, "Bits 12..40 of the lower (inclusive) bound of the WPR2 region";
});

impl NV_PFB_PRI_MMU_WPR2_ADDR_LO {
    /// Returns the lower (inclusive) bound of the WPR2 region.
    pub(crate) fn lower_bound(self) -> u64 {
        u64::from(self.lo_val()) << 12
    }
}

nv_reg!(NV_PFB_PRI_MMU_WPR2_ADDR_HI@0x001fa828  {
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

// Boot Sequence Interface (BSI) register used to determine
// if GSP reload/resume has completed during the boot process.
nv_reg!(NV_PGC6_BSI_SECURE_SCRATCH_14 @ 0x001180f8 {
    26:26   boot_stage_3_handoff => bool;
});

// Privilege level mask register. It dictates whether the host CPU has privilege to access the
// `PGC6_AON_SECURE_SCRATCH_GROUP_05` register (which it needs to read GFW_BOOT).
nv_reg!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK @ 0x00118128,
          "Privilege level mask register" {
    0:0     read_protection_level0 => bool, "Set after FWSEC lowers its protection level";
});

// OpenRM defines this as a register array, but doesn't specify its size and only uses its first
// element. Be conservative until we know the actual size or need to use more registers.
nv_reg!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05 @ 0x00118234[1] {});

nv_reg!(
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

nv_reg!(NV_PGC6_AON_SECURE_SCRATCH_GROUP_42 @ 0x001183a4 {
    31:0    value;
});

nv_reg!(
    NV_USABLE_FB_SIZE_IN_MB => NV_PGC6_AON_SECURE_SCRATCH_GROUP_42,
    "Scratch group 42 register used as framebuffer size" {
        31:0    value, "Usable framebuffer size, in megabytes";
    }
);

impl NV_USABLE_FB_SIZE_IN_MB {
    /// Returns the usable framebuffer size, in bytes.
    pub(crate) fn usable_fb_size(self) -> u64 {
        u64::from(self.value()) * u64::from_safe_cast(kernel::sizes::SZ_1M)
    }
}

// PDISP

nv_reg!(NV_PDISP_VGA_WORKSPACE_BASE @ 0x00625f04 {
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

nv_reg!(NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION @ 0x00824100[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

nv_reg!(NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION @ 0x00824140[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

nv_reg!(NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION @ 0x008241c0[NV_FUSE_OPT_FPF_SIZE] {
    15:0    data;
});

// PFALCON

nv_reg!(NV_PFALCON_FALCON_IRQSCLR @ PFalconBase[0x00000004] {
    4:4     halt => bool;
    6:6     swgen0 => bool;
});

nv_reg!(NV_PFALCON_FALCON_MAILBOX0 @ PFalconBase[0x00000040] {
    31:0    value => u32;
});

nv_reg!(NV_PFALCON_FALCON_MAILBOX1 @ PFalconBase[0x00000044] {
    31:0    value => u32;
});

// Used to store version information about the firmware running
// on the Falcon processor.
nv_reg!(NV_PFALCON_FALCON_OS @ PFalconBase[0x00000080] {
    31:0    value => u32;
});

nv_reg!(NV_PFALCON_FALCON_RM @ PFalconBase[0x00000084] {
    31:0    value => u32;
});

nv_reg!(NV_PFALCON_FALCON_HWCFG2 @ PFalconBase[0x000000f4] {
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

nv_reg!(NV_PFALCON_FALCON_CPUCTL @ PFalconBase[0x00000100] {
    1:1     startcpu => bool;
    4:4     halted => bool;
    6:6     alias_en => bool;
});

nv_reg!(NV_PFALCON_FALCON_BOOTVEC @ PFalconBase[0x00000104] {
    31:0    value => u32;
});

nv_reg!(NV_PFALCON_FALCON_DMACTL @ PFalconBase[0x0000010c] {
    0:0     require_ctx => bool;
    1:1     dmem_scrubbing => bool;
    2:2     imem_scrubbing => bool;
    6:3     dmaq_num;
    7:7     secure_stat => bool;
});

nv_reg!(NV_PFALCON_FALCON_DMATRFBASE @ PFalconBase[0x00000110] {
    31:0    base => u32;
});

nv_reg!(NV_PFALCON_FALCON_DMATRFMOFFS @ PFalconBase[0x00000114] {
    23:0    offs;
});

nv_reg!(NV_PFALCON_FALCON_DMATRFCMD @ PFalconBase[0x00000118] {
    0:0     full => bool;
    1:1     idle => bool;
    3:2     sec;
    4:4     imem => bool;
    5:5     is_write => bool;
    10:8    size ?=> DmaTrfCmdSize;
    14:12   ctxdma;
    16:16   set_dmtag;
});

nv_reg!(NV_PFALCON_FALCON_DMATRFFBOFFS @ PFalconBase[0x0000011c] {
    31:0    offs => u32;
});

nv_reg!(NV_PFALCON_FALCON_DMATRFBASE1 @ PFalconBase[0x00000128] {
    8:0     base;
});

nv_reg!(NV_PFALCON_FALCON_HWCFG1 @ PFalconBase[0x0000012c] {
    3:0     core_rev ?=> FalconCoreRev, "Core revision";
    5:4     security_model ?=> FalconSecurityModel, "Security model";
    7:6     core_rev_subversion => FalconCoreRevSubversion, "Core revision subversion";
});

nv_reg!(NV_PFALCON_FALCON_CPUCTL_ALIAS @ PFalconBase[0x00000130] {
    1:1     startcpu => bool;
});

// Actually known as `NV_PSEC_FALCON_ENGINE` and `NV_PGSP_FALCON_ENGINE` depending on the falcon
// instance.
nv_reg!(NV_PFALCON_FALCON_ENGINE @ PFalconBase[0x000003c0] {
    0:0     reset => bool;
});

nv_reg!(NV_PFALCON_FBIF_TRANSCFG @ PFalconBase[0x00000600[8]] {
    1:0     target ?=> FalconFbifTarget;
    2:2     mem_type => FalconFbifMemType;
});

nv_reg!(NV_PFALCON_FBIF_CTL @ PFalconBase[0x00000624] {
    7:7     allow_phys_no_ctx => bool;
});

/* PFALCON2 */

nv_reg!(NV_PFALCON2_FALCON_MOD_SEL @ PFalcon2Base[0x00000180] {
    7:0     algo ?=> FalconModSelAlgo;
});

nv_reg!(NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID @ PFalcon2Base[0x00000198] {
    7:0    ucode_id => u8;
});

nv_reg!(NV_PFALCON2_FALCON_BROM_ENGIDMASK @ PFalcon2Base[0x0000019c] {
    31:0    value => u32;
});

// OpenRM defines this as a register array, but doesn't specify its size and only uses its first
// element. Be conservative until we know the actual size or need to use more registers.
nv_reg!(NV_PFALCON2_FALCON_BROM_PARAADDR @ PFalcon2Base[0x00000210[1]] {
    31:0    value => u32;
});

// PRISCV

nv_reg!(NV_PRISCV_RISCV_CPUCTL @ PFalcon2Base[0x00000388] {
    0:0     halted => bool;
    7:7     active_stat => bool;
});

nv_reg!(NV_PRISCV_RISCV_BCR_CTRL @ PFalconBase[0x00001668] {
    0:0     valid => bool;
    4:4     core_select => PeregrineCoreSelect;
    8:8     br_fetch => bool;
});

// The modules below provide registers that are not identical on all supported chips. They should
// only be used in HAL modules.

pub(crate) mod gm107 {
    // FUSE

    nv_reg!(NV_FUSE_STATUS_OPT_DISPLAY @ 0x00021c04 {
        0:0     display_disabled => bool;
    });
}

pub(crate) mod ga100 {
    // FUSE

    nv_reg!(NV_FUSE_STATUS_OPT_DISPLAY @ 0x00820c04 {
        0:0     display_disabled => bool;
    });
}
