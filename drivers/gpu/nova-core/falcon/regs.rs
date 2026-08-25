// SPDX-License-Identifier: GPL-2.0

use kernel::{
    io::{
        register,
        register::WithBase,
        Io, //
    },
    time, //
};

use crate::{
    driver::Bar0,
    falcon::{
        DmaTrfCmdSize,
        FalconCoreRev,
        FalconCoreRevSubversion,
        FalconEngine,
        FalconFbifMemType,
        FalconFbifTarget,
        FalconMem,
        FalconModSelAlgo,
        FalconSecurityModel,
        PFalcon2Base,
        PFalconBase,
        PeregrineCoreSelect, //
    },
};

// PFALCON

register! {
    pub(super) NV_PFALCON_FALCON_IRQSCLR(u32) @ PFalconBase + 0x00000004 {
        6:6     swgen0 => bool;
        4:4     halt => bool;
    }

    pub(super) NV_PFALCON_FALCON_MAILBOX0(u32) @ PFalconBase + 0x00000040 {
        31:0    value => u32;
    }

    pub(super) NV_PFALCON_FALCON_MAILBOX1(u32) @ PFalconBase + 0x00000044 {
        31:0    value => u32;
    }

    /// Used to store version information about the firmware running
    /// on the Falcon processor.
    pub(super) NV_PFALCON_FALCON_OS(u32) @ PFalconBase + 0x00000080 {
        31:0    value => u32;
    }

    pub(super) NV_PFALCON_FALCON_RM(u32) @ PFalconBase + 0x00000084 {
        31:0    value => u32;
    }

    pub(super) NV_PFALCON_FALCON_HWCFG2(u32) @ PFalconBase + 0x000000f4 {
        /// Signal indicating that reset is completed (GA102+).
        31:31   reset_ready => bool;
        /// RISC-V branch privilege lockdown bit.
        13:13   riscv_br_priv_lockdown => bool;
        /// Set to 0 after memory scrubbing is completed.
        12:12   mem_scrubbing => bool;
        10:10   riscv => bool;
    }

    pub(super) NV_PFALCON_FALCON_CPUCTL(u32) @ PFalconBase + 0x00000100 {
        6:6     alias_en => bool;
        4:4     halted => bool;
        1:1     startcpu => bool;
    }

    pub(super) NV_PFALCON_FALCON_BOOTVEC(u32) @ PFalconBase + 0x00000104 {
        31:0    value => u32;
    }

    pub(super) NV_PFALCON_FALCON_DMACTL(u32) @ PFalconBase + 0x0000010c {
        7:7     secure_stat => bool;
        6:3     dmaq_num;
        2:2     imem_scrubbing => bool;
        1:1     dmem_scrubbing => bool;
        0:0     require_ctx => bool;
    }

    pub(super) NV_PFALCON_FALCON_DMATRFBASE(u32) @ PFalconBase + 0x00000110 {
        31:0    base => u32;
    }

    pub(super) NV_PFALCON_FALCON_DMATRFMOFFS(u32) @ PFalconBase + 0x00000114 {
        23:0    offs;
    }

    pub(super) NV_PFALCON_FALCON_DMATRFCMD(u32) @ PFalconBase + 0x00000118 {
        16:16   set_dmtag;
        14:12   ctxdma;
        10:8    size ?=> DmaTrfCmdSize;
        5:5     is_write => bool;
        4:4     imem => bool;
        3:2     sec;
        1:1     idle => bool;
        0:0     full => bool;
    }

    pub(super) NV_PFALCON_FALCON_DMATRFFBOFFS(u32) @ PFalconBase + 0x0000011c {
        31:0    offs => u32;
    }

    pub(super) NV_PFALCON_FALCON_DMATRFBASE1(u32) @ PFalconBase + 0x00000128 {
        8:0     base;
    }

    pub(super) NV_PFALCON_FALCON_HWCFG1(u32) @ PFalconBase + 0x0000012c {
        /// Core revision subversion.
        7:6     core_rev_subversion => FalconCoreRevSubversion;
        /// Security model.
        5:4     security_model ?=> FalconSecurityModel;
        /// Core revision.
        3:0     core_rev ?=> FalconCoreRev;
    }

    pub(super) NV_PFALCON_FALCON_CPUCTL_ALIAS(u32) @ PFalconBase + 0x00000130 {
        1:1     startcpu => bool;
    }

    /// IMEM access control register. Up to 4 ports are available for IMEM access.
    pub(super) NV_PFALCON_FALCON_IMEMC(u32)[4, stride = 16] @ PFalconBase + 0x00000180 {
        /// Access secure IMEM.
        28:28     secure => bool;
        /// Auto-increment on write.
        24:24     aincw => bool;
        /// IMEM block and word offset.
        15:0      offs;
    }

    /// IMEM data register. Reading/writing this register accesses IMEM at the address
    /// specified by the corresponding IMEMC register.
    pub(super) NV_PFALCON_FALCON_IMEMD(u32)[4, stride = 16] @ PFalconBase + 0x00000184 {
        31:0      data;
    }

    /// IMEM tag register. Used to set the tag for the current IMEM block.
    pub(super) NV_PFALCON_FALCON_IMEMT(u32)[4, stride = 16] @ PFalconBase + 0x00000188 {
        15:0      tag;
    }

    /// DMEM access control register. Up to 8 ports are available for DMEM access.
    pub(super) NV_PFALCON_FALCON_DMEMC(u32)[8, stride = 8] @ PFalconBase + 0x000001c0 {
        /// Auto-increment on write.
        24:24     aincw => bool;
        /// DMEM block and word offset.
        15:0      offs;
    }

    /// DMEM data register. Reading/writing this register accesses DMEM at the address
    /// specified by the corresponding DMEMC register.
    pub(super) NV_PFALCON_FALCON_DMEMD(u32)[8, stride = 8] @ PFalconBase + 0x000001c4 {
        31:0      data;
    }

    /// Actually known as `NV_PSEC_FALCON_ENGINE` and `NV_PGSP_FALCON_ENGINE` depending on the
    /// falcon instance.
    pub(super) NV_PFALCON_FALCON_ENGINE(u32) @ PFalconBase + 0x000003c0 {
        0:0     reset => bool;
    }

    pub(super) NV_PFALCON_FBIF_TRANSCFG(u32)[8] @ PFalconBase + 0x00000600 {
        2:2     mem_type => FalconFbifMemType;
        1:0     target ?=> FalconFbifTarget;
    }

    pub(super) NV_PFALCON_FBIF_CTL(u32) @ PFalconBase + 0x00000624 {
        7:7     allow_phys_no_ctx => bool;
    }

    // Falcon EMEM PIO registers (used by FSP on Hopper/Blackwell).
    // These provide the falcon external memory communication interface.

    pub(super) NV_PFALCON_FALCON_EMEMC(u32) @ PFalconBase + 0x00000ac0 {
        /// EMEM byte offset (4-byte aligned) within the block.
        7:2     offs;
        /// EMEM block to access.
        15:8    blk;
        /// Auto-increment the offset after each write.
        24:24   aincw => bool;
        /// Auto-increment the offset after each read.
        25:25   aincr => bool;
    }

    pub(super) NV_PFALCON_FALCON_EMEMD(u32) @ PFalconBase + 0x00000ac4 {
        31:0    data => u32;
    }
}

impl NV_PFALCON_FALCON_DMACTL {
    /// Returns `true` if memory scrubbing is completed.
    pub(super) fn mem_scrubbing_done(self) -> bool {
        !self.dmem_scrubbing() && !self.imem_scrubbing()
    }
}

impl NV_PFALCON_FALCON_DMATRFCMD {
    /// Programs the `imem` and `sec` fields for the given FalconMem
    pub(super) fn with_falcon_mem(self, mem: FalconMem) -> Self {
        let this = self.with_imem(mem != FalconMem::Dmem);

        match mem {
            FalconMem::ImemSecure => this.with_const_sec::<1>(),
            _ => this.with_const_sec::<0>(),
        }
    }
}

impl NV_PFALCON_FALCON_ENGINE {
    /// Resets the falcon
    pub(super) fn reset_engine<E: FalconEngine>(bar: Bar0<'_>) {
        bar.update(Self::of::<E>(), |r| r.with_reset(true));

        // TIMEOUT: falcon engine should not take more than 10us to reset.
        time::delay::fsleep(time::Delta::from_micros(10));

        bar.update(Self::of::<E>(), |r| r.with_reset(false));
    }
}

impl NV_PFALCON_FALCON_HWCFG2 {
    /// Returns `true` if memory scrubbing is completed.
    pub(super) fn mem_scrubbing_done(self) -> bool {
        !self.mem_scrubbing()
    }
}

// PFALCON2

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
        4:4     halted => bool;
    }

    /// GA102 and later.
    pub(super) NV_PRISCV_RISCV_BCR_CTRL(u32) @ PFalcon2Base + 0x00000668 {
        8:8     br_fetch => bool;
        4:4     core_select => PeregrineCoreSelect;
        0:0     valid => bool;
    }
}

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

// FUSE registers are defined in the root `regs.rs` but are part of the
// falcon interface, accessed by the whole falcon module. They are
// re-exported here so falcon code can use a single `regs::` prefix.
pub(super) use crate::regs::{
    NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION, NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION,
    NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION, NV_FUSE_OPT_FPF_SIZE,
};
