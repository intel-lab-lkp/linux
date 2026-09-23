// SPDX-License-Identifier: GPL-2.0

//! Sampled benchmarks with in-kernel statistics.
//!
//! Each sample times `iterations` of a workload. A [`Row`] prints the minimum, median, maximum and
//! mean of the sample times in nanoseconds. The caller owns the timer and the printing:
//!
//! ```ignore
//! let mut bench = Bencher::new(samples, entries)?;
//! pr_info!("{samples} samples x {entries} entries, ns per sample:\n");
//! pr_info!("{}\n", bench::Heading);
//! pr_info!("{}\n", bench.run("store", XArray::new, store));
//! pr_info!("total runtime {}\n", bench.runtime());
//! ```

use crate::{
    fmt,
    prelude::*,
    time::{Delta, Instant, Monotonic}, //
};

/// The column headings of a table of [`Row`]s, in the same columns.
pub struct Heading;

impl fmt::Display for Heading {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_fmt(fmt!(
            "{:<18} {:>12} {:>12} {:>12} {:>12} {:>12}",
            "benchmark",
            "min",
            "median",
            "max",
            "mean",
            "runtime"
        ))
    }
}

/// A wall time.
pub struct Runtime(pub Delta);

impl fmt::Display for Runtime {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let ms = self.0.as_millis();
        let secs = ms / 1000;
        let mins = secs / 60;
        let hours = mins / 60;
        let width = f.width().unwrap_or(0);
        if hours > 0 {
            let w = width.saturating_sub(4);
            write!(f, "{hours:w$}h{:02}m", mins % 60)
        } else if mins > 0 {
            let w = width.saturating_sub(4);
            write!(f, "{mins:w$}m{:02}s", secs % 60)
        } else {
            let w = width.saturating_sub(5);
            write!(f, "{secs:w$}.{:03}s", ms % 1000)
        }
    }
}

/// Stats across samples, in nanoseconds.
pub struct Stats {
    /// The fastest sample.
    pub min: i64,
    /// The middle sample.
    pub median: i64,
    /// The slowest sample.
    pub max: i64,
    /// The mean of the samples, rounded down.
    pub mean: i64,
}

impl Stats {
    /// Computes the statistics of the non-empty `samples`, sorting them in place.
    pub fn new(samples: &mut [i64]) -> Self {
        samples.sort_unstable();
        let len = samples.len();
        Self {
            min: samples[0],
            median: (samples[(len - 1) / 2] + samples[len / 2]) / 2,
            max: samples[len - 1],
            mean: samples.iter().sum::<i64>() / len as i64,
        }
    }
}

/// One row of the table: the benchmark's name, its statistics and its wall time, in the columns
/// of [`Heading`].
pub struct Row<'a> {
    name: &'a str,
    stats: Stats,
    runtime: Runtime,
}

impl fmt::Display for Row<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_fmt(fmt!(
            "{:<18} {:>12} {:>12} {:>12} {:>12} {:>12}",
            self.name,
            self.stats.min,
            self.stats.median,
            self.stats.max,
            self.stats.mean,
            self.runtime
        ))
    }
}

/// Benchmark runner.
pub struct Bencher {
    iterations: usize,
    timings: KVVec<i64>,
    runtime: Delta,
}

impl Bencher {
    /// Allocates one timing slot per sample up front, outside the timed loops.
    ///
    /// Returns `EINVAL` if `samples` or `iterations` is 0.
    pub fn new(samples: usize, iterations: usize) -> Result<Self> {
        if samples == 0 || iterations == 0 {
            return Err(EINVAL);
        }
        Ok(Self {
            iterations,
            timings: KVVec::from_elem(0, samples, GFP_KERNEL)?,
            runtime: Delta::ZERO,
        })
    }

    /// Runs `bench` on a fresh `setup` value once per sample and returns the table row of `name`.
    ///
    /// `bench` returns the [`Delta`] of the window it timed.
    pub fn run<'a, A>(
        &mut self,
        name: &'a str,
        setup: impl Fn() -> A,
        bench: impl Fn(A, usize) -> Delta,
    ) -> Row<'a> {
        let start = Instant::<Monotonic>::now();
        for ns in &mut self.timings {
            *ns = bench(setup(), self.iterations).as_nanos();
        }
        let elapsed = start.elapsed();
        self.runtime += elapsed;

        Row {
            name,
            stats: Stats::new(&mut self.timings),
            runtime: Runtime(elapsed),
        }
    }

    /// The wall time of every run so far.
    pub fn runtime(&self) -> Runtime {
        Runtime(self.runtime)
    }
}

#[macros::kunit_tests(rust_bench)]
mod tests {
    use super::*;

    #[test]
    fn stats() {
        let odd = Stats::new(&mut [5, 1, 9]);
        assert_eq!((odd.min, odd.median, odd.max, odd.mean), (1, 5, 9, 5));
        // Median check for even samples.
        let med = Stats::new(&mut [4, 1, 9, 5]);
        assert_eq!(med.median, 4);
    }
}
