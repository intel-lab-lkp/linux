/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 HiSilicon Limited.
 */

#ifndef _KERNEL_DMA_BENCHMARK_H
#define _KERNEL_DMA_BENCHMARK_H

#define DMA_MAP_BENCHMARK       _IOWR('d', 1, struct map_benchmark)
#define DMA_MAP_MAX_THREADS     1024
#define DMA_MAP_MAX_SECONDS     300
#define DMA_MAP_MAX_TRANS_DELAY (10 * NSEC_PER_MSEC)

#define DMA_MAP_BIDIRECTIONAL   0
#define DMA_MAP_TO_DEVICE       1
#define DMA_MAP_FROM_DEVICE     2

enum {
	DMA_MAP_BENCH_SINGLE_MODE,
	DMA_MAP_BENCH_SG_MODE,
	DMA_MAP_BENCH_MODE_MAX
};

/**
 * struct map_benchmark - Benchmarking data for DMA mapping operations.
 * @avg_map_100ns: Average map latency in 100ns.
 * @map_stddev: Standard deviation of map latency.
 * @avg_unmap_100ns: Average unmap latency in 100ns.
 * @unmap_stddev: Standard deviation of unmap latency.
 * @threads: Number of threads performing map/unmap operations in parallel.
 * @seconds: Duration of the test in seconds.
 * @node: NUMA node on which this benchmark will run.
 * @dma_bits: DMA addressing capability.
 * @dma_dir: DMA data direction.
 * @dma_trans_ns: Time for DMA transmission in ns.
 * @granule: Number of PAGE_SIZE units to map/unmap at once.
	     In SG mode, this represents the number of scatterlist entries.
	     In single mode, this represents the total size of the mapping.
 * @map_mode: Mode of DMA mapping.
 * @expansion: Reserved for future use.
 */
struct map_benchmark {
	__u64 avg_map_100ns;
	__u64 map_stddev;
	__u64 avg_unmap_100ns;
	__u64 unmap_stddev;
	__u32 threads;
	__u32 seconds;
	__s32 node;
	__u32 dma_bits;
	__u32 dma_dir;
	__u32 dma_trans_ns;
	__u32 granule;
	__u8  map_mode;
	__u8 expansion[75];
};
#endif /* _KERNEL_DMA_BENCHMARK_H */
