// SPDX-License-Identifier: GPL-2.0

//! Bindings for Rust example LSM.
//!
//! Implements the bare necessities to implement an LSM in Rust and register it, using the bindings
//! to C. Long term this should move to the kernel crate, but since these bindings are currently
//! specific to a single LSM, instead of allowing arbitrary LSMs to get registered, they live here
//! for now.

use kernel::{bindings, c_str, ffi::*, fs::LocalFile, prelude::*, types::Opaque};

/// The name the LSM gets registered under.
const NAME: &CStr = c_str!("rust_lsm");

/// The amount of hooks that get registered by this LSM.
///
/// This could be automatically calculated from the length of the array, but would then still need
/// to manually be adjusted there. Having it as a separate constant simplifies access for calling
/// [`security_add_hooks`] though.
///
/// This value is `usize` so that it can be used in the array directly, but [`security_add_hooks`]
/// requires `i32`, which is why [`SECURITY_HOOK_LIST_COUNT`] exists.
///
/// [`security_add_hooks`]: bindings::security_add_hooks
const SECURITY_HOOK_LIST_LEN: usize = 1;

/// The amount of hooks that get registered by this LSM.
///
/// This time as `i32`, because that's what [`security_add_hooks`] requires. Const-converting this
/// makes sure an overflow results in a compiler error, although having that many hooks is unlikely.
///
/// [`security_add_hooks`]: bindings::security_add_hooks
const SECURITY_HOOK_LIST_COUNT: i32 = {
    // Manually implement i32::try_from(...).unwrap() here because it is not const
    if SECURITY_HOOK_LIST_LEN > (i32::MAX as usize) {
        panic!("Cannot register more than i32::MAX LSM hooks!")
    } else {
        SECURITY_HOOK_LIST_LEN as i32
    }
};

/// Wrapper to be able to use `lsm_id` in a static context.
#[repr(transparent)]
struct LsmId(Opaque<bindings::lsm_id>);

// SAFETY: There is only a static instance and in that one the pointer field points to an immutable
// C string.
unsafe impl Sync for LsmId {}

/// Wrapper to be able to use `lsm_info` in a static context.
// Needs to be aligned to `size_of::<kernel::ffi::c_ulong>()`, but Rust attributes cannot express
// this and even statically setting it to 8 cannot be combined with `repr(transparent)`.
#[repr(transparent)]
struct LsmInfo(Opaque<bindings::lsm_info>);

// SAFETY: There is only a static instance and in that one the pointer fields point to an immutable
// C string and the init function defined here.
unsafe impl Sync for LsmInfo {}

/// Wrapper to be able to use `security_hook_list` in a static context.
///
/// This is `Opaque<[...]>` instead of `[Opaque<...>]` because it simplifies the call to
/// [`bindings::security_add_hooks`] a lot.
#[repr(transparent)]
struct SecurityHookList(Opaque<[bindings::security_hook_list; SECURITY_HOOK_LIST_LEN]>);

// SAFETY: There is only a static instance, which is only modified from C during LSM initialization
// using the interior mutability of `Opaque`.
unsafe impl Sync for SecurityHookList {}

/// Static information about the LSM.
static LSM_ID: LsmId = LsmId(Opaque::new(bindings::lsm_id {
    name: NAME.as_char_ptr(),
    id: bindings::LSM_ID_RUST_LSM as _,
}));

/// Register the LSM in the kernel.
///
/// This is achieved by placing it in the `.lsm_info.init` linker section.
///
/// Everything but `name` and `init` is optional, but [`Default::default()`] cannot be used in a
/// const context.
#[used]
#[link_section = ".lsm_info.init"]
static LSM_INFO: LsmInfo = LsmInfo(Opaque::new(bindings::lsm_info {
    name: NAME.as_char_ptr(),
    init: Some(init),
    order: bindings::lsm_order_LSM_ORDER_MUTABLE,
    flags: 0,
    enabled: core::ptr::null_mut(),
    blobs: core::ptr::null_mut(),
}));

/// List of hooks to register callbacks for.
///
/// The data stored here is updated by the LSM code during LSM initialization using the interior
/// mutability of [`Opaque`].
#[used]
#[link_section = ".data..ro_after_init"]
static HOOKS: SecurityHookList = SecurityHookList(Opaque::new([bindings::security_hook_list {
    // SAFETY: Creates an unaligned pointer to the mutable static since the
    // `static_calls_table` is `repr(packed)`. The pointers are only used
    // from C code.
    scalls: unsafe { &raw mut bindings::static_calls_table.file_permission }.cast(),
    hook: bindings::security_list_options {
        file_permission: Some(file_permission),
    },
    lsmid: LSM_ID.0.get(),
}]));

/// Init function for the LSM
///
/// Gets called from the C side through the pointer stored in [`LSM_INFO`].
///
/// # Safety
///
/// This function must only be called once from the C LSM initialization code.
#[link_section = ".init.text"]
unsafe extern "C" fn init() -> c_int {
    pr_info!("Rust example LSM is starting...\n");

    // Register the hooks
    // SAFETY: FFI call to register the hooks for the LSM. All pointers point to statics which are
    // only accessed from this init and are therefore valid for the call. The hook list is modified
    // using the interior mutability of `Opaque`.
    unsafe {
        bindings::security_add_hooks(
            HOOKS.0.get().cast(),
            SECURITY_HOOK_LIST_COUNT,
            LSM_ID.0.get(),
        );
    }

    // Call the Rust side init function for further initialization
    if let Err(e) = super::init() {
        return e.to_errno();
    }

    pr_info!("Rust example LSM is initialized!\n");

    0
}

/// Callback for the `file_permission` hook.
///
/// This gets called every time an already opened file is read or written.
///
/// # Safety
///
/// May only be called by the LSM framework as `file_permission` hook with a file pointer valid for
/// the duration of the call.
unsafe extern "C" fn file_permission(file: *mut bindings::file, mask: c_int) -> c_int {
    // SAFETY: `file` is valid for the duration of this call
    let file = unsafe { LocalFile::from_raw_file(file) };

    match super::file_permission(file, mask) {
        Ok(true) => 0,
        Ok(false) => EPERM.to_errno(),
        Err(e) => e.to_errno(),
    }
}
