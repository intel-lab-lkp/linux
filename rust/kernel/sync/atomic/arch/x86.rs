// SPDX-License-Identifier: GPL-2.0

//! x86 implementation for atomic and barrier primitives.

use core::arch::asm;
use core::cell::UnsafeCell;

/// Generates an instruction with "lock" prefix.
#[cfg(CONFIG_SMP)]
macro_rules! lock_instr {
    ($i:literal) => { concat!("lock; ", $i) }
}

#[cfg(not(CONFIG_SMP))]
macro_rules! lock_instr {
    ($i:literal) => { $i }
}

/// Atomically exchanges and adds `i` to `*v` in a wrapping way.
///
/// Return the old value before the addition.
///
/// # Safety
///
/// The caller need to make sure `v` points to a valid `i32`.
unsafe fn i32_xadd(v: *mut i32, mut i: i32) -> i32 {
    // SAFETY: Per function safety requirement, the address of `v` is valid for "xadd".
    unsafe {
        asm!(
            lock_instr!("xaddl {i:e}, ({v})"),
            i = inout(reg) i,
            v = in(reg) v,
            options(att_syntax, preserves_flags),
        );
    }

    i
}

pub(crate) fn i32_fetch_add_relaxed(v: &UnsafeCell<i32>, i: i32) -> i32 {
    // SAFETY: `v.get()` points to a valid `i32`.
    unsafe { i32_xadd(v.get(), i) }
}

pub(crate) fn i32_fetch_sub_release(v: &UnsafeCell<i32>, i: i32) -> i32 {
    // SAFETY: `v.get()` points to a valid `i32`.
    unsafe { i32_xadd(v.get(), i.wrapping_neg()) }
}
