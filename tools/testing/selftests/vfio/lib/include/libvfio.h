/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_H
#define SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_H

#include <libvfio/assert.h>
#include <libvfio/iommu.h>
#include <libvfio/iova_allocator.h>
#include <libvfio/sysfs.h>
#include <libvfio/vfio_pci_device.h>
#include <libvfio/vfio_pci_driver.h>

#include <stdint.h>
#include <time.h>
#include <linux/time64.h>

static inline void timer_start(struct timespec *start)
{
	clock_gettime(CLOCK_MONOTONIC, start);
}

static inline uint64_t timer_elapsed_ns(struct timespec start)
{
	struct timespec end;

	clock_gettime(CLOCK_MONOTONIC, &end);

	return (uint64_t)(end.tv_sec - start.tv_sec) * NSEC_PER_SEC +
	       (uint64_t)(end.tv_nsec - start.tv_nsec);
}

#define TIME(_name, _expression) do {				   \
	struct timespec __start;				   \
								   \
	timer_start(&__start);					   \
	_expression;						   \
	printf(_name " = %.2lfms\n",				   \
	       (double)timer_elapsed_ns(__start) / NSEC_PER_MSEC); \
} while (0)

/*
 * Return the BDF string of the device that the test should use.
 *
 * If a BDF string is provided by the user on the command line (as the last
 * element of argv[]), then this function will return that and decrement argc
 * by 1.
 *
 * Otherwise this function will attempt to use the environment variable
 * $VFIO_SELFTESTS_BDF.
 *
 * If BDF cannot be determined then the test will exit with KSFT_SKIP.
 */
const char *vfio_selftests_get_bdf(int *argc, char *argv[]);
char **vfio_selftests_get_bdfs(int *argc, char *argv[], int *nr_bdfs);

/*
 * Reserve virtual address space of size at an address satisfying
 * (vaddr % align) == offset.
 *
 * Returns the reserved vaddr. The caller is responsible for unmapping
 * the returned region.
 */
void *mmap_reserve(size_t size, size_t align, size_t offset);

#endif /* SELFTESTS_VFIO_LIB_INCLUDE_LIBVFIO_H */
