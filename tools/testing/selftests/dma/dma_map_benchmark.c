// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 HiSilicon Limited.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/map_benchmark.h>

#define NSEC_PER_MSEC	1000000L

static char *directions[] = {
	"BIDIRECTIONAL",
	"TO_DEVICE",
	"FROM_DEVICE",
};

int main(int argc, char **argv)
{
	struct map_benchmark map;
	int fd, opt;
	/* default single thread, run 20 seconds on NUMA_NO_NODE */
	int threads = 1, seconds = 20, node = -1;
	/* default dma mask 32bit, bidirectional DMA */
	int bits = 32, xdelay = 0, dir = DMA_MAP_BIDIRECTIONAL;
	/* default granule 1 PAGESIZE */
	int granule = 1;
	int use_iova = 0;

	int cmd = DMA_MAP_BENCHMARK;

	while ((opt = getopt(argc, argv, "t:s:n:b:d:x:g:i:")) != -1) {
		switch (opt) {
		case 't':
			threads = atoi(optarg);
			break;
		case 's':
			seconds = atoi(optarg);
			break;
		case 'n':
			node = atoi(optarg);
			break;
		case 'b':
			bits = atoi(optarg);
			break;
		case 'd':
			dir = atoi(optarg);
			break;
		case 'i':
			use_iova = atoi(optarg);
			if (use_iova)
				cmd = DMA_MAP_BENCHMARK_IOVA;
			break;
		case 'x':
			xdelay = atoi(optarg);
			break;
		case 'g':
			granule = atoi(optarg);
			break;
		default:
			return -1;
		}
	}

	if (threads <= 0 || threads > DMA_MAP_MAX_THREADS) {
		fprintf(stderr, "invalid number of threads, must be in 1-%d\n",
			DMA_MAP_MAX_THREADS);
		exit(1);
	}

	if (seconds <= 0 || seconds > DMA_MAP_MAX_SECONDS) {
		fprintf(stderr, "invalid number of seconds, must be in 1-%d\n",
			DMA_MAP_MAX_SECONDS);
		exit(1);
	}

	if (xdelay < 0 || xdelay > DMA_MAP_MAX_TRANS_DELAY) {
		fprintf(stderr, "invalid transmit delay, must be in 0-%ld\n",
			DMA_MAP_MAX_TRANS_DELAY);
		exit(1);
	}

	/* suppose the mininum DMA zone is 1MB in the world */
	if (bits < 20 || bits > 64) {
		fprintf(stderr, "invalid dma mask bit, must be in 20-64\n");
		exit(1);
	}

	if (dir != DMA_MAP_BIDIRECTIONAL && dir != DMA_MAP_TO_DEVICE &&
			dir != DMA_MAP_FROM_DEVICE) {
		fprintf(stderr, "invalid dma direction\n");
		exit(1);
	}

	if (granule < 1 || granule > 1024) {
		fprintf(stderr, "invalid granule size\n");
		exit(1);
	}

	fd = open("/sys/kernel/debug/dma_map_benchmark", O_RDWR);
	if (fd == -1) {
		perror("open");
		exit(1);
	}

	memset(&map, 0, sizeof(map));
	map.seconds = seconds;
	map.threads = threads;
	map.node = node;
	map.dma_bits = bits;
	map.dma_dir = dir;
	map.dma_trans_ns = xdelay;
	map.granule = granule;
	map.use_iova = use_iova;

	if (ioctl(fd, cmd, &map)) {
		perror("ioctl");
		exit(1);
	}

	printf("=== DMA Mapping Benchmark Results ===\n");
	printf("Configuration: threads:%d seconds:%d node:%d dir:%s granule:%d iova:%d, has_iommu_dma:%d\n",
	       threads, seconds, node, directions[dir], granule, use_iova, map.has_iommu_dma);
	printf("Buffer size: %d pages (%d KB)\n", granule, granule * 4);
	printf("\n");

	if (use_iova == 0 || use_iova == 2) {
	    printf("STREAMING DMA RESULTS:\n");
	    printf("  Map   latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_map_100ns/10.0, map.map_stddev/10.0);
	    printf("  Unmap latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_unmap_100ns/10.0, map.unmap_stddev/10.0);

	    double streaming_total = map.avg_map_100ns/10.0 + map.avg_unmap_100ns/10.0;
	    printf("  Total latency: %7.1f μs\n", streaming_total);
	    printf("\n");
	}

	if (map.has_iommu_dma && (use_iova == 1 || use_iova == 2)) {
	    printf("IOVA DMA RESULTS:\n");
	    printf("  Alloc   latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_iova_alloc_100ns/10.0, map.iova_alloc_stddev/10.0);
	    printf("  Link    latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_iova_link_100ns/10.0, map.iova_link_stddev/10.0);
	    printf("  Sync    latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_iova_sync_100ns/10.0, map.iova_sync_stddev/10.0);
	    printf("  Destroy latency: %7.1f μs (σ=%5.1f μs)\n",
		   map.avg_iova_destroy_100ns/10.0, map.iova_destroy_stddev/10.0);

	    double iova_total = map.avg_iova_alloc_100ns/10.0 + map.avg_iova_link_100ns/10.0 +
				map.avg_iova_sync_100ns/10.0 + map.avg_iova_destroy_100ns/10.0;
	    printf("  Total latency: %7.1f μs\n", iova_total);
	    printf("\n");
	}

	/* Performance comparison for both modes */
	if (map.has_iommu_dma && use_iova == 2) {
	    double streaming_total = map.avg_map_100ns/10.0 + map.avg_unmap_100ns/10.0;
	    double iova_total = map.avg_iova_alloc_100ns/10.0 + map.avg_iova_link_100ns/10.0 +
				map.avg_iova_sync_100ns/10.0 + map.avg_iova_destroy_100ns/10.0;

	    printf("PERFORMANCE COMPARISON:\n");
	    printf("  Streaming DMA total: %7.1f μs\n", streaming_total);
	    printf("  IOVA DMA total:      %7.1f μs\n", iova_total);

	    if (streaming_total > 0) {
		double performance_ratio = iova_total / streaming_total;
		double performance_diff = ((iova_total - streaming_total) / streaming_total) * 100.0;

		printf("  Performance ratio:   %7.2fx", performance_ratio);
		if (performance_ratio < 1.0) {
		    printf(" (IOVA is %.1f%% faster)\n", -performance_diff);
		} else {
		    printf(" (IOVA is %.1f%% slower)\n", performance_diff);
		}

		// Throughput analysis (operations per second)
		double streaming_ops_per_sec = 1000000.0 / streaming_total;
		double iova_ops_per_sec = 1000000.0 / iova_total;

		printf("  Streaming throughput: %8.0f ops/sec\n", streaming_ops_per_sec);
		printf("  IOVA throughput:      %8.0f ops/sec\n", iova_ops_per_sec);

		/* Memory bandwidth estimate (if applicable) */
		double buffer_kb = granule * 4.0;
		double streaming_bw = (streaming_ops_per_sec * buffer_kb) / 1024.0; // MB/s
		double iova_bw = (iova_ops_per_sec * buffer_kb) / 1024.0; // MB/s

		printf("  Streaming bandwidth:  %8.1f MB/s\n", streaming_bw);
		printf("  IOVA bandwidth:       %8.1f MB/s\n", iova_bw);
	    }
	    printf("\n");
	}

	/* IOVA breakdown analysis (for IOVA modes) */
	if (map.has_iommu_dma && (use_iova == 1 || use_iova == 2)) {
	    double iova_total = map.avg_iova_alloc_100ns/10.0 + map.avg_iova_link_100ns/10.0 +
				map.avg_iova_sync_100ns/10.0 + map.avg_iova_destroy_100ns/10.0;

	    if (iova_total > 0) {
		printf("IOVA OPERATION BREAKDOWN:\n");
		printf("  Alloc:   %5.1f%% (%6.1f μs)\n",
		       (map.avg_iova_alloc_100ns/10.0 / iova_total) * 100.0, map.avg_iova_alloc_100ns/10.0);
		printf("  Link:    %5.1f%% (%6.1f μs)\n",
		       (map.avg_iova_link_100ns/10.0 / iova_total) * 100.0, map.avg_iova_link_100ns/10.0);
		printf("  Sync:    %5.1f%% (%6.1f μs)\n",
		       (map.avg_iova_sync_100ns/10.0 / iova_total) * 100.0, map.avg_iova_sync_100ns/10.0);
		printf("  Destroy: %5.1f%% (%6.1f μs)\n",
		       (map.avg_iova_destroy_100ns/10.0 / iova_total) * 100.0, map.avg_iova_destroy_100ns/10.0);
		printf("\n");
	    }
	}

	/* Recommendations based on results */
	if (map.has_iommu_dma && use_iova == 2) {
	    double streaming_total = map.avg_map_100ns/10.0 + map.avg_unmap_100ns/10.0;
	    double iova_total = map.avg_iova_alloc_100ns/10.0 + map.avg_iova_link_100ns/10.0 +
				map.avg_iova_sync_100ns/10.0 + map.avg_iova_destroy_100ns/10.0;

	    printf("RECOMMENDATIONS:\n");
	    if (iova_total < streaming_total * 0.9) {
		printf("  ✓ IOVA API shows significant performance benefits\n");
		printf("  ✓ Consider using IOVA API for this workload\n");
	    } else if (iova_total < streaming_total * 1.1) {
		printf("  ~ IOVA and Streaming APIs show similar performance\n");
	    } else {
		printf("  ⚠ Streaming API outperforms IOVA API for this benchmark\n");

		/* Identify bottlenecks */
		double max_iova_op = map.avg_iova_alloc_100ns/10.0;
		const char* bottleneck = "alloc";

		if (map.avg_iova_link_100ns/10.0 > max_iova_op) {
		    max_iova_op = map.avg_iova_link_100ns/10.0;
		    bottleneck = "link";
		}
		if (map.avg_iova_sync_100ns/10.0 > max_iova_op) {
		    max_iova_op = map.avg_iova_sync_100ns/10.0;
		    bottleneck = "sync";
		}
		if (map.avg_iova_destroy_100ns/10.0 > max_iova_op) {
		    max_iova_op = map.avg_iova_destroy_100ns/10.0;
		    bottleneck = "destroy";
		}

		printf("  ➤ Primary bottleneck appears to be IOVA %s operation\n", bottleneck);
	    }
	}

	printf("=== End of Benchmark ===\n");

	return 0;
}
