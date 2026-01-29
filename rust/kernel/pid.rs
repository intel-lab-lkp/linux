// SPDX-License-Identifier: GPL-2.0

//! Process identifiers (PIDs).
//!
//! C header: [`include/linux/pid.h`](srctree/include/linux/pid.h)

use crate::{bindings, ffi::c_int, sync::rcu, task::Task, types::Opaque};

/// Wraps the kernel's `struct pid`.
///
/// This structure represents the Rust abstraction for a C `struct pid`.
/// A `Pid` represents a process identifier that can be looked up in different
/// PID namespaces.
#[repr(transparent)]
pub struct Pid {
    inner: Opaque<bindings::pid>,
}

impl Pid {
    /// Returns a raw pointer to the inner C struct.
    #[inline]
    pub fn as_ptr(&self) -> *mut bindings::pid {
        self.inner.get()
    }

    /// Finds a `struct pid` by its pid number within the current task's PID namespace.
    ///
    /// Returns `None` if no such pid exists.
    ///
    /// The returned reference is only valid for the duration of the RCU read-side
    /// critical section represented by the `rcu::Guard`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::pid::Pid;
    /// use kernel::sync::rcu;
    ///
    /// let guard = rcu::read_lock();
    /// if let Some(pid) = Pid::find_vpid_with_guard(1, &guard) {
    ///     pr_info!("Found pid 1\n");
    /// }
    /// ```
    ///
    /// Returns `None` for non-existent PIDs:
    ///
    /// ```
    /// use kernel::pid::Pid;
    /// use kernel::sync::rcu;
    ///
    /// let guard = rcu::read_lock();
    /// // PID 0 (swapper/idle) is not visible via find_vpid.
    /// assert!(Pid::find_vpid_with_guard(0, &guard).is_none());
    /// ```
    #[inline]
    pub fn find_vpid_with_guard<'a>(nr: i32, _rcu_guard: &'a rcu::Guard) -> Option<&'a Self> {
        // SAFETY: Called under RCU protection as guaranteed by the Guard reference.
        let ptr = unsafe { bindings::find_vpid(nr as c_int) };
        if ptr.is_null() {
            None
        } else {
            // SAFETY: `find_vpid` returns a valid pointer under RCU protection,
            // and `Pid` is `#[repr(transparent)]` over `bindings::pid`.
            Some(unsafe { &*(ptr as *const Self) })
        }
    }

    /// Gets the task associated with this PID.
    ///
    /// Returns `None` if no task is associated with this PID.
    ///
    /// The returned reference is only valid for the duration of the RCU read-side
    /// critical section represented by the `rcu::Guard`.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::pid::Pid;
    /// use kernel::sync::rcu;
    ///
    /// let guard = rcu::read_lock();
    /// if let Some(pid) = Pid::find_vpid_with_guard(1, &guard) {
    ///     if let Some(task) = pid.pid_task_with_guard(&guard) {
    ///         pr_info!("Found task for pid 1\n");
    ///     }
    /// }
    /// ```
    #[inline]
    pub fn pid_task_with_guard<'a>(&'a self, _rcu_guard: &'a rcu::Guard) -> Option<&'a Task> {
        // SAFETY: Called under RCU protection as guaranteed by the Guard reference.
        let task_ptr = unsafe { bindings::pid_task(self.as_ptr(), bindings::pid_type_PIDTYPE_PID) };
        if task_ptr.is_null() {
            None
        } else {
            // SAFETY: `pid_task` returns a valid pointer under RCU protection,
            // and `Task` is `#[repr(transparent)]` over `bindings::task_struct`.
            Some(unsafe { &*task_ptr.cast() })
        }
    }
}
