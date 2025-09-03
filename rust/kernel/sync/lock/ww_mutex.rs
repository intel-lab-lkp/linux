// SPDX-License-Identifier: GPL-2.0

//! A kernel Wound/Wait Mutex.
//!
//! This module provides Rust abstractions for the Linux kernel's `ww_mutex` implementation,
//! which provides deadlock avoidance through a wait-wound or wait-die algorithm.
//!
//! C header: [`include/linux/ww_mutex.h`](srctree/include/linux/ww_mutex.h)
//!
//! For more information: <https://docs.kernel.org/locking/ww-mutex-design.html>

use crate::bindings;
use crate::prelude::*;
use crate::types::Opaque;

/// Create static [`WwClass`] instances.
///
/// # Examples
///
/// ```
/// use kernel::{c_str, define_ww_class};
///
/// define_ww_class!(WOUND_WAIT_GLOBAL_CLASS, wound_wait, c_str!("wound_wait_global_class"));
/// define_ww_class!(WAIT_DIE_GLOBAL_CLASS, wait_die, c_str!("wait_die_global_class"));
/// ```
#[macro_export]
macro_rules! define_ww_class {
    ($name:ident, wound_wait, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::WwClass::unpinned_new($class_name, false) };
    };
    ($name:ident, wait_die, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::WwClass =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::WwClass::unpinned_new($class_name, true) };
    };
}

/// A class used to group mutexes together for deadlock avoidance.
///
/// All mutexes that might be acquired together should use the same class.
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::WwClass;
/// use kernel::c_str;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let _wait_die_class = WwClass::new_wait_die(c_str!("graphics_buffers")));
/// stack_pin_init!(let _wound_wait_class = WwClass::new_wound_wait(c_str!("memory_pools")));
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
pub struct WwClass {
    #[pin]
    inner: Opaque<bindings::ww_class>,
}

// SAFETY: [`WwClass`] is set up once and never modified. It's fine to share it across threads.
unsafe impl Sync for WwClass {}
// SAFETY: Doesn't hold anything thread-specific. It's safe to send to other threads.
unsafe impl Send for WwClass {}

impl WwClass {
    /// Creates an unpinned [`WwClass`].
    ///
    /// # Safety
    ///
    /// Caller must guarantee that the returned value is not moved after creation.
    pub const unsafe fn unpinned_new(name: &'static CStr, is_wait_die: bool) -> Self {
        WwClass {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_char_ptr(),
                mutex_name: name.as_char_ptr(),
                is_wait_die: is_wait_die as u32,
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                acquire_key: unsafe { core::mem::zeroed() },
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                mutex_key: unsafe { core::mem::zeroed() },
            }),
        }
    }

    /// Creates a [`WwClass`].
    ///
    /// You should not use this function directly. Use the [`define_ww_class!`]
    /// macro or call [`WwClass::new_wait_die`] or [`WwClass::new_wound_wait`] instead.
    const fn new(name: &'static CStr, is_wait_die: bool) -> Self {
        WwClass {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_char_ptr(),
                mutex_name: name.as_char_ptr(),
                is_wait_die: is_wait_die as u32,
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                acquire_key: unsafe { core::mem::zeroed() },
                // TODO: Replace with `bindings::lock_class_key::default()` once stabilized for `const`.
                //
                // SAFETY: This is always zero-initialized when defined with `DEFINE_WD_CLASS`
                // globally on C side.
                //
                // Ref: <https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/linux/ww_mutex.h?h=v6.16-rc2#n85>
                mutex_key: unsafe { core::mem::zeroed() },
            }),
        }
    }

    /// Creates wait-die [`WwClass`].
    pub fn new_wait_die(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, true)
    }

    /// Creates wound-wait [`WwClass`].
    pub fn new_wound_wait(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, false)
    }
}
