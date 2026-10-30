// SPDX-License-Identifier: GPL-2.0

//! NUMA topology utilities.
//!
//! C header: [`include/linux/nodemask.h`](srctree/include/linux/nodemask.h)

#[cfg(CONFIG_NUMA)]
use crate::sync::atomic::{
    Atomic,
    Relaxed, //
};
use crate::{
    bindings,
    prelude::*, //
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

/// Non Uniform Memory Access (NUMA) node identifier.
#[derive(Clone, Copy, PartialEq)]
pub struct NumaNode(i32);

impl NumaNode {
    /// Create a new NUMA node identifier (non-negative integer).
    ///
    /// Returns [`EINVAL`] if a negative id or an id exceeding [`bindings::MAX_NUMNODES`] is
    /// specified.
    pub fn new(node: i32) -> Result<Self> {
        // MAX_NUMNODES never exceeds 2**10 because NODES_SHIFT is 0..10.
        if node < 0 || node >= bindings::MAX_NUMNODES as i32 {
            return Err(EINVAL);
        }
        Ok(Self(node))
    }

    /// Get the raw representation of this node identifier.
    pub(crate) fn as_raw(self) -> i32 {
        self.0
    }
}

/// Specify necessary constant to pass the information to Allocator that the caller doesn't care
/// about the NUMA node to allocate memory from.
impl NumaNode {
    /// No node preference.
    pub const NO_NODE: NumaNode = NumaNode(bindings::NUMA_NO_NODE);
}
