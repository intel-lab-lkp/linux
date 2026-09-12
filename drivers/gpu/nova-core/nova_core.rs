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
mod mm;
#[macro_use]
mod num;
mod regs;
mod sbuffer;
#[cfg(CONFIG_NOVA_CORE_SELFTESTS)]
mod selftest;
mod vbios;
mod vgpu;

pub(crate) const MODULE_NAME: &core::ffi::CStr = <LocalModule as kernel::ModuleMetadata>::NAME;

// TODO: Move this into per-module data once that exists.
static mut DEBUGFS_ROOT: Option<debugfs::Dir> = None;

/// Guard that clears `DEBUGFS_ROOT` when dropped.
struct DebugfsRootGuard;

impl Drop for DebugfsRootGuard {
    fn drop(&mut self) {
        // SAFETY: This guard is dropped after `_driver` (due to field order),
        // so the driver is unregistered and no probe() can be running.
        unsafe { DEBUGFS_ROOT = None };
    }
}

#[pin_data]
struct NovaCoreModule {
    // Fields are dropped in declaration order, so `_driver` is dropped first,
    // then `_debugfs_guard` clears `DEBUGFS_ROOT`. They are initialized the
    // other way round, see `init()`.
    #[pin]
    _driver: Registration<pci::Adapter<driver::NovaCoreDriver>>,
    _debugfs_guard: DebugfsRootGuard,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        let dir = debugfs::Dir::new(c"nova-core");

        // SAFETY: We are the only driver code running during init, so there
        // cannot be any concurrent access to `DEBUGFS_ROOT`.
        unsafe { DEBUGFS_ROOT = Some(dir) };

        // Fields are initialized in the order written here, and an initializer that fails drops
        // what it has already built, so the guard goes first: should registration fail, its drop
        // still takes `DEBUGFS_ROOT` down with it. Nothing would otherwise, as statics are never
        // dropped and the module is unloaded right away, leaving a directory behind that the
        // next load cannot create again.
        try_pin_init!(Self {
            _debugfs_guard: DebugfsRootGuard,
            _driver <- Registration::new(MODULE_NAME, module),
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
