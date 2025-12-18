// SPDX-License-Identifier: GPL-2.0

mod boot;

use kernel::{
    c_str,
    debugfs,
    device,
    dma::{
        CoherentAllocation,
        DmaAddress, //
    },
    dma_write,
    pci,
    prelude::*,
    transmute::AsBytes, //
};

pub(crate) mod cmdq;
pub(crate) mod commands;
mod fw;
mod sequencer;

pub(crate) use fw::{
    GspFwWprMeta,
    LibosParams, //
};

use crate::{
    gsp::cmdq::Cmdq,
    gsp::fw::{
        GspArgumentsCached,
        LibosMemoryRegionInitArgument, //
    },
    num,
};

pub(crate) const GSP_PAGE_SHIFT: usize = 12;
pub(crate) const GSP_PAGE_SIZE: usize = 1 << GSP_PAGE_SHIFT;

/// Number of GSP pages to use in a RM log buffer.
const RM_LOG_BUFFER_NUM_PAGES: usize = 0x10;

/// Array of page table entries, as understood by the GSP bootloader.
#[repr(C)]
struct PteArray<const NUM_ENTRIES: usize>([u64; NUM_ENTRIES]);

/// SAFETY: arrays of `u64` implement `AsBytes` and we are but a wrapper around one.
unsafe impl<const NUM_ENTRIES: usize> AsBytes for PteArray<NUM_ENTRIES> {}

impl<const NUM_PAGES: usize> PteArray<NUM_PAGES> {
    /// Creates a new page table array mapping `NUM_PAGES` GSP pages starting at address `start`.
    fn new(start: DmaAddress) -> Result<Self> {
        let mut ptes = [0u64; NUM_PAGES];
        for (i, pte) in ptes.iter_mut().enumerate() {
            *pte = start
                .checked_add(num::usize_as_u64(i) << GSP_PAGE_SHIFT)
                .ok_or(EOVERFLOW)?;
        }

        Ok(Self(ptes))
    }
}

/// The logging buffers are byte queues that contain encoded printf-like
/// messages from GSP-RM.  They need to be decoded by a special application
/// that can parse the buffers.
///
/// The 'loginit' buffer contains logs from early GSP-RM init and
/// exception dumps.  The 'logrm' buffer contains the subsequent logs. Both are
/// written to directly by GSP-RM and can be any multiple of GSP_PAGE_SIZE.
///
/// The physical address map for the log buffer is stored in the buffer
/// itself, starting with offset 1. Offset 0 contains the "put" pointer (pp).
/// Initially, pp is equal to 0. If the buffer has valid logging data in it,
/// then pp points to index into the buffer where the next logging entry will
/// be written. Therefore, the logging data is valid if:
///   1 <= pp < sizeof(buffer)/sizeof(u64)
struct LogBuffer(CoherentAllocation<u8>);

impl LogBuffer {
    /// Creates a new `LogBuffer` mapped on `dev`.
    fn new(dev: &device::Device<device::Bound>) -> Result<Self> {
        const NUM_PAGES: usize = RM_LOG_BUFFER_NUM_PAGES;

        let mut obj = Self(CoherentAllocation::<u8>::alloc_coherent(
            dev,
            NUM_PAGES * GSP_PAGE_SIZE,
            GFP_KERNEL | __GFP_ZERO,
        )?);
        let ptes = PteArray::<NUM_PAGES>::new(obj.0.dma_handle())?;

        // SAFETY: `obj` has just been created and we are its sole user.
        unsafe {
            // Copy the self-mapping PTE at the expected location.
            obj.0
                .as_slice_mut(size_of::<u64>(), size_of_val(&ptes))?
                .copy_from_slice(ptes.as_bytes())
        };

        Ok(obj)
    }
}

/// Log buffers used by GSP-RM for debug logging.
struct LogBuffers {
    /// Init log buffer.
    loginit: LogBuffer,
    /// Interrupts log buffer.
    logintr: LogBuffer,
    /// RM log buffer.
    logrm: LogBuffer,
}

/// GSP runtime data.
#[pin_data]
pub(crate) struct Gsp {
    /// Libos arguments.
    pub(crate) libos: CoherentAllocation<LibosMemoryRegionInitArgument>,
    /// Log buffers, optionally exposed via debugfs.
    #[pin]
    logs: debugfs::Scope<LogBuffers>,
    /// Command queue.
    pub(crate) cmdq: Cmdq,
    /// RM arguments.
    rmargs: CoherentAllocation<GspArgumentsCached>,
}

impl debugfs::BinaryWriter for LogBuffer {
    fn write_to_slice(
        &self,
        writer: &mut kernel::uaccess::UserSliceWriter,
        offset: &mut kernel::fs::file::Offset,
    ) -> Result<usize> {
        // SAFETY: This is a debug log buffer. GSP may write concurrently, so the
        // snapshot may contain partially-written entries. This is acceptable for
        // debugging purposes - users should be aware logs may be slightly garbled
        // if read while GSP is actively logging.
        let slice = unsafe { self.0.as_slice(0, self.0.count()) }?;

        writer.write_slice_file(slice, offset)
    }
}

// SAFETY: `LogBuffer` only provides shared access to the underlying `CoherentAllocation`.
// GSP may write to the buffer concurrently regardless of CPU access, so concurrent reads
// from multiple CPU threads do not introduce any additional races beyond what already
// exists with the device. Reads may observe partially-written log entries, which is
// acceptable for debug logging purposes.
unsafe impl Sync for LogBuffer {}

impl Gsp {
    // Creates an in-place initializer for a `Gsp` manager for `pdev`.
    pub(crate) fn new<'a>(
        pdev: &'a pci::Device<device::Bound>,
    ) -> Result<impl PinInit<Self, Error> + 'a> {
        let dev = pdev.as_ref();
        let libos = CoherentAllocation::<LibosMemoryRegionInitArgument>::alloc_coherent(
            dev,
            GSP_PAGE_SIZE / size_of::<LibosMemoryRegionInitArgument>(),
            GFP_KERNEL | __GFP_ZERO,
        )?;

        // Initialise the logging structures. The OpenRM equivalents are in:
        // _kgspInitLibosLoggingStructures (allocates memory for buffers)
        // kgspSetupLibosInitArgs_IMPL (creates pLibosInitArgs[] array)
        let loginit = LogBuffer::new(dev)?;
        dma_write!(libos[0] = LibosMemoryRegionInitArgument::new("LOGINIT", &loginit.0))?;

        let logintr = LogBuffer::new(dev)?;
        dma_write!(libos[1] = LibosMemoryRegionInitArgument::new("LOGINTR", &logintr.0))?;

        let logrm = LogBuffer::new(dev)?;
        dma_write!(libos[2] = LibosMemoryRegionInitArgument::new("LOGRM", &logrm.0))?;

        let cmdq = Cmdq::new(dev)?;

        let rmargs = CoherentAllocation::<GspArgumentsCached>::alloc_coherent(
            dev,
            1,
            GFP_KERNEL | __GFP_ZERO,
        )?;
        dma_write!(rmargs[0] = fw::GspArgumentsCached::new(&cmdq))?;
        dma_write!(libos[3] = LibosMemoryRegionInitArgument::new("RMARGS", &rmargs))?;

        let log_buffers = LogBuffers {
            loginit,
            logintr,
            logrm,
        };

        // Look up nova_core debugfs directory - only create entries if it exists.
        // If nova_core doesn't exist, debugfs_dir will be empty and file creation
        // becomes a no-op (data is still stored, just not exposed via debugfs).
        let debugfs_dir = debugfs::Dir::lookup(c_str!("nova_core"), None)
            .unwrap_or_else(debugfs::Dir::empty);

        let logs = debugfs_dir.scope(log_buffers, pdev.name(), |logs, dir| {
            dir.read_binary_file(c_str!("loginit"), &logs.loginit);
            dir.read_binary_file(c_str!("logintr"), &logs.logintr);
            dir.read_binary_file(c_str!("logrm"), &logs.logrm);
        });

        Ok(try_pin_init!(Self {
            libos,
            logs <- logs,
            rmargs,
            cmdq,
        }))
    }
}
