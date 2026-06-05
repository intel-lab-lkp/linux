// SPDX-License-Identifier: GPL-2.0

//! NUMA topology utilities.
//!
//! C header: [`include/linux/nodemask.h`](srctree/include/linux/nodemask.h)

use crate::bindings;

/// Returns the number of online NUMA nodes.
#[inline]
pub fn num_online_nodes() -> u32 {
    // NOTE: In some configurations, we can read this variable without an unsafe block.
    // SAFETY: When NUMA is enabled, this is a global mutable static. We do as C and just read it,
    // even though it might race.
    #[allow(unused_unsafe)]
    unsafe {
        bindings::nr_online_nodes
    }
}
