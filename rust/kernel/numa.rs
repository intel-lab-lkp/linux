// SPDX-License-Identifier: GPL-2.0

//! NUMA topology utilities.
//!
//! C header: [`include/linux/nodemask.h`](srctree/include/linux/nodemask.h)

use crate::bindings;
#[cfg(CONFIG_NUMA)]
use crate::sync::atomic::{
    Atomic,
    Relaxed, //
};

/// Returns the number of online NUMA nodes.
#[inline]
pub fn num_online_nodes() -> u32 {
    #[cfg(CONFIG_NUMA)]
    {
        // SAFETY: `nr_online_nodes` is a global variable that is valid for the lifetime of the
        // kernel. The C side updates it with plain writes during node hotplug and reads it with
        // plain reads. A relaxed load is the Rust equivalent of `READ_ONCE()`, which is the best
        // we can do on the reader side.
        unsafe { Atomic::from_ptr(&raw mut bindings::nr_online_nodes) }.load(Relaxed)
    }
    #[cfg(not(CONFIG_NUMA))]
    {
        bindings::nr_online_nodes
    }
}
