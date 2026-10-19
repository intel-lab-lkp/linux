// SPDX-License-Identifier: GPL-2.0

//! Kernel taint tracking.
//!
//! C header: [`include/linux/panic.h`](srctree/include/linux/panic.h)
//!
//! See the [tainted-kernels documentation] for the meaning of each taint.
//!
//! [tainted-kernels documentation]: srctree/Documentation/admin-guide/tainted-kernels.rst

/// A reason for tainting the kernel.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum Taint {
    /// Tainting was explicitly requested by the user.
    User = bindings::TAINT_USER,
}

impl Taint {
    #[inline]
    const fn as_raw(self) -> u32 {
        self as u32
    }
}

/// Whether lock debugging remains reliable after a tainting event.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u32)]
pub enum Lockdep {
    /// Keep lock debugging enabled.
    StillOk = bindings::lockdep_ok_LOCKDEP_STILL_OK,
}

impl Lockdep {
    #[inline]
    const fn as_raw(self) -> bindings::lockdep_ok {
        self as u32
    }
}

/// Adds a kernel taint flag.
///
/// Taint flags persist until reboot, even if the module that set them is unloaded.
/// Use [`Lockdep::StillOk`] when the tainting event does not compromise lock debugging.
/// This may panic if the kernel's `panic_on_taint` setting matches a set taint flag.
#[inline]
pub fn add_taint(flag: Taint, lockdep: Lockdep) {
    // SAFETY: `Taint` and `Lockdep` restrict both arguments to valid C API values.
    unsafe { bindings::add_taint(flag.as_raw(), lockdep.as_raw()) };
}
