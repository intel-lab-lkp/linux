// SPDX-License-Identifier: GPL-2.0

//! Linux Security Module (LSM) abstractions.
//!
//! This module provides safe Rust abstractions for implementing Linux Security
//! Modules.  An LSM written in Rust implements the [`Hooks`] trait, then
//! registers itself at boot time with the [`define_lsm!`] macro.
//!
//! # Minimal example
//!
//! ```no_run
//! use kernel::lsm;
//! use kernel::prelude::*;
//!
//! struct MyLsm;
//!
//! impl lsm::Hooks for MyLsm {
//!     fn file_open(file: &kernel::fs::File) -> Result {
//!         pr_info!("file_open: flags={:#x}\n", file.flags());
//!         Ok(())
//!     }
//! }
//!
//! kernel::define_lsm!(MyLsm, "my_lsm\0", bindings::LSM_ID_UNDEF as u64);
//! ```
//!
//! C headers: [`include/linux/lsm_hooks.h`](srctree/include/linux/lsm_hooks.h),
//!            [`include/linux/security.h`](srctree/include/linux/security.h)

use crate::{bindings, fs::File, task::Task};
use core::marker::PhantomData;
use core::mem::MaybeUninit;

/// Wrapper around [`bindings::lsm_id`] that implements [`Sync`].
///
/// `lsm_id` contains a `*const c_char` name pointer (not `Sync` by default).
/// The pointer always points to a string literal in static storage and is
/// never mutated after construction, so sharing across threads is safe.
#[repr(transparent)]
pub struct LsmId(pub bindings::lsm_id);

// SAFETY: `lsm_id` holds only a static string pointer and a numeric ID.
// Both are immutable after construction.
unsafe impl Sync for LsmId {}

/// Wrapper around [`bindings::lsm_info`] that implements [`Sync`].
///
/// `lsm_info` contains several raw pointer fields.  All are set to static
/// addresses during `define_lsm!` macro expansion and are never mutated
/// after that, so sharing across threads is safe.
#[repr(transparent)]
pub struct LsmInfo(pub bindings::lsm_info);

// SAFETY: `lsm_info` is initialised once (at compile time via const fn)
// with pointers to static data, then placed in `.lsm_info.init` as a
// read-only descriptor.
unsafe impl Sync for LsmInfo {}

/// The trait that a Rust LSM must implement.
///
/// Each method corresponds to a kernel LSM hook.  Methods have default
/// no-op implementations so you only need to override the hooks you care
/// about.
///
/// # Safety contract for implementors
///
/// All methods are called from kernel context and may be called concurrently.
/// Implementors must ensure their implementations are:
///
/// - **Thread-safe** — multiple CPUs may call the same hook simultaneously.
/// - **Non-sleeping where required** — [`Hooks::task_free`] runs in a context
///   that must not sleep; see its documentation.
///
/// The `Sync + Send + 'static` bounds enforce these requirements at the
/// type level.
pub trait Hooks: Sync + Send + 'static {
    /// Called when a file is being opened.
    ///
    /// Return `Ok(())` to allow the open, or an `Err` to deny it.
    /// Common denial codes: [`EACCES`](crate::error::code::EACCES),
    /// [`EPERM`](crate::error::code::EPERM).
    fn file_open(_file: &File) -> crate::error::Result {
        Ok(())
    }

    /// Called when a new task (thread or process) is being created.
    ///
    /// `clone_flags` contains the `CLONE_*` flags passed to `clone(2)`.
    ///
    /// Return `Ok(())` to allow creation, or an `Err` to deny it.
    fn task_alloc(_task: &Task, _clone_flags: u64) -> crate::error::Result {
        Ok(())
    }

    /// Called when a task is being freed.
    ///
    /// This hook runs during task teardown.  **Must not sleep.**  Any
    /// per-task state stored in a security blob should be cleaned up here.
    fn task_free(_task: &Task) {}
}

/// C-callable adapter functions that bridge the LSM framework to [`Hooks`].
///
/// This type is never instantiated.  Its associated functions serve as the
/// `unsafe extern "C" fn` pointers registered with the kernel's LSM
/// static-call infrastructure.
///
/// # Invariants
///
/// Each adapter function is called only from the LSM framework in the
/// appropriate hook context, with valid, non-null pointer arguments.
#[doc(hidden)]
pub struct Adapter<T: Hooks>(PhantomData<T>);

impl<T: Hooks> Adapter<T> {
    /// Adapter for the `file_open` LSM hook.
    ///
    /// # Safety
    ///
    /// `file` must be a valid, non-null pointer to a `struct file` that
    /// remains valid for the duration of this call.  Called only by the
    /// LSM framework.
    pub unsafe extern "C" fn file_open(
        file: *mut bindings::file,
    ) -> core::ffi::c_int {
        // SAFETY: The LSM framework guarantees `file` is valid and non-null
        // for the duration of this call.
        let file_ref = unsafe { File::from_raw_file(file.cast_const()) };
        match T::file_open(file_ref) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// Adapter for the `task_alloc` LSM hook.
    ///
    /// # Safety
    ///
    /// `task` must be a valid, non-null pointer to a newly-allocated
    /// `task_struct`.  `clone_flags` are the flags passed to `clone(2)`.
    /// Called only by the LSM framework.
    pub unsafe extern "C" fn task_alloc(
        task: *mut bindings::task_struct,
        clone_flags: u64,
    ) -> core::ffi::c_int {
        // SAFETY: The LSM framework guarantees `task` is valid, non-null,
        // and exclusively owned by the framework for the duration of this
        // call.  `Task` is `#[repr(transparent)]` over
        // `Opaque<task_struct>`, which is `#[repr(transparent)]` over
        // `UnsafeCell<MaybeUninit<task_struct>>`, giving the same layout.
        let task_ref = unsafe { &*(task as *const Task) };
        match T::task_alloc(task_ref, clone_flags) {
            Ok(()) => 0,
            Err(e) => e.to_errno(),
        }
    }

    /// Adapter for the `task_free` LSM hook.
    ///
    /// # Safety
    ///
    /// `task` must be a valid, non-null pointer to a `task_struct` that
    /// is being freed.  Must not sleep.  Called only by the LSM framework.
    pub unsafe extern "C" fn task_free(task: *mut bindings::task_struct) {
        // SAFETY: Same layout argument as `task_alloc`.
        let task_ref = unsafe { &*(task as *const Task) };
        T::task_free(task_ref);
    }
}

/// Constructs a [`bindings::lsm_info`] value for use by [`define_lsm!`].
///
/// All fields not explicitly set are zero-initialised, which the kernel
/// interprets as:
/// - `order`: `LSM_ORDER_MUTABLE` (0)
/// - `flags`: none
/// - `blobs`: no security blobs requested
/// - `enabled`: use the default (always enabled)
/// - `initcall_*`: no additional initcalls
///
/// # Safety
///
/// - `id` must point to a `lsm_id` with `'static` lifetime.
/// - `init` must be a valid init function called once during `security_init()`.
#[doc(hidden)]
pub const unsafe fn new_lsm_info(
    id: *const bindings::lsm_id,
    init: Option<unsafe extern "C" fn() -> core::ffi::c_int>,
) -> LsmInfo {
    // SAFETY: `bindings::lsm_info` is a C struct.  Zero-initialisation
    // gives valid zero/null values for every optional field.  The caller
    // is responsible for providing a valid `id` and `init`.
    let mut info: bindings::lsm_info =
        unsafe { MaybeUninit::zeroed().assume_init() };
    info.id = id;
    info.init = init;
    LsmInfo(info)
}

/// Registers a Rust LSM implementation with the kernel at boot time.
///
/// # Usage
///
/// ```no_run
/// kernel::define_lsm!(MyLsmType, "my_lsm\0", bindings::LSM_ID_UNDEF as u64);
/// ```
///
/// The macro generates:
/// - A `static lsm_id` identifying this LSM to the kernel.
/// - A static array of `security_hook_list` entries (initialised via C shims
///   to avoid `__randomize_layout` pitfalls).
/// - An `unsafe extern "C"` init function that populates the hook list and
///   calls `security_add_hooks()`.
/// - A `static lsm_info` placed in the `.lsm_info.init` linker section so
///   the kernel discovers and calls the init function during `security_init()`.
///
/// # Limitations (v1)
///
/// - Only the `file_open`, `task_alloc`, and `task_free` hooks are registered.
///   Additional hooks will be added in follow-up patches as safe Rust wrappers
///   for the argument types are contributed upstream.
/// - At most one Rust LSM can be registered per kernel build.  Unique name
///   generation for multiple LSMs requires a proc-macro (planned for v2).
#[macro_export]
macro_rules! define_lsm {
    ($T:ty, $name:expr, $id:expr) => {
        mod __rust_lsm_registration {
            use super::*;

            // The LSM identity — must be 'static because the static-call
            // table holds a back-pointer to it via security_hook_list.lsmid.
            // LsmId wraps lsm_id with `unsafe impl Sync` so it can be `static`.
            static __LSM_ID: $crate::lsm::LsmId =
                $crate::lsm::LsmId($crate::bindings::lsm_id {
                    name: $name.as_ptr().cast(),
                    id: $id,
                });

            // The hook list array.  MaybeUninit avoids needing security_hook_list
            // to implement a const-initialiser; the C shims fill each slot in
            // __lsm_init() below before any hook can fire.
            //
            // SAFETY: This array must be 'static — the static-call table holds
            // back-pointers (scall->hl) into it after registration.
            static mut __LSM_HOOKS: [
                ::core::mem::MaybeUninit<$crate::bindings::security_hook_list>;
                3
            ] = [::core::mem::MaybeUninit::zeroed(); 3];

            // The init function stored in lsm_info.init.  Called once by
            // security_init() in single-threaded boot context.
            unsafe extern "C" fn __lsm_init() -> ::core::ffi::c_int {
                // SAFETY: Called once, single-threaded, during security_init().
                // __LSM_HOOKS is exclusively owned here.
                unsafe {
                    // bindgen strips the rust_helper_ prefix from helper names.
                    $crate::bindings::lsm_hook_init_file_open(
                        __LSM_HOOKS[0].as_mut_ptr(),
                        Some($crate::lsm::Adapter::<$T>::file_open),
                    );
                    $crate::bindings::lsm_hook_init_task_alloc(
                        __LSM_HOOKS[1].as_mut_ptr(),
                        Some($crate::lsm::Adapter::<$T>::task_alloc),
                    );
                    $crate::bindings::lsm_hook_init_task_free(
                        __LSM_HOOKS[2].as_mut_ptr(),
                        Some($crate::lsm::Adapter::<$T>::task_free),
                    );
                    $crate::bindings::security_add_hooks(
                        __LSM_HOOKS[0].as_mut_ptr(),
                        __LSM_HOOKS.len() as ::core::ffi::c_int,
                        &raw const __LSM_ID.0,
                    );
                }
                0
            }

            // The lsm_info descriptor placed in the .lsm_info.init ELF section.
            // The kernel's security_init() iterates this section to discover and
            // call the init function of every compiled-in LSM.
            // LsmInfo wraps lsm_info with `unsafe impl Sync` so it can be `static`.
            #[used]
            #[link_section = ".lsm_info.init"]
            static __LSM_INFO: $crate::lsm::LsmInfo = {
                // SAFETY: __LSM_ID and __lsm_init have 'static lifetime.
                unsafe {
                    $crate::lsm::new_lsm_info(
                        &raw const __LSM_ID.0,
                        Some(__lsm_init),
                    )
                }
            };
        }
    };
}
