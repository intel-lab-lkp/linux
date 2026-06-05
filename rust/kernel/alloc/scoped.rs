// SPDX-License-Identifier: GPL-2.0

//! Scoped allocation policies for the current task.
//!
//! The kernel exposes several per-task allocation policies through
//! save/restore pairs in [`include/linux/sched/mm.h`]: `memalloc_noio`,
//! `memalloc_nofs`, `memalloc_noreclaim` and `memalloc_pin`. Each pair
//! sets a bit in `current->flags` and returns the prior state, which a
//! later call restores. The save/restore APIs assume strict LIFO
//! nesting; restoring out of order corrupts the per-task state.
//!
//! This module exposes the policies as a generic [`Scope<K>`] guard,
//! parameterized over a [`ScopeKind`] tag. The type is `!Unpin` and
//! constructed only through the [`memalloc_scope!`] macro, which binds
//! it to a hidden stack slot via [`core::pin::pin!`] and rebinds the
//! handle as a shared pinned reference. Safe code therefore has no path
//! to either move the guard or drop it ahead of its lexical scope, so
//! nested scopes always restore in LIFO order.
//!
//! [`include/linux/sched/mm.h`]: srctree/include/linux/sched/mm.h
//!
//! # Examples
//!
//! ```ignore
//! use kernel::memalloc_scope;
//! use kernel::alloc::scoped::NoIo;
//!
//! fn process_io_request() {
//!     memalloc_scope!(let _noio: NoIo);
//!     // Every allocation in this scope behaves as if `GFP_NOIO` were
//!     // set, even when the call site passes `GFP_KERNEL`.
//! }
//! ```

use core::{
    ffi::c_uint,
    marker::{
        PhantomData,
        PhantomPinned, //
    },
};

use crate::types::NotThreadSafe;

pub use crate::memalloc_scope;

mod private {
    pub trait Sealed {}
}

/// Selects which `memalloc_*` save/restore pair a [`Scope`] wraps.
///
/// Implemented only by the zero-sized tag types in this module
/// ([`NoIo`], [`NoFs`], [`NoReclaim`], [`MemallocPin`]). The trait is
/// sealed.
pub trait ScopeKind: private::Sealed {
    /// Begin a scope on the current task and return the prior state.
    #[doc(hidden)]
    fn save() -> c_uint;

    /// End a scope on the current task.
    ///
    /// # Safety
    ///
    /// `prev` must be the value returned by the matching [`save`] call,
    /// and the call must execute on the same task that ran [`save`].
    ///
    /// [`save`]: ScopeKind::save
    #[doc(hidden)]
    unsafe fn restore(prev: c_uint);
}

/// A scope that imposes an allocation policy on the current task while
/// it is live.
///
/// Construct one with [`memalloc_scope!`]. `Scope` is `!Unpin` and its
/// constructor is hidden, so a `Scope` only ever exists pinned to a
/// stack slot owned by the construction macro; safe code cannot drop
/// it out of order or send it across tasks. The C-side state is
/// restored in [`Drop`], which runs when the stack slot goes out of
/// scope.
pub struct Scope<K: ScopeKind> {
    prev: c_uint,
    _kind: PhantomData<K>,
    _pin: PhantomPinned,
    _not_thread_safe: NotThreadSafe,
}

impl<K: ScopeKind> Scope<K> {
    /// Begin a scope of kind `K` on the current task.
    ///
    /// # Safety
    ///
    /// The returned value must be pinned to the stack frame that calls
    /// this function and dropped on the same task. In practice, only
    /// [`memalloc_scope!`] should call this — the macro arranges both.
    #[doc(hidden)]
    pub unsafe fn new() -> Self {
        Self {
            prev: K::save(),
            _kind: PhantomData,
            _pin: PhantomPinned,
            _not_thread_safe: NotThreadSafe,
        }
    }
}

impl<K: ScopeKind> Drop for Scope<K> {
    fn drop(&mut self) {
        // SAFETY: `self.prev` was produced by `K::save` in `Self::new`.
        // The caller of `new` upheld the contract that the value
        // remains pinned to its construction stack frame, so this drop
        // runs on the same task as the matching save.
        unsafe { K::restore(self.prev) };
    }
}

macro_rules! define_kind {
    (
        $(#[$meta:meta])*
        $name:ident, $save:ident, $restore:ident $(,)?
    ) => {
        $(#[$meta])*
        pub struct $name;

        impl private::Sealed for $name {}

        impl ScopeKind for $name {
            fn save() -> c_uint {
                // SAFETY: Updates a per-task flag and is documented as
                // safe from any context.
                unsafe { bindings::$save() }
            }

            unsafe fn restore(prev: c_uint) {
                // SAFETY: Per the trait contract, `prev` is the value
                // returned by the matching `save`, on the same task.
                unsafe { bindings::$restore(prev) };
            }
        }
    };
}

define_kind!(
    /// `GFP_NOIO` scope.
    ///
    /// While a `Scope<NoIo>` is live, allocations on the current task
    /// behave as if `GFP_NOIO` were set, making them safe to issue from
    /// the IO completion path.
    ///
    /// Corresponds to `memalloc_noio_save` / `memalloc_noio_restore` in
    /// `include/linux/sched/mm.h`.
    NoIo,
    memalloc_noio_save,
    memalloc_noio_restore,
);

define_kind!(
    /// `GFP_NOFS` scope.
    ///
    /// While a `Scope<NoFs>` is live, allocations on the current task
    /// behave as if `GFP_NOFS` were set, making them safe to issue from
    /// a filesystem critical section.
    ///
    /// Corresponds to `memalloc_nofs_save` / `memalloc_nofs_restore` in
    /// `include/linux/sched/mm.h`.
    NoFs,
    memalloc_nofs_save,
    memalloc_nofs_restore,
);

define_kind!(
    /// No-reclaim scope.
    ///
    /// While a `Scope<NoReclaim>` is live, allocations on the current
    /// task may dip into the memory reserves. Callers must be sure their
    /// allocations will help free more memory shortly; see the kernel C
    /// documentation for the full contract.
    ///
    /// Corresponds to `memalloc_noreclaim_save` /
    /// `memalloc_noreclaim_restore` in `include/linux/sched/mm.h`.
    NoReclaim,
    memalloc_noreclaim_save,
    memalloc_noreclaim_restore,
);

define_kind!(
    /// Long-term pin scope.
    ///
    /// While a `Scope<MemallocPin>` is live, allocations on the current
    /// task are restricted to zones that allow long-term pinning.
    ///
    /// Corresponds to `memalloc_pin_save` / `memalloc_pin_restore` in
    /// `include/linux/sched/mm.h`.
    MemallocPin,
    memalloc_pin_save,
    memalloc_pin_restore,
);

/// Bind a [`Scope`] of the given kind to the current stack frame.
///
/// `$kind` must name one of the zero-sized tag types defined in this
/// module: [`NoIo`], [`NoFs`], [`NoReclaim`], [`MemallocPin`]. The
/// macro shadows `$name` first with the owning pinned slot and then
/// with a shared pinned reference, so the value lives until the end of
/// the enclosing block and cannot be dropped early by safe code.
///
/// # Examples
///
/// ```ignore
/// use kernel::memalloc_scope;
/// use kernel::alloc::scoped::NoIo;
///
/// memalloc_scope!(let _scope: NoIo);
/// // ... allocations here behave as if `GFP_NOIO` were set.
/// ```
#[macro_export]
macro_rules! memalloc_scope {
    (let $name:ident : $kind:ident) => {
        // SAFETY: `pin!` places the value in a hidden stack slot and
        // returns a `Pin<&mut _>`; combined with `Scope: !Unpin`, safe
        // code can neither extract ownership nor reorder its drop
        // relative to nested scopes, so the save/restore discipline of
        // the underlying C API is preserved.
        let $name = ::core::pin::pin!(unsafe {
            $crate::alloc::scoped::Scope::<$crate::alloc::scoped::$kind>::new()
        });
        let $name: ::core::pin::Pin<&$crate::alloc::scoped::Scope<$crate::alloc::scoped::$kind>> =
            $name.as_ref();
    };
}
