// SPDX-License-Identifier: GPL-2.0

use kernel::io::register;

use crate::falcon::{
    PFalcon2Base,
    FalconModSelAlgo, //
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
