// SPDX-License-Identifier: GPL-2.0

//! Provides [`Class`] to group wound/wait mutexes to be acquired together
//! and specifies which deadlock avoidance algorithm to use (e.g., wound-wait
//! or wait-die).
//!
//! The [`define_ww_class!`] and [`define_wd_class!`] macros provide safe
//! ways to create classes.

use crate::bindings;
use crate::prelude::*;
use crate::types::Opaque;

/// Defines a static wound-wait [`Class`].
///
/// # Examples
///
/// ```
/// use kernel::define_ww_class;
///
/// define_ww_class!(SOME_WW_CLASS);
/// ```
#[macro_export]
macro_rules! define_ww_class {
    ($name:ident) => {
        static $name: $crate::sync::lock::ww_mutex::Class =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe {
                $crate::sync::lock::ww_mutex::Class::new_unpinned(
                    $crate::c_str!(::core::stringify!($name)),
                    false,
                )
            };
    };
}

/// Defines a static wait-die [`Class`].
///
/// # Examples
///
/// ```
/// use kernel::define_wd_class;
///
/// define_wd_class!(SOME_WD_CLASS);
/// ```
#[macro_export]
macro_rules! define_wd_class {
    ($name:ident) => {
        static $name: $crate::sync::lock::ww_mutex::Class =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe {
                $crate::sync::lock::ww_mutex::Class::new_unpinned(
                    $crate::c_str!(::core::stringify!($name)),
                    true,
                )
            };
    };
}

/// Used to group mutexes together for deadlock avoidance.
///
/// All mutexes that might be acquired together should use the same class.
///
/// # Examples
///
/// ```
/// use kernel::{define_ww_class, define_wd_class};
///
/// define_ww_class!(SOME_WW_CLASS);
/// define_wd_class!(SOME_WD_CLASS);
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
#[repr(transparent)]
pub struct Class {
    #[pin]
    pub(super) inner: Opaque<bindings::ww_class>,
}

impl Class {
    /// Creates an unpinned [`Class`].
    ///
    /// You should prefer using [`define_ww_class!`] and [`define_wd_class!`]
    /// macros. This function is `pub` only so that those macros can use it.
    /// The alternative would be to expose the private fields of [`Class`]
    /// which is less desirable.
    ///
    /// # Safety
    ///
    /// Caller must guarantee that the returned value must be pinned before
    /// its first use.
    pub const unsafe fn new_unpinned(name: &'static CStr, is_wait_die: bool) -> Self {
        Class {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_ptr().cast(),
                mutex_name: name.as_ptr().cast(),
                is_wait_die: is_wait_die as u32,
                // TODO: Replace with `bindings::lock_class_key::default()` once
                // stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with
                // `DEFINE_WD_CLASS` globally on C side.
                //
                // For reference, see __WW_CLASS_INITIALIZER() in
                // "include/linux/ww_mutex.h".
                acquire_key: unsafe { core::mem::zeroed() },
                // TODO: Replace with `bindings::lock_class_key::default()` once
                // stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with
                // `DEFINE_WD_CLASS` globally on C side.
                //
                // For reference, see __WW_CLASS_INITIALIZER() in
                // "include/linux/ww_mutex.h".
                mutex_key: unsafe { core::mem::zeroed() },
            }),
        }
    }

    /// Creates a [`Class`] from a raw pointer.
    ///
    /// This function is intended for interoperability with C code.
    ///
    /// # Safety
    ///
    /// The caller must ensure that `ptr` points to the `inner` field of
    /// [`Class`] and that it remains valid for the lifetime `'a`.
    pub const unsafe fn from_raw<'a>(ptr: *mut bindings::ww_class) -> &'a Self {
        // SAFETY: By the safety contract, `ptr` is valid to construct `Class`.
        unsafe { &*ptr.cast() }
    }
}

// SAFETY: [`Class`] is set up once and never modified. It's fine to share it across threads.
unsafe impl Sync for Class {}
// SAFETY: Doesn't hold anything thread-specific. It's safe to send to other threads.
unsafe impl Send for Class {}
