// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for the scaled_ppm_to_ppb() integer overflow that allowed
 * a crafted clock_adjtime(ADJ_FREQUENCY) to bypass the PTP max_adj check.
 *
 * testptp's -f option stores the adjustment as an int ppb and cannot express
 * the 64-bit scaled-ppm values needed to overflow the conversion, so this
 * test crafts struct timex.freq directly.
 */
#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>
#include <linux/ptp_clock.h>
#include "../kselftest.h"

#define FD_TO_CLOCKID(fd)	((~(clockid_t)(fd) << 3) | 3)

/* clock_adjtime is not available in GLIBC < 2.14 */
#if !__GLIBC_PREREQ(2, 14)
#include <sys/syscall.h>
static int clock_adjtime(clockid_t id, struct timex *tx)
{
	return syscall(__NR_clock_adjtime, id, tx);
}
#endif

int main(int argc, char *argv[])
{
	const char *device = argc > 1 ? argv[1] : "/dev/ptp0";
	struct ptp_clock_caps caps;
	struct timex tx = { 0 };
	clockid_t clkid;
	int fd, ret;

	ksft_print_header();
	ksft_set_plan(1);

	if (sizeof(tx.freq) < 8)
		ksft_exit_skip("the overflow only affects 64-bit kernels\n");

	fd = open(device, O_RDWR);
	if (fd < 0)
		ksft_exit_skip("cannot open %s: %s\n", device, strerror(errno));

	clkid = FD_TO_CLOCKID(fd);

	if (ioctl(fd, PTP_CLOCK_GETCAPS, &caps))
		ksft_exit_skip("PTP_CLOCK_GETCAPS on %s: %s\n", device, strerror(errno));
	if (!caps.max_adj)
		ksft_exit_skip("%s does not support frequency adjustment\n", device);

	/*
	 * (1 + 147573952589676412) * 125 == 2^64 + 9, which overflows s64 in
	 * scaled_ppm_to_ppb() and wraps the result to a ppb of 0.  A kernel
	 * that does not detect the overflow lets this absurd frequency past
	 * the max_adj check; a fixed kernel rejects it with -ERANGE.
	 */
	tx.modes = ADJ_FREQUENCY;
	tx.freq = 147573952589676412LL;

	ret = clock_adjtime(clkid, &tx);
	ksft_test_result(ret < 0 && errno == ERANGE,
			 "overflowing frequency adjustment is rejected (ret=%d errno=%d)\n",
			 ret, ret < 0 ? errno : 0);

	close(fd);
	ksft_finished();
}
