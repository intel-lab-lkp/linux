// SPDX-License-Identifier: GPL-2.0

// Copyright (C) 2026 Google LLC.

//! Binder debugging helpers.

#![allow(dead_code)]

pub(crate) const BINDER_DEBUG_USER_ERROR: u32 = 1 << 0;
pub(crate) const BINDER_DEBUG_FAILED_TRANSACTION: u32 = 1 << 1;
pub(crate) const BINDER_DEBUG_DEAD_TRANSACTION: u32 = 1 << 2;
pub(crate) const BINDER_DEBUG_OPEN_CLOSE: u32 = 1 << 3;
pub(crate) const BINDER_DEBUG_DEAD_BINDER: u32 = 1 << 4;
pub(crate) const BINDER_DEBUG_DEATH_NOTIFICATION: u32 = 1 << 5;
pub(crate) const BINDER_DEBUG_READ_WRITE: u32 = 1 << 6;
pub(crate) const BINDER_DEBUG_USER_REFS: u32 = 1 << 7;
pub(crate) const BINDER_DEBUG_THREADS: u32 = 1 << 8;
pub(crate) const BINDER_DEBUG_TRANSACTION: u32 = 1 << 9;
pub(crate) const BINDER_DEBUG_TRANSACTION_COMPLETE: u32 = 1 << 10;
pub(crate) const BINDER_DEBUG_FREE_BUFFER: u32 = 1 << 11;
pub(crate) const BINDER_DEBUG_INTERNAL_REFS: u32 = 1 << 12;
pub(crate) const BINDER_DEBUG_PRIORITY_CAP: u32 = 1 << 13;
pub(crate) const BINDER_DEBUG_SPINLOCKS: u32 = 1 << 14;

extern "C" {
    static rust_binder_debug_mask: u32;
}

/// Checks if the given debug logging category is enabled in the mask.
pub(crate) fn debug_mask_enabled(mask: u32) -> bool {
    let ptr = &raw const rust_binder_debug_mask;
    // SAFETY: `rust_binder_debug_mask` is defined in the companion C code linked in the module.
    let current_mask = unsafe { core::ptr::read_volatile(ptr) };
    (current_mask & mask) != 0
}

/// Prints a debug log if the specified mask category is enabled.
#[macro_export]
macro_rules! binder_debug {
    ($mask:expr, $fmt:literal $(, $($arg:tt)*)?) => {
        if $crate::debug::debug_mask_enabled($mask) {
            kernel::pr_info!(
                "{}: {}\n",
                kernel::current!().pid(),
                format_args!($fmt $(, $($arg)*)?)
            );
        }
    };
}
