// SPDX-License-Identifier: GPL-2.0

//! Reboot support.
//!
//! C header: [`include/linux/reboot.h`](srctree/include/linux/reboot.h).

use crate::bindings;

/// Restarts the machine immediately, without syncing or unmounting
/// filesystems and without going through the reboot notifier chains.
///
/// This is intended for situations where the system is in a state where an
/// orderly shutdown is no longer possible, for example when a watchdog
/// expires. It can be called from any context, including interrupt context.
pub fn emergency_restart() {
    // SAFETY: `emergency_restart` has no preconditions and is documented as
    // safe to call from any context.
    unsafe { bindings::emergency_restart() }
}
