// SPDX-License-Identifier: GPL-2.0

mod boot;
mod hal;

#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
use kernel::sync::aref::ARef;
use kernel::{
    debugfs,
    device,
    dma::{
        Coherent,
        CoherentBox,
        CoherentView,
        DmaAddress, //
    },
    io::{
        io_project,
        io_write,
        Io, //
    },
    pci,
    prelude::*, //
};

pub(crate) mod cmdq;
pub(crate) mod commands;
mod fw;
mod regs;
mod sequencer;

pub(crate) use fw::{
    GspFmcBootParams,
    GspFwWprMeta,
    LibosMemoryRegionInitArgument,
    LibosParams, //
};
pub(crate) use hal::boot_firmware_files;

use crate::{
    driver::Bar0,
    falcon::{
        gsp::Gsp as GspFalcon,
        sec2::Sec2 as Sec2Falcon,
        Falcon, //
    },
    fsp::Fsp,
    gpu::Chipset,
    gsp::{
        cmdq::Cmdq,
        fw::GspArgumentsPadded, //
    },
    num,
    vgpu::VgpuManager, //
};

pub(crate) const GSP_PAGE_SHIFT: usize = 12;
pub(crate) const GSP_PAGE_SIZE: usize = 1 << GSP_PAGE_SHIFT;

/// Common context for the GSP boot process.
///
/// It carries two distinct lifetimes:
///
/// - `'gpu` is the lifetime of the bound GPU device, as captured by the GPU subdevices.
/// - `'ctx` is a shorter lifetime during which this context borrows those subdevices.
pub(crate) struct GspBootContext<'ctx, 'gpu> {
    pub(crate) pdev: &'gpu pci::Device<device::Bound>,
    pub(crate) bar: Bar0<'gpu>,
    pub(crate) chipset: Chipset,
    pub(crate) gsp_falcon: &'ctx Falcon<'gpu, GspFalcon>,
    pub(crate) sec2_falcon: &'ctx Falcon<'gpu, Sec2Falcon>,
    pub(crate) fsp: Option<&'ctx mut Fsp<'gpu>>,
    pub(crate) vgpu: &'ctx VgpuManager,
}

impl<'ctx, 'gpu> GspBootContext<'ctx, 'gpu> {
    pub(crate) fn dev(&self) -> &'gpu device::Device<device::Bound> {
        self.pdev.as_ref()
    }
}

/// Number of GSP pages to use in a RM log buffer.
const RM_LOG_BUFFER_NUM_PAGES: usize = 0x10;
const LOG_BUFFER_SIZE: usize = RM_LOG_BUFFER_NUM_PAGES * GSP_PAGE_SIZE;

/// Array of page table entries, as understood by the GSP bootloader.
#[repr(C)]
#[derive(FromBytes, IntoBytes)]
struct PteArray<const NUM_ENTRIES: usize>([u64; NUM_ENTRIES]);

impl<const NUM_PAGES: usize> PteArray<NUM_PAGES> {
    /// Initialize a new page table array mapping `NUM_PAGES` GSP pages starting at address `start`.
    fn init(view: CoherentView<'_, Self>, start: DmaAddress) -> Result<()> {
        for i in 0..NUM_PAGES {
            io_write!(view, .0[build: i],
                start
                    .checked_add(num::usize_as_u64(i) << GSP_PAGE_SHIFT)
                    .ok_or(EOVERFLOW)?
            );
        }

        Ok(())
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
struct LogBuffer(Coherent<[u8; LOG_BUFFER_SIZE]>);

impl LogBuffer {
    /// Creates a new `LogBuffer` mapped on `dev`.
    fn new(dev: &device::Device<device::Bound>) -> Result<Self> {
        let obj = Self(Coherent::zeroed(dev, GFP_KERNEL)?);

        let start_addr = obj.0.dma_address();

        let pte_view = io_project!(
            obj.0,
            [build: size_of::<u64>()..][build: ..RM_LOG_BUFFER_NUM_PAGES * size_of::<u64>()]
        )
        .try_cast::<PteArray<RM_LOG_BUFFER_NUM_PAGES>>()?;
        PteArray::init(pte_view, start_addr)?;

        Ok(obj)
    }

    /// Copies the contents of this buffer into memory that does not belong to the device.
    ///
    /// A buffer the GSP never wrote to yields an empty vector, as it holds nothing worth keeping.
    #[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
    fn snapshot(&self) -> Result<KVec<u8>> {
        // Offset 0 holds the "put" pointer, which the GSP advances as it appends entries. It is
        // still zero if nothing was ever logged.
        let put = io_project!(self.0, [build: ..size_of::<u64>()]).try_cast::<u64>()?;
        if put.read_val() == 0 {
            return Ok(KVec::new());
        }

        let mut snapshot = KVec::zeroed(LOG_BUFFER_SIZE, GFP_KERNEL)?;
        io_project!(self.0, [build: ..]).copy_to_slice(&mut snapshot);

        Ok(snapshot)
    }
}

struct LogBuffers {
    /// Device the buffers belong to. Also names their debugfs directory.
    #[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
    dev: ARef<device::Device>,
    /// Init log buffer.
    loginit: LogBuffer,
    /// Interrupts log buffer.
    logintr: LogBuffer,
    /// RM log buffer.
    logrm: LogBuffer,
}

/// Copies of the log buffers of a GPU that is no longer around.
#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
struct RetainedLogBuffers {
    /// Device the buffers came from.
    dev: ARef<device::Device>,
    /// Contents of the init log buffer, empty if it was never written to.
    loginit: KVec<u8>,
    /// Contents of the interrupts log buffer, empty if it was never written to.
    logintr: KVec<u8>,
    /// Contents of the RM log buffer, empty if it was never written to.
    logrm: KVec<u8>,
}

/// Log buffers of GPUs that are gone, and the debugfs entries exposing them.
///
/// The copies live under a `retained` directory of their own instead of next to the entries of
/// the GPUs that are actually bound, so that a device coming back does not find its name taken.
#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
pub(crate) struct RetainedLogs {
    /// Parent directory of all copies, created together with the first one.
    dir: Option<debugfs::Dir>,
    /// One entry per GPU.
    gpus: KVec<Pin<KBox<debugfs::Scope<RetainedLogBuffers>>>>,
}

#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
impl RetainedLogs {
    /// Creates an empty set of retained log buffers.
    pub(crate) const fn new() -> Self {
        Self {
            dir: None,
            gpus: KVec::new(),
        }
    }

    /// Releases every copy and the directory holding them.
    pub(crate) fn clear(&mut self) {
        self.gpus.clear();
        self.dir = None;
    }
}

#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
impl LogBuffers {
    /// Preserves whatever the GSP logged, so it can still be read once the GPU is gone.
    ///
    /// The buffers are DMA allocations of the device and cannot outlive it, so their contents are
    /// copied into memory owned by the module and exposed through fresh debugfs entries. Those
    /// live until the module is unloaded.
    fn retain(&self) -> Result {
        let logs = RetainedLogBuffers {
            dev: self.dev.clone(),
            loginit: self.loginit.snapshot()?,
            logintr: self.logintr.snapshot()?,
            logrm: self.logrm.snapshot()?,
        };

        if logs.loginit.is_empty() && logs.logintr.is_empty() && logs.logrm.is_empty() {
            return Ok(());
        }

        let mut retained = crate::RETAINED_LOGS.lock();

        // An earlier run of the same device may have left a copy behind. Its directory carries
        // the name about to be used again, and its logs are the older ones, so drop it first.
        retained
            .gpus
            .retain(|gpu| gpu.dev.name() != self.dev.name());

        let dir = match retained.dir.clone() {
            Some(dir) => dir,
            None => {
                #[allow(static_mut_refs)]
                // SAFETY: `DEBUGFS_ROOT` is set before driver registration and cleared after
                // driver unregistration. This runs while a device is still bound, or on the way
                // out of a failed probe, so the driver is registered and nothing can be modifying
                // it.
                let root: &debugfs::Dir = unsafe { crate::DEBUGFS_ROOT.as_ref() }.ok_or(ENODEV)?;

                let dir = root.subdir(c"retained");
                retained.dir = Some(dir.clone());

                dir
            }
        };

        let scope = KBox::pin_init(
            dir.scope(logs, self.dev.name(), |logs, dir| {
                if !logs.loginit.is_empty() {
                    dir.read_binary_file(c"loginit", &logs.loginit);
                }
                if !logs.logintr.is_empty() {
                    dir.read_binary_file(c"logintr", &logs.logintr);
                }
                if !logs.logrm.is_empty() {
                    dir.read_binary_file(c"logrm", &logs.logrm);
                }
            }),
            GFP_KERNEL,
        )?;

        retained.gpus.push(scope, GFP_KERNEL)?;

        dev_info!(
            self.dev,
            "GSP-RM log buffers retained until the module is unloaded\n"
        );

        Ok(())
    }
}

#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
impl Drop for LogBuffers {
    fn drop(&mut self) {
        if let Err(e) = self.retain() {
            dev_warn!(self.dev, "failed to retain GSP-RM log buffers: {:?}\n", e);
        }
    }
}

/// GSP runtime data.
#[pin_data]
pub(crate) struct Gsp {
    /// Libos arguments.
    pub(crate) libos: Coherent<[LibosMemoryRegionInitArgument]>,
    /// Log buffers, optionally exposed via debugfs.
    #[pin]
    logs: debugfs::Scope<LogBuffers>,
    /// Command queue.
    #[pin]
    pub(crate) cmdq: Cmdq,
    /// RM arguments.
    rmargs: Coherent<GspArgumentsPadded>,
}

impl Gsp {
    // Creates an in-place initializer for a `Gsp` manager for `pdev`.
    pub(crate) fn new(pdev: &pci::Device<device::Bound>) -> impl PinInit<Self, Error> + '_ {
        pin_init::pin_init_scope(move || {
            let dev = pdev.as_ref();

            let loginit = LogBuffer::new(dev)?;
            let logintr = LogBuffer::new(dev)?;
            let logrm = LogBuffer::new(dev)?;

            // Initialise the logging structures. The OpenRM equivalents are in:
            // _kgspInitLibosLoggingStructures (allocates memory for buffers)
            // kgspSetupLibosInitArgs_IMPL (creates pLibosInitArgs[] array)
            Ok(try_pin_init!(Self {
                cmdq <- Cmdq::new(dev),
                rmargs: Coherent::init(dev, GFP_KERNEL, GspArgumentsPadded::new(&cmdq))?,
                libos: {
                    let mut libos = CoherentBox::zeroed_slice(
                        dev,
                        GSP_PAGE_SIZE / size_of::<LibosMemoryRegionInitArgument>(),
                        GFP_KERNEL,
                    )?;

                    libos.init_at(0, LibosMemoryRegionInitArgument::new("LOGINIT", &loginit.0))?;
                    libos.init_at(1, LibosMemoryRegionInitArgument::new("LOGINTR", &logintr.0))?;
                    libos.init_at(2, LibosMemoryRegionInitArgument::new("LOGRM", &logrm.0))?;
                    libos.init_at(3, LibosMemoryRegionInitArgument::new("RMARGS", rmargs))?;

                    libos.into()
                },
                logs <- {
                    let log_buffers = LogBuffers {
                        #[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
                        dev: dev.into(),
                        loginit,
                        logintr,
                        logrm,
                    };

                    #[allow(static_mut_refs)]
                    // SAFETY: `DEBUGFS_ROOT` is created before driver registration and cleared
                    // after driver unregistration, so no probe() can race with its modification.
                    //
                    // PANIC: `DEBUGFS_ROOT` cannot be `None` here.  It is set before driver
                    // registration and cleared after driver unregistration, so it is always
                    // `Some` for the entire lifetime that probe() can be called.
                    let log_parent: &debugfs::Dir = unsafe { crate::DEBUGFS_ROOT.as_ref() }
                        .expect("DEBUGFS_ROOT not initialized");

                    log_parent.scope(log_buffers, dev.name(), |logs, dir| {
                        dir.read_binary_file(c"loginit", &logs.loginit.0);
                        dir.read_binary_file(c"logintr", &logs.logintr.0);
                        dir.read_binary_file(c"logrm", &logs.logrm.0);
                    })
                },
            }))
        })
    }

    /// Query the GSP for the static GPU information.
    pub(crate) fn get_static_info(&self, bar: Bar0<'_>) -> Result<commands::GetGspStaticInfoReply> {
        self.cmdq.send_command(bar, commands::GetGspStaticInfo)
    }
}

/// Opaque bundle required to unload the GSP. Created by [`Gsp::boot`], consumed by [`Gsp::unload`].
pub(crate) struct UnloadBundle(KBox<dyn hal::UnloadBundle>);
