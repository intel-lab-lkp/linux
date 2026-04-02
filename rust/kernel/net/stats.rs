// SPDX-License-Identifier: GPL-2.0

//! Network-device statistics helpers.
//!
//! C header: [`include/linux/netdevice.h`](srctree/include/linux/netdevice.h)

use crate::{
    bindings,
    net::netdevice,
};

/// Equivalent to `dev_lstats_add(dev, len)`.
pub fn dev_lstats_add(dev: &netdevice::Device, len: u32) {
    // SAFETY: The helper expects a valid `net_device *`; `netdevice::Device` maintains that
    // invariant and the helper handles the required per-cpu synchronization internally.
    unsafe { bindings::dev_lstats_add(dev.as_ptr(), len) };
}
