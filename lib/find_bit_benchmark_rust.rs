// SPDX-License-Identifier: GPL-2.0
//! Benchmark for find_bit-like methods in Bitmap Rust API.

use kernel::alloc::flags::GFP_KERNEL;
use kernel::bindings;
use kernel::bitmap::Bitmap;
use kernel::error::{code, Result};
use kernel::prelude::module;
use kernel::time::Ktime;
use kernel::ThisModule;
use kernel::{pr_cont, pr_err};

const BITMAP_LEN: usize = 4096 * 8 * 10;
// Reciprocal of the fraction of bits that are set in sparse bitmap.
const SPARSENESS: usize = 500;

/// Test module that benchmarks performance of traversing bitmaps.
struct Benchmark();

fn test_next_bit(bitmap: &Bitmap) {
    let mut time = Ktime::ktime_get();
    let mut cnt = 0;
    let mut i = 0;

    while let Some(index) = bitmap.next_bit(i) {
        cnt += 1;
        i = index + 1;
        // CONFIG_RUST_BITMAP_HARDENED enforces strict bounds.
        if i == BITMAP_LEN {
            break;
        }
    }

    time = Ktime::ktime_get() - time;
    pr_cont!(
        "next_bit:           {:18} ns, {:6} iterations\n",
        time.to_ns(),
        cnt
    );
}

fn test_next_zero_bit(bitmap: &Bitmap) {
    let mut time = Ktime::ktime_get();
    let mut cnt = 0;
    let mut i = 0;

    while let Some(index) = bitmap.next_zero_bit(i) {
        cnt += 1;
        i = index + 1;
        // CONFIG_RUST_BITMAP_HARDENED enforces strict bounds.
        if i == BITMAP_LEN {
            break;
        }
    }

    time = Ktime::ktime_get() - time;
    pr_cont!(
        "next_zero_bit:      {:18} ns, {:6} iterations\n",
        time.to_ns(),
        cnt
    );
}

fn find_bit_test() {
    pr_err!("\n");
    pr_cont!("Start testing find_bit() Rust with random-filled bitmap\n");

    let mut bitmap = Bitmap::new(BITMAP_LEN, GFP_KERNEL).expect("alloc bitmap failed");
    bitmap.fill_random();

    test_next_bit(&bitmap);
    test_next_zero_bit(&bitmap);

    pr_cont!("Start testing find_bit() Rust with sparse bitmap\n");

    let mut bitmap = Bitmap::new(BITMAP_LEN, GFP_KERNEL).expect("alloc sparse bitmap failed");
    let nbits = BITMAP_LEN / SPARSENESS;
    for _i in 0..nbits {
        // SAFETY: BITMAP_LEN fits in 32 bits.
        let bit: usize =
            unsafe { bindings::__get_random_u32_below(BITMAP_LEN.try_into().unwrap()) as _ };
        bitmap.set_bit(bit);
    }

    test_next_bit(&bitmap);
    test_next_zero_bit(&bitmap);
}

impl kernel::Module for Benchmark {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        find_bit_test();
        // Return error so test module can be inserted again without rmmod.
        Err(code::EINVAL)
    }
}

module! {
    type: Benchmark,
    name: "find_bit_benchmark_rust",
    authors: ["Burak Emir <bqe@google.com>"],
    description: "Module with benchmark for bitmap Rust API",
    license: "GPL v2",
}
