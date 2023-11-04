// SPDX-License-Identifier: GPL-2.0

//! USB devices and drivers.
//!
//! C header: [`include/linux/usb.h`](../../../../include/linux/usb.h)

use kernel::bindings;

/// Check if USB is disabled.
pub fn disabled() -> bool {
    // SAFETY: FFI call.
    unsafe { bindings::usb_disabled() != 0 }
}
