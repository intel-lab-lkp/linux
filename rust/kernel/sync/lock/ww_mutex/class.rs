// SPDX-License-Identifier: GPL-2.0

//! Provides [`Class`] to group wound/wait mutexes to be acquired together
//! and specifies which deadlock avoidance algorithm to use (e.g., wound-wait
//! or wait-die).
//!
//! The [`define_class`] macro and [`Class::new_wait_die`]/[`Class::new_wound_wait`]
//! constructors provide safe ways to create classes.

use crate::bindings;
use crate::prelude::*;
use crate::types::Opaque;

/// Creates static [`Class`] instances.
///
/// # Examples
///
/// ```
/// use kernel::{c_str, define_class};
///
/// define_class!(WOUND_WAIT_GLOBAL_CLASS, wound_wait, c_str!("wound_wait_global_class"));
/// define_class!(WAIT_DIE_GLOBAL_CLASS, wait_die, c_str!("wait_die_global_class"));
/// ```
#[macro_export]
macro_rules! define_class {
    ($name:ident, wound_wait, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::Class =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::Class::unpinned_new($class_name, false) };
    };
    ($name:ident, wait_die, $class_name:expr) => {
        static $name: $crate::sync::lock::ww_mutex::Class =
            // SAFETY: This is `static`, so address is fixed and won't move.
            unsafe { $crate::sync::lock::ww_mutex::Class::unpinned_new($class_name, true) };
    };
}

/// Used to group mutexes together for deadlock avoidance.
///
/// All mutexes that might be acquired together should use the same class.
///
/// # Examples
///
/// ```
/// use kernel::sync::lock::ww_mutex::Class;
/// use kernel::c_str;
/// use pin_init::stack_pin_init;
///
/// stack_pin_init!(let _wait_die_class = Class::new_wait_die(c_str!("some_class")));
/// stack_pin_init!(let _wound_wait_class = Class::new_wound_wait(c_str!("some_other_class")));
///
/// # Ok::<(), Error>(())
/// ```
#[pin_data]
#[repr(transparent)]
pub struct Class {
    #[pin]
    pub(super) inner: Opaque<bindings::ww_class>,
}

// SAFETY: [`Class`] is set up once and never modified. It's fine to share it across threads.
unsafe impl Sync for Class {}
// SAFETY: Doesn't hold anything thread-specific. It's safe to send to other threads.
unsafe impl Send for Class {}

impl Class {
    /// Creates an unpinned [`Class`].
    ///
    /// # Safety
    ///
    /// Caller must guarantee that the returned value is not moved after creation.
    pub const unsafe fn unpinned_new(name: &'static CStr, is_wait_die: bool) -> Self {
        Class {
            inner: Opaque::new(bindings::ww_class {
                stamp: bindings::atomic_long_t { counter: 0 },
                acquire_name: name.as_char_ptr(),
                mutex_name: name.as_char_ptr(),
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

    /// Creates a [`Class`].
    ///
    /// You should not use this function directly. Use the [`define_class!`]
    /// macro or call [`Class::new_wait_die`] or [`Class::new_wound_wait`] instead.
    fn new(name: &'static CStr, is_wait_die: bool) -> impl PinInit<Self> {
        pin_init! {
            Self {
                inner <- Opaque::ffi_init(|slot: *mut bindings::ww_class| {
                    // SAFETY: The fields are being initialized. The `name` pointer is valid for a
                    // static lifetime. The keys are zeroed, which is what the C side does.
                    unsafe {
                        slot.write(bindings::ww_class {
                            stamp: bindings::atomic_long_t { counter: 0 },
                            acquire_name: name.as_char_ptr(),
                            mutex_name: name.as_char_ptr(),
                            is_wait_die: is_wait_die.into(),
                            // TODO: Replace with `bindings::lock_class_key::default()` once
                            // stabilized for `const`.
                            //
                            // SAFETY: This is always zero-initialized when defined with
                            // `DEFINE_WD_CLASS` globally on C side.
                            //
                            // For reference, see __WW_CLASS_INITIALIZER() in
                            // "include/linux/ww_mutex.h".
                            acquire_key: core::mem::zeroed(),
                            mutex_key: core::mem::zeroed(),
                        });
                    }
                }),
            }
        }
    }

    /// Creates wait-die [`Class`].
    pub fn new_wait_die(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, true)
    }

    /// Creates wound-wait [`Class`].
    pub fn new_wound_wait(name: &'static CStr) -> impl PinInit<Self> {
        Self::new(name, false)
    }

    /// Creates a `Class` from a raw pointer.
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
