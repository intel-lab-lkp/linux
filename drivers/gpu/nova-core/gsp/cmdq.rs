// SPDX-License-Identifier: GPL-2.0

use core::cmp;
use core::mem::offset_of;
use core::sync::atomic::fence;
use core::sync::atomic::Ordering;

use kernel::device;
use kernel::dma::{CoherentAllocation, DmaAddress};
use kernel::dma_write;
use kernel::io::poll::read_poll_timeout;
use kernel::prelude::*;
use kernel::slice::AsFlattened;
use kernel::sync::aref::ARef;
use kernel::time::Delta;
use kernel::transmute::{AsBytes, FromBytes};

use crate::driver::Bar0;
use crate::gsp::fw::{GspMsgElement, MsgFunction, MsgqRxHeader, MsgqTxHeader};
use crate::gsp::PteArray;
use crate::gsp::{GSP_PAGE_SHIFT, GSP_PAGE_SIZE};
use crate::num;
use crate::regs;
use crate::sbuffer::SBufferIter;

// Base trait for commands sent to the GSP. Commands always have a function associated with them
// but may or may not have a payload.
pub(crate) trait CommandToGspBase: Sized + FromBytes + AsBytes {
    const FUNCTION: MsgFunction;
}

// Trait for commands which do not require a variable-length payload.
pub(crate) trait CommandToGsp: CommandToGspBase {}

// Trait for commands which require a variable-length payload.
pub(crate) trait CommandToGspWithPayload: CommandToGspBase {}

// Trait for messages received from the GSP.
pub(crate) trait MessageFromGsp: Sized + FromBytes + AsBytes {
    const FUNCTION: MsgFunction;
}

/// Number of GSP pages making the [`Msgq`].
pub(crate) const MSGQ_NUM_PAGES: u32 = 0x3f;

#[repr(C, align(0x1000))]
#[derive(Debug)]
struct MsgqData {
    data: [[u8; GSP_PAGE_SIZE]; num::u32_as_usize(MSGQ_NUM_PAGES)],
}

// Annoyingly there is no real equivalent of #define so we're forced to use a
// literal to specify the alignment above. So check that against the actual GSP
// page size here.
static_assert!(align_of::<MsgqData>() == GSP_PAGE_SIZE);

// There is no struct defined for this in the open-gpu-kernel-source headers.
// Instead it is defined by code in GspMsgQueuesInit().
#[repr(C)]
struct Msgq {
    tx: MsgqTxHeader,
    rx: MsgqRxHeader,
    msgq: MsgqData,
}

#[repr(C)]
struct GspMem {
    ptes: PteArray<{ GSP_PAGE_SIZE / size_of::<u64>() }>,
    cpuq: Msgq,
    gspq: Msgq,
}

// SAFETY: These structs don't meet the no-padding requirements of AsBytes but
// that is not a problem because they are not used outside the kernel.
unsafe impl AsBytes for GspMem {}

// SAFETY: These structs don't meet the no-padding requirements of FromBytes but
// that is not a problem because they are not used outside the kernel.
unsafe impl FromBytes for GspMem {}

// `GspMem` struct that is shared with the GSP. This contains the shared memory
// region that both the host CPU and GSP read and write commands and messages
// to.
struct DmaGspMem(CoherentAllocation<GspMem>);

impl DmaGspMem {
    fn new(dev: &device::Device<device::Bound>) -> Result<Self> {
        const MSGQ_SIZE: u32 = num::usize_into_u32::<{ size_of::<Msgq>() }>();
        const RX_HDR_OFF: u32 = num::usize_into_u32::<{ offset_of!(Msgq, rx) }>();

        let gsp_mem =
            CoherentAllocation::<GspMem>::alloc_coherent(dev, 1, GFP_KERNEL | __GFP_ZERO)?;
        dma_write!(gsp_mem[0].ptes = PteArray::new(gsp_mem.dma_handle())?)?;
        dma_write!(gsp_mem[0].cpuq.tx = MsgqTxHeader::new(MSGQ_SIZE, RX_HDR_OFF, MSGQ_NUM_PAGES))?;
        dma_write!(gsp_mem[0].cpuq.rx = MsgqRxHeader::new())?;

        Ok(Self(gsp_mem))
    }

    /// Allocates the various regions for the command and reduces the payload size
    /// to match what is needed for the command.
    ///
    /// Returns a tuple with a reference to the GspMsgElement header, the command
    /// struct and two slices to contain the payload if required. The second
    /// payload slice may be zero length if the ring buffer didn't need to wrap
    /// to contain the command.
    ///
    /// # Errors
    ///
    /// Returns `Err(EAGAIN)` if the driver area is too small to hold the
    /// requested command.
    fn allocate_command_regions<'a, M: CommandToGspBase>(
        &'a mut self,
        payload_size: usize,
    ) -> Result<(&'a mut GspMsgElement, &'a mut M, &'a mut [u8], &'a mut [u8])> {
        // Allocate a region from the shared memory area to write our command
        // and payload to.
        let driver_area = self.driver_write_area();
        let msg_size = size_of::<GspMsgElement>() + size_of::<M>() + payload_size;
        let driver_area_size = (driver_area.0.len() + driver_area.1.len()) << GSP_PAGE_SHIFT;

        // If the GSP is still processing previous messages the shared region
        // may be full in which case we will have to retry once the GSP has
        // processed the existing commands.
        if msg_size > driver_area_size {
            return Err(EAGAIN);
        }

        // Split the memory region into an area for the command header and
        // struct + payload.
        #[allow(clippy::incompatible_msrv)]
        let (msg_header_slice, slice_1) = driver_area
            .0
            .as_flattened_slice_mut()
            .split_at_mut(size_of::<GspMsgElement>());
        let msg_header = GspMsgElement::from_bytes_mut(msg_header_slice).ok_or(EINVAL)?;

        // Split the remaining region into command struct and possible payload.
        let (cmd_slice, payload_1) = slice_1.split_at_mut(size_of::<M>());
        let cmd = M::from_bytes_mut(cmd_slice).ok_or(EINVAL)?;

        #[allow(clippy::incompatible_msrv)]
        let payload_2 = driver_area.1.as_flattened_slice_mut();

        // Create the payload area
        let (payload_1, payload_2) = if payload_1.len() > payload_size {
            // Payload fits entirely in payload_1
            (&mut payload_1[..payload_size], &mut payload_2[0..0])
        } else {
            // Need all of payload_1 and some of payload_2
            let payload_2_len = payload_size - payload_1.len();
            (payload_1, &mut payload_2[..payload_2_len])
        };

        Ok((msg_header, cmd, payload_1, payload_2))
    }

    // Returns a region of shared memory for the driver to write to. As this
    // region is a circular buffer it may be discontiguous in memory. In that
    // case the second slice will have a non-zero length.
    fn driver_write_area(&mut self) -> (&mut [[u8; GSP_PAGE_SIZE]], &mut [[u8; GSP_PAGE_SIZE]]) {
        let tx = self.cpu_write_ptr() as usize;
        let rx = self.gsp_read_ptr() as usize;

        // SAFETY:
        // - The [`CoherentAllocation`] contains exactly one object.
        // - We will only access the driver-owned part of the shared memory.
        // - Per the safety statement of the function, no concurrent access will be performed.
        let gsp_mem = &mut unsafe { self.0.as_slice_mut(0, 1) }.unwrap()[0];
        let (before_tx, after_tx) = gsp_mem.cpuq.msgq.data.split_at_mut(tx);

        if rx <= tx {
            // The area from `tx` up to the end of the ring, and from the beginning of the ring up
            // to `rx`, minus one unit, belongs to the driver.
            if rx == 0 {
                let last = after_tx.len() - 1;
                (&mut after_tx[..last], &mut before_tx[0..0])
            } else {
                (after_tx, &mut before_tx[..rx])
            }
        } else {
            // The area from `tx` to `rx`, minus one unit, belongs to the driver.
            (after_tx.split_at_mut(rx - tx).0, &mut before_tx[0..0])
        }
    }

    // Returns a region of shared memory for the driver to read from. As this
    // region is a circular buffer it may be discontiguous in memory. In that
    // case the second slice will have a non-zero length.
    fn driver_read_area(&self) -> (&[[u8; GSP_PAGE_SIZE]], &[[u8; GSP_PAGE_SIZE]]) {
        let tx = self.gsp_write_ptr() as usize;
        let rx = self.cpu_read_ptr() as usize;

        // SAFETY:
        // - The [`CoherentAllocation`] contains exactly one object.
        // - We will only access the driver-owned part of the shared memory.
        // - Per the safety statement of the function, no concurrent access will be performed.
        let gsp_mem = &unsafe { self.0.as_slice(0, 1) }.unwrap()[0];
        let (before_rx, after_rx) = gsp_mem.gspq.msgq.data.split_at(rx);

        match tx.cmp(&rx) {
            cmp::Ordering::Equal => (&after_rx[0..0], &after_rx[0..0]),
            cmp::Ordering::Greater => (&after_rx[..tx], &before_rx[0..0]),
            cmp::Ordering::Less => (after_rx, &before_rx[..tx]),
        }
    }

    // Return the index the GSP will write the next message to.
    fn gsp_write_ptr(&self) -> u32 {
        let gsp_mem = self.0.start_ptr();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        (unsafe { (*gsp_mem).gspq.tx.write_ptr() } % MSGQ_NUM_PAGES)
    }

    // Return the index the GSP will read the next command from.
    fn gsp_read_ptr(&self) -> u32 {
        let gsp_mem = self.0.start_ptr();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        (unsafe { (*gsp_mem).gspq.rx.read_ptr() } % MSGQ_NUM_PAGES)
    }

    // Return the index the CPU should start reading the next message from.
    fn cpu_read_ptr(&self) -> u32 {
        let gsp_mem = self.0.start_ptr();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        (unsafe { (*gsp_mem).cpuq.rx.read_ptr() } % MSGQ_NUM_PAGES)
    }

    // Inform the GSP that it can send `elem_count` new pages into the message queue.
    fn advance_cpu_read_ptr(&mut self, elem_count: u32) {
        // let gsp_mem = &self.0;
        let rptr = self.cpu_read_ptr().wrapping_add(elem_count) % MSGQ_NUM_PAGES;

        // Ensure read pointer is properly ordered
        fence(Ordering::SeqCst);

        let gsp_mem = self.0.start_ptr_mut();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        unsafe { (*gsp_mem).cpuq.rx.set_read_ptr(rptr) };
    }

    // Return the index the CPU should start writing the next command.
    fn cpu_write_ptr(&self) -> u32 {
        let gsp_mem = self.0.start_ptr();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        (unsafe { (*gsp_mem).cpuq.tx.write_ptr() } % MSGQ_NUM_PAGES)
    }

    // Inform the GSP that it can process `elem_count` new pages from the command queue.
    fn advance_cpu_write_ptr(&mut self, elem_count: u32) {
        let wptr = self.cpu_write_ptr().wrapping_add(elem_count) & MSGQ_NUM_PAGES;
        let gsp_mem = self.0.start_ptr_mut();

        // SAFETY:
        //  - The ['CoherentAllocation'] contains at least one object.
        //  - By the invariants of CoherentAllocation the pointer is valid.
        unsafe { (*gsp_mem).cpuq.tx.set_write_ptr(wptr) };

        // Ensure all command data is visible before triggering the GSP read
        fence(Ordering::SeqCst);
    }
}

pub(crate) struct Cmdq {
    dev: ARef<device::Device>,
    seq: u32,
    gsp_mem: DmaGspMem,
}

impl Cmdq {
    /// Offset of the data after the PTEs.
    const POST_PTE_OFFSET: usize = core::mem::offset_of!(GspMem, cpuq);

    /// Offset of command queue ring buffer.
    pub(crate) const CMDQ_OFFSET: usize = core::mem::offset_of!(GspMem, cpuq)
        + core::mem::offset_of!(Msgq, msgq)
        - Self::POST_PTE_OFFSET;

    /// Offset of message queue ring buffer.
    pub(crate) const STATQ_OFFSET: usize = core::mem::offset_of!(GspMem, gspq)
        + core::mem::offset_of!(Msgq, msgq)
        - Self::POST_PTE_OFFSET;

    /// Number of page table entries for the GSP shared region.
    pub(crate) const NUM_PTES: usize = size_of::<GspMem>() >> GSP_PAGE_SHIFT;

    pub(crate) fn new(dev: &device::Device<device::Bound>) -> Result<Cmdq> {
        let gsp_mem = DmaGspMem::new(dev)?;

        Ok(Cmdq {
            dev: dev.into(),
            seq: 0,
            gsp_mem,
        })
    }

    fn calculate_checksum<T: Iterator<Item = u8>>(it: T) -> u32 {
        let sum64 = it
            .enumerate()
            .map(|(idx, byte)| (((idx % 8) * 8) as u32, byte))
            .fold(0, |acc, (rol, byte)| acc ^ u64::from(byte).rotate_left(rol));

        ((sum64 >> 32) as u32) ^ (sum64 as u32)
    }

    // Notify GSP that we have updated the command queue pointers.
    fn notify_gsp(bar: &Bar0) {
        regs::NV_PGSP_QUEUE_HEAD::default()
            .set_address(0)
            .write(bar);
    }

    pub(crate) fn send_gsp_command<M, E>(&mut self, bar: &Bar0, init: impl Init<M, E>) -> Result
    where
        M: CommandToGsp,
        // This allows all error types, including `Infallible`, to be used with `init`. Without
        // this we cannot use regular stack objects as `init` since their `Init` implementation
        // does not return any error.
        Error: From<E>,
    {
        self.send_gsp_command_base_with_payload(bar, 0, init, |_| Ok(()))
    }

    pub(crate) fn send_gsp_command_with_payload<M, E>(
        &mut self,
        bar: &Bar0,
        payload_size: usize,
        init: impl Init<M, E>,
        init_payload: impl FnOnce(SBufferIter<core::array::IntoIter<&mut [u8], 2>>) -> Result,
    ) -> Result
    where
        M: CommandToGspWithPayload,
        // This allows all error types, including `Infallible`, to be used with `init`. Without
        // this we cannot use regular stack objects as `init` since their `Init` implementation
        // does not return any error.
        Error: From<E>,
    {
        self.send_gsp_command_base_with_payload(bar, payload_size, init, init_payload)
    }

    fn send_gsp_command_base_with_payload<M, E>(
        &mut self,
        bar: &Bar0,
        payload_size: usize,
        init: impl Init<M, E>,
        init_payload: impl FnOnce(SBufferIter<core::array::IntoIter<&mut [u8], 2>>) -> Result,
    ) -> Result
    where
        M: CommandToGspBase,
        // This allows all error types, including `Infallible`, to be used with `init`. Without
        // this we cannot use regular stack objects as `init` since their `Init` implementation
        // does not return any error.
        Error: From<E>,
    {
        #[repr(C)]
        struct FullCommand<M> {
            hdr: GspMsgElement,
            cmd: M,
        }

        let (msg_header, cmd, payload_1, payload_2) =
            self.gsp_mem.allocate_command_regions::<M>(payload_size)?;

        let seq = self.seq;
        let initializer = try_init!(FullCommand {
            hdr <- GspMsgElement::init(seq, size_of::<M>() + payload_size, M::FUNCTION),
            cmd <- init,
        });

        // Fill the header and command in-place.
        // SAFETY:
        //  - allocate_command_regions guarantees msg_header points to enough
        //    space in the command queue to contain FullCommand
        //  - __init ensures the command header and struct a fully initialized
        unsafe {
            initializer.__init(msg_header.as_bytes_mut().as_mut_ptr().cast())?;
        }

        // Fill the payload
        let sbuffer = SBufferIter::new_writer([&mut payload_1[..], &mut payload_2[..]]);
        init_payload(sbuffer)?;

        msg_header.set_checksum(Cmdq::calculate_checksum(SBufferIter::new_reader([
            msg_header.as_bytes(),
            cmd.as_bytes(),
            payload_1,
            payload_2,
        ])));

        dev_info!(
            &self.dev,
            "GSP RPC: send: seq# {}, function={:?}, length=0x{:x}\n",
            self.seq,
            msg_header.function(),
            msg_header.length(),
        );

        let elem_count = msg_header.element_count();
        self.seq += 1;
        self.gsp_mem.advance_cpu_write_ptr(elem_count);
        Cmdq::notify_gsp(bar);

        Ok(())
    }

    pub(crate) fn receive_msg_from_gsp<M: MessageFromGsp, R>(
        &mut self,
        timeout: Delta,
        init: impl FnOnce(&M, SBufferIter<core::array::IntoIter<&[u8], 2>>) -> Result<R>,
    ) -> Result<R> {
        // Wait for a message to arrive from the GSP.
        let driver_area = read_poll_timeout(
            || Ok(self.gsp_mem.driver_read_area()),
            |driver_area: &(&[[u8; 4096]], &[[u8; 4096]])| !driver_area.0.is_empty(),
            Delta::from_millis(10),
            timeout,
        )?;

        // Get references to the entire memory region available for the driver
        // to read.
        #[allow(clippy::incompatible_msrv)]
        let (msg_header_slice, slice_1) = driver_area
            .0
            .as_flattened_slice()
            .split_at(size_of::<GspMsgElement>());
        let msg_header = GspMsgElement::from_bytes(msg_header_slice).ok_or(EIO)?;
        if msg_header.length() < size_of::<M>() {
            return Err(EIO);
        }

        // Get message function.
        let function = msg_header.function().map_err(|bad_function| {
            dev_info!(
                self.dev,
                "GSP RPC: receive: seq# {}, bad function=0x{:x}, length=0x{:x}\n",
                msg_header.sequence(),
                bad_function,
                msg_header.length(),
            );
            EIO
        })?;

        // Log RPC receive with message type decoding.
        dev_info!(
            self.dev,
            "GSP RPC: receive: seq# {}, function={}, length=0x{:x}\n",
            msg_header.sequence(),
            function,
            msg_header.length(),
        );

        let (cmd_slice, payload_1) = slice_1.split_at(size_of::<M>());
        #[allow(clippy::incompatible_msrv)]
        let payload_2 = driver_area.1.as_flattened_slice();

        // `driver_read_area` returns the entire region available to be read,
        // not just the area containing the message. So we need to cut the
        // payload slice(s) down to the actual length of the payload.
        let (cmd_payload_1, cmd_payload_2) =
            if payload_1.len() > msg_header.length() - size_of::<M>() {
                (
                    payload_1.split_at(msg_header.length() - size_of::<M>()).0,
                    &payload_2[0..0],
                )
            } else {
                (
                    payload_1,
                    payload_2
                        .split_at(msg_header.length() - size_of::<M>() - payload_1.len())
                        .0,
                )
            };

        if Cmdq::calculate_checksum(SBufferIter::new_reader([
            msg_header.as_bytes(),
            cmd_slice,
            cmd_payload_1,
            cmd_payload_2,
        ])) != 0
        {
            dev_err!(
                self.dev,
                "GSP RPC: receive: Call {} - bad checksum",
                msg_header.sequence()
            );
            return Err(EIO);
        }

        // Extract the command struct and payload if present.
        let result = if function == M::FUNCTION {
            let cmd = M::from_bytes(cmd_slice).ok_or(EINVAL)?;
            let sbuffer = SBufferIter::new_reader([cmd_payload_1, cmd_payload_2]);
            init(cmd, sbuffer)
        } else {
            Err(ERANGE)
        };

        self.gsp_mem
            .advance_cpu_read_ptr(u32::try_from(msg_header.length().div_ceil(GSP_PAGE_SIZE))?);
        result
    }

    pub(crate) fn dma_handle(&self) -> DmaAddress {
        self.gsp_mem.0.dma_handle()
    }
}
