// SPDX-License-Identifier: GPL-2.0

//! Scoped allocation policies for the current task.
//!
//! The kernel exposes several per-task allocation policies through save/restore pairs in
//! [`include/linux/sched/mm.h`]: `memalloc_noio`, `memalloc_nofs`, `memalloc_noreclaim` and
//! `memalloc_pin`. Each pair is a wrapper around `memalloc_flags_save` and
//! `memalloc_flags_restore`. The save call sets bits in `current->flags` and returns the bits it
//! added, which the restore call clears again. The save/restore APIs assume strict LIFO nesting;
//! restoring out of order corrupts the per-task state.
//!
//! This module exposes each policy as a [`ScopeFlag`]. [`alloc_scope`] applies a set of policies,
//! runs a closure and restores the previous policies before returning.
//!
//! [`include/linux/sched/mm.h`]: srctree/include/linux/sched/mm.h
//!
//! # Examples
//!
//! ```
//! use kernel::alloc::scoped::{
//!     alloc_scope,
//!     ScopeFlag, //
//! };
//!
//! // Every allocation inside the closure behaves as if `GFP_NOIO` were set, even though the call
//! // site passes `GFP_KERNEL`.
//! let b = alloc_scope(ScopeFlag::NoIo, || KBox::new(42_u32, GFP_KERNEL))?;
//! assert_eq!(*b, 42);
//!
//! // Policies can be combined with the bitwise OR (`|`) operator.
//! let b = alloc_scope(ScopeFlag::NoIo | ScopeFlag::Pin, || KBox::new(42_u32, GFP_KERNEL))?;
//! assert_eq!(*b, 42);
//! # Ok::<(), Error>(())
//! ```
// # Implementation details
//
// Since the closure used in this API is synchronous it cannot contain a suspension point, so the
// save and restore always execute in the same call frame, on the same task, and nested scopes
// restore in LIFO order. The save and restore operations are not exposed on their own, so safe
// code cannot begin a scope without ending it.
use crate::impl_flags;

impl_flags!(
    /// A set of per-task allocation policies.
    ///
    /// Build a set by combining [`ScopeFlag`] values with the bitwise operators, then pass it to
    /// [`alloc_scope`] to run code under the policies.
    ///
    /// # Invariants
    ///
    /// Only bits that correspond to a [`ScopeFlag`] are set.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct ScopeFlags(u32);

    /// A per-task allocation policy.
    ///
    /// Each policy corresponds to a `memalloc_*_save` / `memalloc_*_restore` pair in
    /// [`include/linux/sched/mm.h`](srctree/include/linux/sched/mm.h).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum ScopeFlag {
        /// `GFP_NOIO` scope.
        ///
        /// Allocations on the current task behave as if `GFP_NOIO` were set, making them safe to
        /// issue from the IO completion path.
        ///
        /// Corresponds to `memalloc_noio_save` / `memalloc_noio_restore`.
        NoIo = bindings::PF_MEMALLOC_NOIO,

        /// `GFP_NOFS` scope.
        ///
        /// Allocations on the current task behave as if `GFP_NOFS` were set, making them safe to
        /// issue from a filesystem critical section.
        ///
        /// Corresponds to `memalloc_nofs_save` / `memalloc_nofs_restore`.
        NoFs = bindings::PF_MEMALLOC_NOFS,

        /// No-reclaim scope.
        ///
        /// Allocations on the current task may dip into the memory reserves. Callers must be sure
        /// their allocations will help free more memory shortly; see the kernel C documentation
        /// for the full contract.
        ///
        /// The scope has no effect in hard interrupt context, where `PF_MEMALLOC` does not grant
        /// access to the reserves; see `__gfp_pfmemalloc_flags()` in
        /// [`mm/page_alloc.c`](srctree/mm/page_alloc.c).
        ///
        /// Corresponds to `memalloc_noreclaim_save` / `memalloc_noreclaim_restore`.
        NoReclaim = bindings::PF_MEMALLOC,

        /// Long-term pin scope.
        ///
        /// Allocations on the current task are restricted to zones that allow long-term pinning.
        ///
        /// Corresponds to `memalloc_pin_save` / `memalloc_pin_restore`.
        Pin = bindings::PF_MEMALLOC_PIN,
    }
);

/// Run `f` with the allocation policies in `flags` applied to the current task.
///
/// The policies in `flags` are applied before `f` runs. When `f` returns, the policies that were
/// not already in effect before the call are removed again. `flags` can be a single [`ScopeFlag`]
/// or a [`ScopeFlags`] set.
///
/// # Examples
///
/// ```
/// use kernel::alloc::scoped::{
///     alloc_scope,
///     ScopeFlag, //
/// };
///
/// let b = alloc_scope(ScopeFlag::NoFs, || KBox::new(42_u32, GFP_KERNEL))?;
/// assert_eq!(*b, 42);
/// # Ok::<(), Error>(())
/// ```
#[inline]
pub fn alloc_scope<R>(flags: impl Into<ScopeFlags>, f: impl FnOnce() -> R) -> R {
    let flags: ScopeFlags = flags.into();

    // SAFETY: By the type invariant of `ScopeFlags`, `flags` only contains `PF_MEMALLOC*` bits.
    // Setting these on the current task is sound from any context. In hard interrupt context
    // `PF_MEMALLOC` is ignored rather than misinterpreted.
    let added_flags = unsafe { bindings::memalloc_flags_save(flags.into()) };

    let ret = f();

    // SAFETY: `added_flags` was produced by the `memalloc_flags_save` call above on this task. `f`
    // is a synchronous closure, so it has already returned and any scope it began has already
    // been restored.
    unsafe { bindings::memalloc_flags_restore(added_flags) };

    ret
}
