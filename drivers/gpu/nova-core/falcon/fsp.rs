// SPDX-License-Identifier: GPL-2.0
// SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

//! FSP (Foundation Security Processor) falcon engine for Hopper/Blackwell GPUs.
//!
//! The FSP falcon handles secure boot and Chain of Trust operations
//! on Hopper and Blackwell architectures, replacing SEC2's role.

use kernel::{
    io::{
        poll::read_poll_timeout,
        register::{
            Array,
            RegisterBase,
            WithBase, //
        },
        Io, //
    },
    prelude::*,
    time::Delta,
};

use crate::{
    driver::Bar0,
    falcon::{
        Falcon,
        FalconEngine,
        PFalcon2Base,
        PFalconBase, //
    },
    num,
    regs, //
};

/// FSP message timeout in milliseconds.
const FSP_MSG_TIMEOUT_MS: i64 = 2000;

/// Size of the FSP EMEM channel 0 that we can use.
const FSP_EMEM_CHANNEL_0_SIZE: usize = 1024;

/// Type specifying the `Fsp` falcon engine. Cannot be instantiated.
pub(crate) struct Fsp(());

impl RegisterBase<PFalconBase> for Fsp {
    const BASE: usize = 0x8f2000;
}

impl RegisterBase<PFalcon2Base> for Fsp {
    const BASE: usize = 0x8f3000;
}

impl FalconEngine for Fsp {}

impl Falcon<Fsp> {
    /// Writes `data` to FSP external memory at offset `0`.
    ///
    /// `data` is interpreted as little-endian 32-bit words. Returns `EINVAL`
    /// if the `data` length is not 4-byte aligned.
    fn write_emem(&mut self, bar: Bar0<'_>, data: &[u8]) -> Result {
        if data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        // Begin a write burst at offset `0`, auto-incrementing on each write.
        bar.write(
            WithBase::of::<Fsp>(),
            regs::NV_PFALCON_FALCON_EMEMC::zeroed().with_aincw(true),
        );

        for chunk in data.chunks_exact(4) {
            let value = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);

            // Write the next 32-bit `value`; hardware advances the offset.
            bar.write(
                WithBase::of::<Fsp>(),
                regs::NV_PFALCON_FALCON_EMEMD::zeroed().with_data(value),
            );
        }

        Ok(())
    }

    /// Reads FSP external memory from offset `0` into `data`.
    ///
    /// `data` is stored as little-endian 32-bit words. Returns `EINVAL` if
    /// the `data` length is not 4-byte aligned.
    fn read_emem(&mut self, bar: Bar0<'_>, data: &mut [u8]) -> Result {
        if data.len() % 4 != 0 {
            return Err(EINVAL);
        }

        // Begin a read burst at offset `0`, auto-incrementing on each read.
        bar.write(
            WithBase::of::<Fsp>(),
            regs::NV_PFALCON_FALCON_EMEMC::zeroed().with_aincr(true),
        );

        for chunk in data.chunks_exact_mut(4) {
            // Read the next 32-bit word; hardware advances the offset.
            let value = bar.read(regs::NV_PFALCON_FALCON_EMEMD::of::<Fsp>()).data();
            chunk.copy_from_slice(&value.to_le_bytes());
        }

        Ok(())
    }

    /// Poll FSP for incoming data.
    ///
    /// Returns the size of available data in bytes, or 0 if no data is available.
    /// Returns an error if the queue pointers are bogus (`tail < head`).
    ///
    /// The FSP message queue is not circular. Pointers are reset to 0 after each
    /// message exchange, so `tail >= head` is always true when data is present.
    fn poll_msgq(&self, bar: Bar0<'_>) -> Result<u32> {
        let head = bar.read(regs::NV_PFSP_MSGQ_HEAD::at(0)).val();
        let tail = bar.read(regs::NV_PFSP_MSGQ_TAIL::at(0)).val();

        if head == tail {
            Ok(0)
        } else {
            // TAIL points at the last DWORD written, so the size is `tail - head + 4`.
            tail.checked_sub(head)
                .and_then(|delta| delta.checked_add(4))
                .ok_or(EIO)
        }
    }

    /// Both the kernel driver and GSP talk to FSP. Try to ensure exclusive access to the FSP is
    /// enforced by making sure there is not a pending message already sent to FSP, and that there
    /// is no pending message from FSP to be read.
    fn wait_until_ready(&mut self, bar: Bar0<'_>) -> Result {
        read_poll_timeout(
            || {
                let qhead = bar.read(regs::NV_PFSP_QUEUE_HEAD::at(0)).address();
                let qtail = bar.read(regs::NV_PFSP_QUEUE_TAIL::at(0)).address();
                let mhead = bar.read(regs::NV_PFSP_MSGQ_HEAD::at(0)).val();
                let mtail = bar.read(regs::NV_PFSP_MSGQ_TAIL::at(0)).val();

                Ok(qhead == qtail && mhead == mtail)
            },
            |&ready| ready,
            Delta::from_millis(10),
            Delta::from_millis(FSP_MSG_TIMEOUT_MS),
        )?;
        Ok(())
    }

    /// Writes `packet` to FSP EMEM and updates the queue pointers to notify FSP.
    ///
    /// Returns `EINVAL` if `packet` is empty or its length is not 4-byte aligned.
    pub(crate) fn send_msg(&mut self, bar: Bar0<'_>, packet: &[u8]) -> Result {
        if packet.is_empty() {
            return Err(EINVAL);
        }

        // Try to make sure we have exclusive access to the FSP at this point.
        self.wait_until_ready(bar)?;

        self.write_emem(bar, packet)?;

        // Update queue pointers. TAIL points at the last DWORD written.
        let tail_offset = u32::try_from(packet.len() - 4).map_err(|_| EINVAL)?;
        bar.write(
            Array::at(0),
            regs::NV_PFSP_QUEUE_TAIL::zeroed().with_address(tail_offset),
        );
        bar.write(
            Array::at(0),
            regs::NV_PFSP_QUEUE_HEAD::zeroed().with_address(0),
        );

        Ok(())
    }

    /// Reads the next message from FSP EMEM into a newly-allocated buffer and resets the queue
    /// pointers.
    ///
    /// Returns `ETIMEDOUT` if no message was available until timeout, or a regular error code if a
    /// memory allocation error occurred.
    pub(crate) fn recv_msg(&mut self, bar: Bar0<'_>) -> Result<KVec<u8>> {
        let result = (|| {
            let msg_size = read_poll_timeout(
                || self.poll_msgq(bar),
                |&size| size > 0,
                Delta::from_millis(10),
                Delta::from_millis(FSP_MSG_TIMEOUT_MS),
            )
            .map(num::u32_as_usize)?;

            // Don't blindly allocate more than the maximum we expect from FSP.
            if msg_size > FSP_EMEM_CHANNEL_0_SIZE {
                return Err(EIO);
            }

            let mut buffer = KVec::<u8>::new();
            buffer.resize(msg_size, 0, GFP_KERNEL)?;

            self.read_emem(bar, &mut buffer)?;

            Ok(buffer)
        })();

        // Reset the message queue pointers regardless of outcome.
        bar.write(Array::at(0), regs::NV_PFSP_MSGQ_TAIL::zeroed().with_val(0));
        bar.write(Array::at(0), regs::NV_PFSP_MSGQ_HEAD::zeroed().with_val(0));

        result
    }
}
