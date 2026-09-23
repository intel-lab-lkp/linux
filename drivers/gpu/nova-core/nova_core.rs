// SPDX-License-Identifier: GPL-2.0

//! Nova Core GPU Driver

use kernel::{
    debugfs,
    device,
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

/// Pointer to the [`DebugfsData`] the module owns.
///
/// A device has no way to reach the data of its module, so code running on behalf of a bound
/// device goes through here instead.
/// Written once, while the module data is built, and never again: what it points at is dropped
/// only after the driver is unregistered, so it is good for as long as any device is bound, and
/// is not read outside of that.
// TODO: Drop this once devices can reach the data of their module.
static mut DEBUGFS_DATA: *const DebugfsData = core::ptr::null();

/// Data the module shares with every GPU it drives.
///
/// Reached from a device through [`debugfs_data()`].
#[pin_data]
pub(crate) struct DebugfsData {
    /// Root directory of the driver in debugfs.
    root: debugfs::Dir,
}

impl DebugfsData {
    /// Creates the shared data.
    fn new() -> impl PinInit<Self> {
        pin_init!(Self {
            root: debugfs::Dir::new(c"nova-core"),
        })
    }

    /// Returns the root directory of the driver in debugfs.
    pub(crate) fn root(&self) -> &debugfs::Dir {
        &self.root
    }
}

/// Returns the data the module shares with its devices.
///
/// The bound device is what makes this sound: the data is built before the driver is registered
/// and dropped after it is unregistered, so it outlives every device that is bound, and the
/// returned reference cannot be held past the one it is taken for.
pub(crate) fn debugfs_data<'a>(_dev: &'a device::Device<device::Bound>) -> &'a DebugfsData {
    // SAFETY: A device can only be bound once the driver is registered, which happens after
    // `DEBUGFS_DATA` is written, so it points at the data of this module, which lives at least
    // as long as the caller's device is bound.
    unsafe { &*DEBUGFS_DATA }
}

#[pin_data]
struct NovaCoreModule {
    // Fields are dropped in declaration order, so the registration goes first and no probe() can
    // still be running once the shared data is torn down. `init()` builds them the other way
    // round, as the data has to be there before the first probe() reaches for it.
    #[pin]
    _driver: Registration<pci::Adapter<driver::NovaCoreDriver>>,
    #[pin]
    _debugfs: DebugfsData,
}

impl InPlaceModule for NovaCoreModule {
    fn init(module: &'static kernel::ThisModule) -> impl PinInit<Self, Error> {
        try_pin_init!(Self {
            _debugfs <- DebugfsData::new(),
            _: {
                // SAFETY: Nothing reads `DEBUGFS_DATA` before a device is bound, which cannot
                // happen until the driver is registered below. What it points at is dropped only
                // once the driver is unregistered, so it is valid for as long as any device is
                // bound, and nothing reads it outside of that.
                unsafe { DEBUGFS_DATA = &*_debugfs };
            },
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
