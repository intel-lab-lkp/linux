// SPDX-License-Identifier: GPL-2.0
//! Contains abstractions for disabling CPU preemption. See [`CpuGuard`].

/// A RAII guard for `bindings::preempt_disable` and `bindings::preempt_enable`.
///
/// Guarantees preemption is disabled for as long as this object exists.
pub struct CpuGuard {
    // Don't make one without using new()
    _phantom: (),
    /// [`CpuGuard`] is not [`Send`]: moving it to another CPU would break its guarantees and also
    /// leave the CPU it was created on with preemption disabled indefinitely.
    ///
    /// It is also not [`Sync`]: preemption being disabled on one CPU is not relevant to other
    /// CPUs, so we shouldn't be able to share a `&CpuGuard` between threads.
    _not_send_sync: core::marker::PhantomData<*const ()>,
}

impl CpuGuard {
    /// Create a new [`CpuGuard`]. Disables preemption for its lifetime.
    #[inline]
    pub fn new() -> Self {
        // SAFETY: There are no preconditions required to call preempt_disable
        unsafe {
            bindings::preempt_disable();
        }
        CpuGuard {
            _phantom: (),
            _not_send_sync: core::marker::PhantomData,
        }
    }
}

impl Default for CpuGuard {
    #[inline]
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for CpuGuard {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: There are no preconditions required to call preempt_enable
        unsafe {
            bindings::preempt_enable();
        }
    }
}
