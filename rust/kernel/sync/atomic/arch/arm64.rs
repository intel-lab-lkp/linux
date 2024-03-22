// SPDX-License-Identifier: GPL-2.0

//! ARM64 implementation for atomic and barrier primitives.

use core::arch::asm;
use core::cell::UnsafeCell;

pub(crate) fn i32_fetch_add_relaxed(v: &UnsafeCell<i32>, i: i32) -> i32 {
    let mut result;
    unsafe {
        asm!(
            "prfm    pstl1strm, [{v}]",
            "1: ldxr {result:w}, [{v}]",
            "add {val:w}, {result:w}, {i:w}",
            "stxr {tmp:w}, {val:w}, [{v}]",
            "cbnz {tmp:w}, 1b",
            result = out(reg) result,
            tmp = out(reg) _,
            val = out(reg) _,
            v = in(reg) v.get(),
            i = in(reg) i,
        )
    }

    result
}

pub(crate) fn i32_fetch_sub_release(v: &UnsafeCell<i32>, i: i32) -> i32 {
    let mut result;
    unsafe {
        asm!(
            "prfm    pstl1strm, [{v}]",
            "1: ldxr {result:w}, [{v}]",
            "sub {val:w}, {result:w}, {i:w}",
            "stlxr {tmp:w}, {val:w}, [{v}]",
            "cbnz {tmp:w}, 1b",
            result = out(reg) result,
            tmp = out(reg) _,
            val = out(reg) _,
            v = in(reg) v.get(),
            i = in(reg) i,
        )
    }

    result
}
