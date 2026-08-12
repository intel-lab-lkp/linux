// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

use kernel::{
    debugfs,
    driver::Registration,
    pci,
    prelude::*,
    InPlaceModule, //
};

mod driver;
mod falcon;
mod fb;
mod firmware;
mod fsp;
mod gpu;
mod gsp;
mod mctp;
#[macro_use]
mod num;
mod regs;
mod sbuffer;
mod vbios;
mod vgpu;

pub(crate) const MODULE_NAME: &core::ffi::CStr = <LocalModule as kernel::ModuleMetadata>::NAME;

// TODO: Move this into per-module data once that exists.
static mut DEBUGFS_ROOT: Option<debugfs::Dir> = None;

#[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
kernel::sync::global_lock! {
    /// Log buffers of GPUs that are gone, kept around until the module is unloaded.
    // TODO: Move this into per-module data once that exists.
    unsafe(uninit) static RETAINED_LOGS: Mutex<gsp::RetainedLogs> = gsp::RetainedLogs::new();
}

/// Guard that clears `DEBUGFS_ROOT` when dropped.
struct DebugfsRootGuard;

impl Drop for DebugfsRootGuard {
    fn drop(&mut self) {
        // Retained log buffers own debugfs entries below `DEBUGFS_ROOT`, so they have to go away
        // before it does.
        #[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
        RETAINED_LOGS.lock().clear();

        // SAFETY: This guard is dropped after `_driver` (due to field order),
        // so the driver is unregistered and no probe() can be running.
        unsafe { DEBUGFS_ROOT = None };
    }
}

#[pin_data]
struct NovaCoreModule {
    // Fields are dropped in declaration order, so `_driver` is dropped first,
    // then `_debugfs_guard` clears `DEBUGFS_ROOT`.
    #[pin]
    _driver: Registration<pci::Adapter<driver::NovaCoreDriver>>,
    _debugfs_guard: DebugfsRootGuard,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        let dir = debugfs::Dir::new(c"nova-core");

        // SAFETY: Module initialization runs exactly once, and before the driver is registered,
        // so no probe can have touched `RETAINED_LOGS` yet.
        #[cfg(CONFIG_NOVA_CORE_KEEP_GSP_LOGS)]
        unsafe {
            RETAINED_LOGS.init()
        };

        // SAFETY: We are the only driver code running during init, so there
        // cannot be any concurrent access to `DEBUGFS_ROOT`.
        unsafe { DEBUGFS_ROOT = Some(dir) };

        try_pin_init!(Self {
            _driver <- Registration::new(MODULE_NAME, module),
            _debugfs_guard: DebugfsRootGuard,
        })
    }
}

module! {
    type: NovaCoreModule,
    name: "nova-core",
    authors: ["Danilo Krummrich"],
    description: "Nova Core GPU driver",
    license: "GPL v2",
    firmware: [],
}

kernel::module_firmware!(firmware::ModInfoBuilder);
