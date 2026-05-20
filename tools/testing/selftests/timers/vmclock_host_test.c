// SPDX-License-Identifier: GPL-2.0
/*
 * Test /dev/vmclock_host by comparing its time against CLOCK_TAI.
 *
 * Maps the vmclock page, reads time from it using the ABI formula,
 * and compares with clock_gettime(CLOCK_TAI) using ABA timestamps
 * to bound the uncertainty.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/vmclock-abi.h>

#ifdef __x86_64__
static inline uint64_t read_counter(void)
{
	unsigned int lo, hi;
	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t read_counter(void)
{
	uint64_t val;
	asm volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
}
#else
#error "Unsupported architecture"
#endif

/*
 * Compute time from vmclock: T = time_sec + time_frac_sec/2^64 +
 *   (counter_now - counter_value) * counter_period_frac_sec >> (64 + shift)
 *
 * Returns nanoseconds since epoch.
 */
static int64_t vmclock_read_ns(const volatile struct vmclock_abi *clk,
			       uint64_t counter_now)
{
	uint64_t delta = counter_now - clk->counter_value;
	uint64_t period = clk->counter_period_frac_sec;
	uint8_t shift = clk->counter_period_shift;
	__uint128_t ns128;

	/* delta * period gives seconds in 0.(64+shift) fixed point */
	ns128 = (__uint128_t)delta * period;
	ns128 >>= shift;
	/* Now ns128 is seconds in 0.64 fixed point. Add time_frac_sec */
	ns128 += clk->time_frac_sec;
	/* Top 64 bits are whole seconds of fractional part — but we
	 * need to add time_sec for the full result */
	uint64_t frac_sec = (uint64_t)(ns128 >> 64);
	uint64_t sub_sec_ns = (uint64_t)(((ns128 & 0xFFFFFFFFFFFFFFFFULL) *
					   1000000000ULL) >> 64);

	return (int64_t)(clk->time_sec + frac_sec) * 1000000000LL + sub_sec_ns;
}

static int64_t clock_tai_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_TAI, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void)
{
	int fd, ret = 0;
	volatile struct vmclock_abi *clk;
	int i, failures = 0;

	fd = open("/dev/vmclock_host", O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT) {
			printf("SKIP: /dev/vmclock_host not available\n");
			return 4;
		}
		perror("open /dev/vmclock_host");
		return 1;
	}

	clk = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	if (clk == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	if (clk->magic != VMCLOCK_MAGIC) {
		fprintf(stderr, "Bad magic: 0x%x\n", clk->magic);
		ret = 1;
		goto out;
	}

	if (clk->counter_id == VMCLOCK_COUNTER_INVALID) {
		printf("SKIP: counter_id is INVALID (clocksource not TSC?)\n");
		ret = 4;
		goto out;
	}

	printf("vmclock_host: version=%u counter_id=%u time_type=%u status=%u\n",
	       clk->version, clk->counter_id, clk->time_type, clk->clock_status);
	printf("  tai_offset=%d\n", (int16_t)clk->tai_offset_sec);
	printf("  counter_period_frac_sec=0x%" PRIx64 " shift=%u\n",
	       (uint64_t)clk->counter_period_frac_sec, clk->counter_period_shift);

	/* ABA comparison: read CLOCK_TAI, vmclock, CLOCK_TAI */
	printf("\nABA comparison (vmclock vs CLOCK_TAI):\n");
	for (i = 0; i < 10; i++) {
		uint32_t seq;
		int64_t tai_before, tai_after, vmclock_ns;
		int64_t delta, window;

		/* Read with seqcount retry */
		do {
			seq = clk->seq_count;
			if (seq & 1) {
				__asm__ volatile("pause" ::: "memory");
				continue;
			}
			__asm__ volatile("" ::: "memory");

			tai_before = clock_tai_ns();
			uint64_t ctr = read_counter();
			tai_after = clock_tai_ns();

			__asm__ volatile("" ::: "memory");
			if (clk->seq_count != seq)
				continue;

			vmclock_ns = vmclock_read_ns(clk, ctr);
			break;
		} while (1);

		window = tai_after - tai_before;
		/* vmclock should be between tai_before and tai_after */
		delta = vmclock_ns - tai_before;

		printf("  [%d] vmclock-tai_before=%+" PRId64 "ns window=%"
		       PRId64 "ns", i, delta, window);

		if (delta < -2000 || delta > window + 2000) {
			printf(" FAIL (out of range)\n");
			failures++;
		} else {
			printf(" OK\n");
		}

		usleep(100000); /* 100ms between samples */
	}

	if (failures) {
		printf("\nFAIL: %d/%d samples out of range\n", failures, 10);
		ret = 1;
	} else {
		printf("\nPASS: all samples within ABA window\n");
	}

out:
	munmap((void *)clk, 4096);
	close(fd);
	return ret;
}
