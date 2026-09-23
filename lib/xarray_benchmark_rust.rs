// SPDX-License-Identifier: GPL-2.0
//
//! Benchmark for the XArray Rust APIs.

use kernel::{
    bench::{self, Bencher},
    prelude::*,
    rxarray::{self, XArray4, XArray6},
    time::{Delta, Instant, Monotonic},
    xarray::{self, AllocKind}, //
};

/// Stores integer `entries` at `0..entries` in the empty `xa` and returns the time taken.
fn store_int<const SHIFT: usize, const SIZE: usize>(
    mut xa: rxarray::XArray<KBox<u64>, SHIFT, SIZE>,
    entries: usize,
) -> Delta {
    let time = Instant::<Monotonic>::now();
    for i in 0..entries {
        let entry = rxarray::Entry::try_int(i * 10).expect("the value fits in an integer entry");
        xa.store(i, entry, GFP_KERNEL).expect("store");
    }
    time.elapsed()
}

/// Stores pointer `entries` at `0..entries` in the empty `xa` and returns the time taken.
fn store_ptr<const SHIFT: usize, const SIZE: usize>(
    mut xa: rxarray::XArray<KBox<u64>, SHIFT, SIZE>,
    entries: usize,
) -> Delta {
    let time = Instant::<Monotonic>::now();
    for i in 0..entries {
        let entry = rxarray::Entry::Pointer(KBox::new(i as u64, GFP_KERNEL).expect("allocation"));
        xa.store(i, entry, GFP_KERNEL).expect("store");
    }
    time.elapsed()
}

/// Allocates the empty C XArray that [`store_ptr_xarray`] stores into.
fn new_ptr_xarray() -> Pin<KBox<xarray::XArray<KBox<u64>>>> {
    KBox::pin_init(xarray::XArray::new(AllocKind::Alloc), GFP_KERNEL).expect("allocation")
}

/// Stores pointer `entries` at `0..entries` in the empty C XArray `xa` and returns the
/// time taken.
fn store_ptr_xarray(xa: Pin<KBox<xarray::XArray<KBox<u64>>>>, entries: usize) -> Delta {
    let time = Instant::<Monotonic>::now();
    for i in 0..entries {
        let value = KBox::new(i as u64, GFP_KERNEL).expect("allocation");
        xa.lock()
            .store(i, value, GFP_KERNEL)
            .map_err(|e| e.error)
            .expect("store");
    }
    time.elapsed()
}

/// Runs every benchmark.
fn benchmark(samples: usize, entries: usize) -> Result {
    let mut bench = Bencher::new(samples, entries)?;

    pr_info!("{samples} samples x {entries} entries, ns per sample:\n");
    pr_info!("{}\n", bench::Heading);
    pr_info!(
        "{}\n",
        bench.run("store_int_rxarray4", XArray4::<KBox<u64>>::new, store_int)
    );
    pr_info!(
        "{}\n",
        bench.run("store_int_rxarray6", XArray6::<KBox<u64>>::new, store_int)
    );
    pr_info!(
        "{}\n",
        bench.run("store_ptr_rxarray4", XArray4::<KBox<u64>>::new, store_ptr)
    );
    pr_info!(
        "{}\n",
        bench.run("store_ptr_rxarray6", XArray6::<KBox<u64>>::new, store_ptr)
    );
    pr_info!(
        "{}\n",
        bench.run("store_ptr_xarray", new_ptr_xarray, store_ptr_xarray)
    );
    pr_info!("total runtime {}\n", bench.runtime());
    Ok(())
}

/// The benchmark module.
struct Benchmark;

impl kernel::Module for Benchmark {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        let samples = module_parameters::samples.value();
        let entries = module_parameters::entries.value();
        benchmark(samples, entries)?;

        Ok(Benchmark)
    }
}

module! {
    type: Benchmark,
    name: "xarray_benchmark_rust",
    authors: ["Daniel Gomez <da.gomez@samsung.com>"],
    description: "Benchmark: XArray",
    license: "GPL v2",
    params: {
        samples: usize {
            default: 100,
            description: "Timed runs per benchmark",
        },
        entries: usize {
            default: 100_000,
            description: "Entries stored per run",
        },
    },
}
