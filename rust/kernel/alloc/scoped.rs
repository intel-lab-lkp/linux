// SPDX-License-Identifier: GPL-2.0

//! Scoped allocation policies for the current task.
//!
//! The kernel exposes several per-task allocation policies through save/restore pairs in
//! [`include/linux/sched/mm.h`]: `memalloc_noio`, `memalloc_nofs`, `memalloc_noreclaim` and
//! `memalloc_pin`. Each pair sets a bit in `current->flags` and returns the bits it added, which a
//! later call clears again. The save/restore APIs assume strict LIFO nesting; restoring out of
//! order corrupts the per-task state.
//!
//! This module exposes each policy as a [`ScopeKind`] implementation. [`ScopeKind::with`] saves
//! the policy, runs a closure and restores the policy before returning.
//!
//! [`include/linux/sched/mm.h`]: srctree/include/linux/sched/mm.h
//!
//! # Examples
//!
//! ```
//! use kernel::alloc::scoped::{NoIo, ScopeKind};
//!
//! // Every allocation inside the closure behaves as if `GFP_NOIO` were set, even though the call
//! // site passes `GFP_KERNEL`.
//! let b = NoIo::with(|| KBox::new(42_u32, GFP_KERNEL))?;
//! assert_eq!(*b, 42);
//! # Ok::<(), Error>(())
//! ```
// # Implementation details
//
// Since the closure used in this API is synchronous it cannot contain a suspension point, so the
// save and restore always execute in the same call frame, on the same task, and nested scopes
// restore in LIFO order.
use crate::ffi::c_uint;

/// A per-task allocation policy with a `memalloc_*` save/restore pair.
///
/// Implemented by the uninhabited types [`NoIo`], [`NoFs`], [`NoReclaim`] and [`MemallocPin`]. Use
/// [`ScopeKind::with`] to run code under the policy.
pub trait ScopeKind {
    /// Begin a scope on the current task and return the flags added to `current->flags`.
    ///
    /// Every call must be paired with a [`restore`] call on the same task, in LIFO order with
    /// respect to other scopes. Prefer [`with`], which arranges this automatically.
    ///
    /// [`restore`]: ScopeKind::restore
    /// [`with`]: ScopeKind::with
    fn save() -> c_uint;

    /// End a scope on the current task.
    ///
    /// # Safety
    ///
    /// `prev` must be the value returned by the matching [`save`] call, the call must execute on
    /// the same task that ran [`save`], and no scope begun after the matching [`save`] may still be
    /// active.
    ///
    /// [`save`]: ScopeKind::save
    unsafe fn restore(prev: c_uint);

    /// Run `f` with this policy applied.
    ///
    /// The current policy is saved before `f` runs and restored when `f` returns.
    fn with<R>(f: impl FnOnce() -> R) -> R {
        let prev = Self::save();
        let ret = f();
        // SAFETY: `prev` was produced by the `save` above on this task. `f` is a synchronous
        // closure, so it has already returned and any scope it began has already been restored.
        unsafe { Self::restore(prev) };
        ret
    }
}

/// `GFP_NOIO` scope.
///
/// Inside [`NoIo::with`], allocations on the current task behave as if `GFP_NOIO` were set, making
/// them safe to issue from the IO completion path.
///
/// Corresponds to `memalloc_noio_save` / `memalloc_noio_restore` in
/// [`include/linux/sched/mm.h`](srctree/include/linux/sched/mm.h).
///
/// [`NoIo::with`]: ScopeKind::with
pub enum NoIo {}

impl ScopeKind for NoIo {
    fn save() -> c_uint {
        // SAFETY: Updates a per-task flag and is documented as safe from any context.
        unsafe { bindings::memalloc_noio_save() }
    }

    unsafe fn restore(prev: c_uint) {
        // SAFETY: Per the trait contract, `prev` is the value returned by the matching `save`, on
        // the same task.
        unsafe { bindings::memalloc_noio_restore(prev) };
    }
}

/// `GFP_NOFS` scope.
///
/// Inside [`NoFs::with`], allocations on the current task behave as if `GFP_NOFS` were set, making
/// them safe to issue from a filesystem critical section.
///
/// Corresponds to `memalloc_nofs_save` / `memalloc_nofs_restore` in
/// [`include/linux/sched/mm.h`](srctree/include/linux/sched/mm.h).
///
/// [`NoFs::with`]: ScopeKind::with
pub enum NoFs {}

impl ScopeKind for NoFs {
    fn save() -> c_uint {
        // SAFETY: Updates a per-task flag and is documented as safe from any context.
        unsafe { bindings::memalloc_nofs_save() }
    }

    unsafe fn restore(prev: c_uint) {
        // SAFETY: Per the trait contract, `prev` is the value returned by the matching `save`, on
        // the same task.
        unsafe { bindings::memalloc_nofs_restore(prev) };
    }
}

/// No-reclaim scope.
///
/// Inside [`NoReclaim::with`], allocations on the current task may dip into the memory reserves.
/// Callers must be sure their allocations will help free more memory shortly; see the kernel C
/// documentation for the full contract.
///
/// The scope has no effect in hard interrupt context, where `PF_MEMALLOC` does not grant access to
/// the reserves; see `__gfp_pfmemalloc_flags()` in [`mm/page_alloc.c`](srctree/mm/page_alloc.c).
///
/// Corresponds to `memalloc_noreclaim_save` / `memalloc_noreclaim_restore` in
/// [`include/linux/sched/mm.h`](srctree/include/linux/sched/mm.h).
///
/// [`NoReclaim::with`]: ScopeKind::with
pub enum NoReclaim {}

impl ScopeKind for NoReclaim {
    fn save() -> c_uint {
        // SAFETY: Updates a per-task flag. In hard interrupt context the flag is ignored rather
        // than misinterpreted, so the call is sound from any context.
        unsafe { bindings::memalloc_noreclaim_save() }
    }

    unsafe fn restore(prev: c_uint) {
        // SAFETY: Per the trait contract, `prev` is the value returned by the matching `save`, on
        // the same task.
        unsafe { bindings::memalloc_noreclaim_restore(prev) };
    }
}

/// Long-term pin scope.
///
/// Inside [`MemallocPin::with`], allocations on the current task are restricted to zones that
/// allow long-term pinning.
///
/// Corresponds to `memalloc_pin_save` / `memalloc_pin_restore` in
/// [`include/linux/sched/mm.h`](srctree/include/linux/sched/mm.h).
///
/// [`MemallocPin::with`]: ScopeKind::with
pub enum MemallocPin {}

impl ScopeKind for MemallocPin {
    fn save() -> c_uint {
        // SAFETY: Updates a per-task flag and is documented as safe from any context.
        unsafe { bindings::memalloc_pin_save() }
    }

    unsafe fn restore(prev: c_uint) {
        // SAFETY: Per the trait contract, `prev` is the value returned by the matching `save`, on
        // the same task.
        unsafe { bindings::memalloc_pin_restore(prev) };
    }
}
