// SPDX-License-Identifier: GPL-2.0

use kernel::io::register;

// FSP (Foundation Security Processor) queue registers for Hopper/Blackwell Chain of Trust.
// These registers manage falcon EMEM communication queues.

register! {
    pub(super) NV_PFSP_QUEUE_HEAD(u32)[8] @ 0x008f2c00 {
        31:0    address => u32;
    }

    pub(super) NV_PFSP_QUEUE_TAIL(u32)[8] @ 0x008f2c04 {
        31:0    address => u32;
    }

    pub(super) NV_PFSP_MSGQ_HEAD(u32)[8] @ 0x008f2c80 {
        31:0    val => u32;
    }

    pub(super) NV_PFSP_MSGQ_TAIL(u32)[8] @ 0x008f2c84 {
        31:0    val => u32;
    }
}

// PFALCON registers are defined in the root `regs.rs` but are part of the falcon
// interface, accessed by the whole falcon module. They are re-exported here so
// falcon code can use a single `regs::` prefix.
// Once the PFALCON family moves out of the root module, these re-exports become
// plain definitions.
pub(super) use crate::regs::{
    NV_PFALCON_FALCON_BOOTVEC, NV_PFALCON_FALCON_CPUCTL, NV_PFALCON_FALCON_CPUCTL_ALIAS,
    NV_PFALCON_FALCON_DMACTL, NV_PFALCON_FALCON_DMATRFBASE, NV_PFALCON_FALCON_DMATRFBASE1,
    NV_PFALCON_FALCON_DMATRFCMD, NV_PFALCON_FALCON_DMATRFFBOFFS, NV_PFALCON_FALCON_DMATRFMOFFS,
    NV_PFALCON_FALCON_DMEMC, NV_PFALCON_FALCON_DMEMD, NV_PFALCON_FALCON_EMEMC,
    NV_PFALCON_FALCON_EMEMD, NV_PFALCON_FALCON_IMEMC, NV_PFALCON_FALCON_IMEMD,
    NV_PFALCON_FALCON_IMEMT, NV_PFALCON_FALCON_MAILBOX0, NV_PFALCON_FALCON_MAILBOX1,
    NV_PFALCON_FALCON_OS, NV_PFALCON_FALCON_RM, NV_PFALCON_FBIF_CTL, NV_PFALCON_FBIF_TRANSCFG,
};
