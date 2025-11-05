// SPDX-License-Identifier: GPL-2.0
//! Contains abstractions for disabling CPU preemption. See [`CpuGuard`].

/// A RAII guard for `bindings::preempt_disable` and `bindings::preempt_enable`.
///
/// Guarantees preemption is disabled for as long as this object exists.
pub struct CpuGuard {
    // Don't make one without using new()
    _phantom: (),
}

impl CpuGuard {
    /// Create a new [`CpuGuard`]. Disables preemption for its lifetime.
    pub fn new() -> Self {
        // SAFETY: There are no preconditions required to call preempt_disable
        unsafe {
            bindings::preempt_disable();
        }
        CpuGuard { _phantom: () }
    }
}

impl Default for CpuGuard {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for CpuGuard {
    fn drop(&mut self) {
        // SAFETY: There are no preconditions required to call preempt_enable
        unsafe {
            bindings::preempt_enable();
        }
    }
}
