// SPDX-License-Identifier: GPL-2.0

//! GSP Sequencer implementation for Pre-hopper GSP boot sequence.

use core::mem::size_of;
use kernel::alloc::flags::GFP_KERNEL;
use kernel::device;
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

struct GspSequencerInfo<'a> {
    info: &'a fw::rpc_run_cpu_sequencer_v17_00,
    cmd_data: KVec<u8>,
}

/// GSP Sequencer Command types with payload data.
/// Commands have an opcode and a opcode-dependent struct.
#[allow(dead_code)]
pub(crate) enum GspSeqCmd {}

impl GspSeqCmd {
    /// Creates a new GspSeqCmd from a firmware GSP_SEQUENCER_BUFFER_CMD.
    pub(crate) fn from_fw_cmd(_cmd: &fw::GSP_SEQUENCER_BUFFER_CMD) -> Result<Self> {
        Err(EINVAL)
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
        0
    }
}

#[expect(dead_code)]
pub(crate) struct GspSequencer<'a> {
    seq_info: GspSequencerInfo<'a>,
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

impl GspSeqCmdRunner for GspSeqCmd {
    fn run(&self, _seq: &GspSequencer<'_>) -> Result {
        Ok(())
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
            total_cmds: self.seq_info.info.cmdIndex,
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
        cmdq.receive_msg_from_gsp(timeout, |info, mut sbuf| {
            let cmd_data = sbuf.flush_into_kvec(GFP_KERNEL)?;
            let seq_info = GspSequencerInfo { info, cmd_data };

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
                            sequencer.seq_info.info.cmdIndex
                        );
                        return Err(e);
                    }
                }
            }

            dev_dbg!(params.dev, "CPU Sequencer commands completed successfully");
            Ok(())
        })
    }
}
