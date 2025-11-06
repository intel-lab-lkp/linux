// SPDX-License-Identifier: GPL-2.0

//! GSP Sequencer implementation for Pre-hopper GSP boot sequence.

use core::mem::size_of;
use kernel::alloc::flags::GFP_KERNEL;
use kernel::bindings;
use kernel::device;
use kernel::io::poll::read_poll_timeout;
use kernel::prelude::*;
use kernel::time::Delta;
use kernel::transmute::FromBytes;

use crate::driver::Bar0;
use crate::falcon::{
    gsp::Gsp,
    sec2::Sec2,
    Falcon, //
};
use crate::firmware::gsp::GspFirmware;
use crate::gsp::cmdq::{
    Cmdq,
    MessageFromGsp, //
};
use crate::gsp::fw;

use kernel::{
    dev_dbg,
    dev_err, //
};

impl MessageFromGsp for fw::rpc_run_cpu_sequencer_v17_00 {
    const FUNCTION: fw::MsgFunction = fw::MsgFunction::GspRunCpuSequencer;
}

const CMD_SIZE: usize = size_of::<fw::GSP_SEQUENCER_BUFFER_CMD>();

struct GspSequencerInfo {
    cmd_index: u32,
    cmd_data: KVec<u8>,
}

/// GSP Sequencer Command types with payload data.
/// Commands have an opcode and a opcode-dependent struct.
#[allow(clippy::enum_variant_names)]
pub(crate) enum GspSeqCmd {
    RegWrite(fw::GSP_SEQ_BUF_PAYLOAD_REG_WRITE),
    RegModify(fw::GSP_SEQ_BUF_PAYLOAD_REG_MODIFY),
    RegPoll(fw::GSP_SEQ_BUF_PAYLOAD_REG_POLL),
    DelayUs(fw::GSP_SEQ_BUF_PAYLOAD_DELAY_US),
    RegStore(fw::GSP_SEQ_BUF_PAYLOAD_REG_STORE),
    CoreReset,
    CoreStart,
    CoreWaitForHalt,
    CoreResume,
}

impl GspSeqCmd {
    /// Creates a new GspSeqCmd from a firmware GSP_SEQUENCER_BUFFER_CMD.
    pub(crate) fn from_fw_cmd(cmd: &fw::GSP_SEQUENCER_BUFFER_CMD) -> Result<Self> {
        match cmd.opCode {
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_WRITE => {
                // SAFETY: We're using the union field that corresponds to the opCode.
                Ok(GspSeqCmd::RegWrite(unsafe { cmd.payload.regWrite }))
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_MODIFY => {
                // SAFETY: We're using the union field that corresponds to the opCode.
                Ok(GspSeqCmd::RegModify(unsafe { cmd.payload.regModify }))
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_POLL => {
                // SAFETY: We're using the union field that corresponds to the opCode.
                Ok(GspSeqCmd::RegPoll(unsafe { cmd.payload.regPoll }))
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_DELAY_US => {
                // SAFETY: We're using the union field that corresponds to the opCode.
                Ok(GspSeqCmd::DelayUs(unsafe { cmd.payload.delayUs }))
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_REG_STORE => {
                // SAFETY: We're using the union field that corresponds to the opCode.
                Ok(GspSeqCmd::RegStore(unsafe { cmd.payload.regStore }))
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESET => Ok(GspSeqCmd::CoreReset),
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_START => Ok(GspSeqCmd::CoreStart),
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT => {
                Ok(GspSeqCmd::CoreWaitForHalt)
            }
            fw::GSP_SEQ_BUF_OPCODE_GSP_SEQ_BUF_OPCODE_CORE_RESUME => Ok(GspSeqCmd::CoreResume),
            _ => Err(EINVAL),
        }
    }

    pub(crate) fn new(data: &[u8], dev: &device::Device<device::Bound>) -> Result<Self> {
        let fw_cmd = fw::GSP_SEQUENCER_BUFFER_CMD::from_bytes(data).ok_or(EINVAL)?;
        let cmd = Self::from_fw_cmd(fw_cmd)?;

        if data.len() < cmd.size_bytes() {
            dev_err!(dev, "data is not enough for command");
            return Err(EINVAL);
        }

        Ok(cmd)
    }

    /// Get the size of this command in bytes, the command consists of
    /// a 4-byte opcode, and a variable-sized payload.
    pub(crate) fn size_bytes(&self) -> usize {
        let opcode_size = size_of::<fw::GSP_SEQ_BUF_OPCODE>();
        match self {
            // Each simple command type just adds 4 bytes (opcode_size) for the header.
            GspSeqCmd::CoreReset
            | GspSeqCmd::CoreStart
            | GspSeqCmd::CoreWaitForHalt
            | GspSeqCmd::CoreResume => opcode_size,

            // For commands with payloads, add the payload size in bytes.
            GspSeqCmd::RegWrite(_) => opcode_size + size_of::<fw::GSP_SEQ_BUF_PAYLOAD_REG_WRITE>(),
            GspSeqCmd::RegModify(_) => {
                opcode_size + size_of::<fw::GSP_SEQ_BUF_PAYLOAD_REG_MODIFY>()
            }
            GspSeqCmd::RegPoll(_) => opcode_size + size_of::<fw::GSP_SEQ_BUF_PAYLOAD_REG_POLL>(),
            GspSeqCmd::DelayUs(_) => opcode_size + size_of::<fw::GSP_SEQ_BUF_PAYLOAD_DELAY_US>(),
            GspSeqCmd::RegStore(_) => opcode_size + size_of::<fw::GSP_SEQ_BUF_PAYLOAD_REG_STORE>(),
        }
    }
}

pub(crate) struct GspSequencer<'a> {
    seq_info: GspSequencerInfo,
    bar: &'a Bar0,
    sec2_falcon: &'a Falcon<Sec2>,
    gsp_falcon: &'a Falcon<Gsp>,
    libos_dma_handle: u64,
    gsp_fw: &'a GspFirmware,
    dev: &'a device::Device<device::Bound>,
}

pub(crate) trait GspSeqCmdRunner {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result;
}

impl GspSeqCmdRunner for fw::GSP_SEQ_BUF_PAYLOAD_REG_WRITE {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result {
        let addr = self.addr as usize;
        let val = self.val;
        let _ = sequencer.bar.try_write32(val, addr);
        Ok(())
    }
}

impl GspSeqCmdRunner for fw::GSP_SEQ_BUF_PAYLOAD_REG_MODIFY {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result {
        let addr = self.addr as usize;
        if let Ok(temp) = sequencer.bar.try_read32(addr) {
            let _ = sequencer
                .bar
                .try_write32((temp & !self.mask) | self.val, addr);
        }
        Ok(())
    }
}

impl GspSeqCmdRunner for fw::GSP_SEQ_BUF_PAYLOAD_REG_POLL {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result {
        let addr = self.addr as usize;
        let mut timeout_us = i64::from(self.timeout);

        // Default timeout to 4 seconds.
        timeout_us = if timeout_us == 0 { 4000000 } else { timeout_us };

        // First read.
        sequencer.bar.try_read32(addr)?;

        // Poll the requested register with requested timeout.
        read_poll_timeout(
            || sequencer.bar.try_read32(addr),
            |current| (current & self.mask) == self.val,
            Delta::ZERO,
            Delta::from_micros(timeout_us),
        )
        .map(|_| ())
    }
}

impl GspSeqCmdRunner for fw::GSP_SEQ_BUF_PAYLOAD_DELAY_US {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result {
        dev_dbg!(sequencer.dev, "DelayUs: val=0x{:x}\n", self.val);
        // SAFETY: `usleep_range_state` is safe to call with any parameter.
        unsafe {
            bindings::usleep_range_state(
                self.val as usize,
                self.val as usize,
                bindings::TASK_UNINTERRUPTIBLE,
            )
        };
        Ok(())
    }
}

impl GspSeqCmdRunner for fw::GSP_SEQ_BUF_PAYLOAD_REG_STORE {
    fn run(&self, sequencer: &GspSequencer<'_>) -> Result {
        let addr = self.addr as usize;
        let _index = self.index;

        let _val = sequencer.bar.try_read32(addr)?;

        Ok(())
    }
}

impl GspSeqCmdRunner for GspSeqCmd {
    fn run(&self, seq: &GspSequencer<'_>) -> Result {
        match self {
            GspSeqCmd::RegWrite(cmd) => cmd.run(seq),
            GspSeqCmd::RegModify(cmd) => cmd.run(seq),
            GspSeqCmd::RegPoll(cmd) => cmd.run(seq),
            GspSeqCmd::DelayUs(cmd) => cmd.run(seq),
            GspSeqCmd::RegStore(cmd) => cmd.run(seq),
            GspSeqCmd::CoreReset => {
                dev_dbg!(seq.dev, "CoreReset\n");
                seq.gsp_falcon.reset(seq.bar)?;
                seq.gsp_falcon.dma_reset(seq.bar);
                Ok(())
            }
            GspSeqCmd::CoreStart => {
                dev_dbg!(seq.dev, "CoreStart\n");
                seq.gsp_falcon.start(seq.bar)?;
                Ok(())
            }
            GspSeqCmd::CoreWaitForHalt => {
                dev_dbg!(seq.dev, "CoreWaitForHalt\n");
                seq.gsp_falcon.wait_till_halted(seq.bar)?;
                Ok(())
            }
            GspSeqCmd::CoreResume => {
                dev_dbg!(seq.dev, "CoreResume\n");
                // At this point, 'SEC2-RTOS' has been loaded into SEC2 by the sequencer
                // but neither SEC2-RTOS nor GSP-RM is running yet. This part of the
                // sequencer will start both.

                // Reset the GSP to prepare it for resuming.
                seq.gsp_falcon.reset(seq.bar)?;

                // Write the libOS DMA handle to GSP mailboxes.
                seq.gsp_falcon.write_mailboxes(
                    seq.bar,
                    Some(seq.libos_dma_handle as u32),
                    Some((seq.libos_dma_handle >> 32) as u32),
                )?;

                // Start the SEC2 falcon which will trigger GSP-RM to resume on the GSP.
                seq.sec2_falcon.start(seq.bar)?;

                // Poll until GSP-RM reload/resume has completed (up to 2 seconds).
                seq.gsp_falcon
                    .check_reload_completed(seq.bar, Delta::from_secs(2))?;

                // Verify SEC2 completed successfully by checking its mailbox for errors.
                let mbox0 = seq.sec2_falcon.read_mailbox0(seq.bar)?;
                if mbox0 != 0 {
                    dev_err!(seq.dev, "Sequencer: sec2 errors: {:?}\n", mbox0);
                    return Err(EIO);
                }

                // Configure GSP with the bootloader version.
                seq.gsp_falcon
                    .write_os_version(seq.bar, seq.gsp_fw.bootloader.app_version);

                // Verify the GSP's RISC-V core is active indicating successful GSP boot.
                if !seq.gsp_falcon.is_riscv_active(seq.bar) {
                    dev_err!(seq.dev, "Sequencer: RISC-V core is not active\n");
                    return Err(EIO);
                }
                Ok(())
            }
        }
    }
}

pub(crate) struct GspSeqIter<'a> {
    cmd_data: &'a [u8],
    current_offset: usize, // Tracking the current position.
    total_cmds: u32,
    cmds_processed: u32,
    dev: &'a device::Device<device::Bound>,
}

impl<'a> Iterator for GspSeqIter<'a> {
    type Item = Result<GspSeqCmd>;

    fn next(&mut self) -> Option<Self::Item> {
        // Stop if we've processed all commands or reached the end of data.
        if self.cmds_processed >= self.total_cmds || self.current_offset >= self.cmd_data.len() {
            return None;
        }

        // Check if we have enough data for opcode.
        let opcode_size = size_of::<fw::GSP_SEQ_BUF_OPCODE>();
        if self.current_offset + opcode_size > self.cmd_data.len() {
            return Some(Err(EINVAL));
        }

        let offset = self.current_offset;

        // Handle command creation based on available data,
        // zero-pad if necessary (since last command may not be full size).
        let mut buffer = [0u8; CMD_SIZE];
        let copy_len = if offset + CMD_SIZE <= self.cmd_data.len() {
            CMD_SIZE
        } else {
            self.cmd_data.len() - offset
        };
        buffer[..copy_len].copy_from_slice(&self.cmd_data[offset..offset + copy_len]);
        let cmd_result = GspSeqCmd::new(&buffer, self.dev);

        cmd_result.map_or_else(
            |_err| {
                dev_err!(self.dev, "Error parsing command at offset {}", offset);
                None
            },
            |cmd| {
                self.current_offset += cmd.size_bytes();
                self.cmds_processed += 1;
                Some(Ok(cmd))
            },
        )
    }
}

impl<'a, 'b> IntoIterator for &'b GspSequencer<'a> {
    type Item = Result<GspSeqCmd>;
    type IntoIter = GspSeqIter<'b>;

    fn into_iter(self) -> Self::IntoIter {
        let cmd_data = &self.seq_info.cmd_data[..];

        GspSeqIter {
            cmd_data,
            current_offset: 0,
            total_cmds: self.seq_info.cmd_index,
            cmds_processed: 0,
            dev: self.dev,
        }
    }
}

/// Parameters for running the GSP sequencer.
pub(crate) struct GspSequencerParams<'a> {
    pub(crate) gsp_fw: &'a GspFirmware,
    pub(crate) libos_dma_handle: u64,
    pub(crate) gsp_falcon: &'a Falcon<Gsp>,
    pub(crate) sec2_falcon: &'a Falcon<Sec2>,
    pub(crate) dev: &'a device::Device<device::Bound>,
    pub(crate) bar: &'a Bar0,
}

impl<'a> GspSequencer<'a> {
    pub(crate) fn run(cmdq: &mut Cmdq, params: GspSequencerParams<'a>, timeout: Delta) -> Result {
        let seq_info = loop {
            match cmdq.receive_msg_from_gsp(
                timeout,
                |info: &fw::rpc_run_cpu_sequencer_v17_00, mut sbuf| {
                    let cmd_data = sbuf.flush_into_kvec(GFP_KERNEL)?;
                    Ok(GspSequencerInfo {
                        cmd_index: info.cmdIndex,
                        cmd_data,
                    })
                },
            ) {
                Ok(seq_info) => break seq_info,
                Err(ERANGE) => continue,
                Err(e) => return Err(e),
            }
        };

        let sequencer = GspSequencer {
            seq_info,
            bar: params.bar,
            sec2_falcon: params.sec2_falcon,
            gsp_falcon: params.gsp_falcon,
            libos_dma_handle: params.libos_dma_handle,
            gsp_fw: params.gsp_fw,
            dev: params.dev,
        };

        dev_dbg!(params.dev, "Running CPU Sequencer commands");

        for cmd_result in &sequencer {
            match cmd_result {
                Ok(cmd) => cmd.run(&sequencer)?,
                Err(e) => {
                    dev_err!(
                        params.dev,
                        "Error running command at index {}",
                        sequencer.seq_info.cmd_index
                    );
                    return Err(e);
                }
            }
        }

        dev_dbg!(params.dev, "CPU Sequencer commands completed successfully");
        Ok(())
    }
}
