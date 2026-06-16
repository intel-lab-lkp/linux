// SPDX-License-Identifier: GPL-2.0

use kernel::io::register;

use crate::falcon::{
    PFalcon2Base,
    FalconModSelAlgo,
    PeregrineCoreSelect, //
};

/* PFALCON2 */

register! {
    pub(super) NV_PFALCON2_FALCON_MOD_SEL(u32) @ PFalcon2Base + 0x00000180 {
        7:0     algo ?=> FalconModSelAlgo;
    }

    pub(super) NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID(u32) @ PFalcon2Base + 0x00000198 {
        7:0    ucode_id => u8;
    }

    pub(super) NV_PFALCON2_FALCON_BROM_ENGIDMASK(u32) @ PFalcon2Base + 0x0000019c {
        31:0    value => u32;
    }

    /// OpenRM defines this as a register array, but doesn't specify its size and only uses its
    /// first element. Be conservative until we know the actual size or need to use more registers.
    pub(super) NV_PFALCON2_FALCON_BROM_PARAADDR(u32)[1] @ PFalcon2Base + 0x00000210 {
        31:0    value => u32;
    }
}

// PRISCV

register! {
    /// RISC-V status register for debug (Turing and GA100 only).
    /// Reflects current RISC-V core status.
    pub(super) NV_PRISCV_RISCV_CORE_SWITCH_RISCV_STATUS(u32) @ PFalcon2Base + 0x00000240 {
        /// RISC-V core active/inactive status.
        0:0     active_stat => bool;
    }

    /// GA102 and later.
    pub(super) NV_PRISCV_RISCV_CPUCTL(u32) @ PFalcon2Base + 0x00000388 {
        7:7     active_stat => bool;
        0:0     halted => bool;
    }

    /// GA102 and later.
    pub(super) NV_PRISCV_RISCV_BCR_CTRL(u32) @ PFalcon2Base + 0x00000668 {
        8:8     br_fetch => bool;
        4:4     core_select => PeregrineCoreSelect;
        0:0     valid => bool;
    }
}
