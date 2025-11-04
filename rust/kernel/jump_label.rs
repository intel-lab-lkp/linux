// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2024 Google LLC.

//! Logic for static keys.
//!
//! C header: [`include/linux/jump_label.h`](srctree/include/linux/jump_label.h).

/// The key used for the static_key_false/true.
///
/// If the key just contains a static_key, like: `struct tracepoint`;
///
/// ```
/// pub struct tracepoint {
///     ...,
///     key: static_key,
///     ...,
/// }
///
/// // When you use the tracepoint as the parameter.
/// if static_branch_unlikely!(tp, tracepoint, key) {
///     // Do something
/// }
///
/// // It just like:
/// let _key: *const crate::bindings::tracepoint = ::core::ptr::addr_of!($key);
/// let _key: *const crate::bindings::static_key_false = ::core::ptr::addr_of!((*_key).key);
/// let _key: *const crate::bindings::static_key = _key.cast();
/// ```
///
/// If the key just contains a single static_key, like: `struct static_key_false`;
///
/// ```
/// pub struct static_key_false {
///     key: static_key,
/// }
///
/// // When you use the static_key_false as the parameter.
/// if static_branch_unlikely!(key) {
///     // Do something
/// }
///
/// // It just like:
/// let _key: *const crate::bindings::static_key_false = ::core::ptr::addr_of!($key);
/// let _key: *const crate::bindings::static_key = _key.cast();
/// ```
///
macro_rules! __static_branch_base {
    ($basety:ty, $branch:expr, $key:path) => {{
        let _key: *const $basety = ::core::ptr::addr_of!($key);
        let _key: *const $crate::bindings::static_key = _key.cast();

        #[cfg(not(CONFIG_JUMP_LABEL))]
        {
            $crate::bindings::static_key_count(_key.cast_mut()) > 0
        }

        #[cfg(CONFIG_JUMP_LABEL)]
        {
            $crate::jump_label::arch_static_branch! { _key, $branch }
        }
    }};
    ($basety:ty, $branch:expr, $key:path, $keytyp:ty, $field:ident) => {{
        let _key: *const $keytyp = ::core::ptr::addr_of!($key);
        let _key: *const $basety = ::core::ptr::addr_of!((*_key).$field);
        let _key: *const $crate::bindings::static_key = _key.cast();

        #[cfg(not(CONFIG_JUMP_LABEL))]
        {
            $crate::bindings::static_key_count(_key.cast_mut()) > 0
        }

        #[cfg(CONFIG_JUMP_LABEL)]
        {
            $crate::jump_label::arch_static_branch! { _key, $branch }
        }
    }};
}

/// Branch based on a static key.
///
/// Takes two type arguments:
///
/// First Type takes one argument:
///
/// * `key` - the static variable containing the `static_key`.
///
/// Second Type takes three arguments:
///
/// * `key` - the path to the static variable containing the `static_key`.
/// * `keytyp` - the type of `key`.
/// * `field` - the name of the field of `key` that contains the `static_key`.
///
/// # Safety
///
/// ```
/// let tp: tracepoint = tracepoint::new();
/// if static_key_likely!(tp, tracepoint, key) {
///     // Do something
/// }
///
/// let key: static_key_false = static_key_true::new();
/// if static_key_likely!(key) {
///     // Do something
/// }
/// ```
///
/// The macro must be used with a real static key defined by C.
#[macro_export]
macro_rules! static_branch_likely {
    ($key:path) => {{
        __static_branch_base! { $crate::bindings::static_key_true, true, $key }
    }};
    ($key:path, $keytyp:ty, $field:ident) => {{
        __static_branch_base! { $crate::bindings::static_key_true, true, $key, $keytyp, $field }
    }};
}
pub use static_branch_likely;

/// Branch based on a static key.
///
/// Takes two type arguments:
///
/// First Type takes one argument:
///
/// * `key` - the static variable containing the `static_key`.
///
/// Second Type takes three arguments:
///
/// * `key` - the path to the static variable containing the `static_key`.
/// * `keytyp` - the type of `key`.
/// * `field` - the name of the field of `key` that contains the `static_key`.
///
/// # Safety
///
/// ```
/// let tp: tracepoint = tracepoint::new();
/// if static_key_unlikely!(tp, tracepoint, key) {
///     // Do something
/// }
///
/// let key: static_key_false = static_key_false::new();
/// if static_key_unlikely!(key) {
///     // Do something
/// }
/// ```
///
/// The macro must be used with a real static key defined by C.
#[macro_export]
macro_rules! static_branch_unlikely {
    ($key:path) => {{
        static_branch_base! { $crate::bindings::static_key_false, false, $key }
    }};
    ($key:path, $keytyp:ty, $field:ident) => {{
        static_branch_base! { $crate::bindings::static_key_false, false, $key, $keytyp, $field }
    }};
}
pub use static_branch_unlikely;

/// Assert that the assembly block evaluates to a string literal.
#[cfg(CONFIG_JUMP_LABEL)]
const _: &str = include!(concat!(
    env!("OBJTREE"),
    "/rust/kernel/generated_arch_static_branch_asm.rs"
));

#[macro_export]
#[doc(hidden)]
#[cfg(CONFIG_JUMP_LABEL)]
macro_rules! arch_static_branch {
    ($key:path, $branch:expr) => {'my_label: {
        $crate::asm!(
            include!(concat!(env!("OBJTREE"), "/rust/kernel/generated_arch_static_branch_asm.rs"));
            l_yes = label {
                break 'my_label true;
            },
            real_key = sym $key,
            branch = const $crate::jump_label::bool_to_int($branch),
        );

        break 'my_label false;
    }};
}

#[cfg(CONFIG_JUMP_LABEL)]
pub use arch_static_branch;

/// A helper used by inline assembly to pass a boolean to as a `const` parameter.
///
/// Using this function instead of a cast lets you assert that the input is a boolean, and not some
/// other type that can also be cast to an integer.
#[doc(hidden)]
pub const fn bool_to_int(b: bool) -> i32 {
    b as i32
}

/// Enable a static branch.
#[macro_export]
macro_rules! static_branch_enable {
    ($key:path) => {{
        let _key: *const $crate::bindings::static_key = ::core::ptr::addr_of!($key);
        $crate::bindings::static_key_enable(_key.cast_mut());
    }};
}

/// Disable a static branch.
#[macro_export]
macro_rules! static_branch_disable {
    ($key:path) => {{
        let _key: *const $crate::bindings::static_key = ::core::ptr::addr_of!($key);
        $crate::bindings::static_key_disable(_key.cast_mut());
    }};
}
