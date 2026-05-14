// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>

#include "../kselftest_harness.h"

#define RT_SIG			(SIGRTMIN + 1)

#define EVENT_LIMIT		5
#define ITERATIONS		100

static struct {
	volatile sig_atomic_t in;
	volatile sig_atomic_t hup;
} count;

static void sigio_handler(int signo __maybe_unused, siginfo_t *info,
			  void *ucontext __maybe_unused)
{
	switch (info->si_code) {
	case POLL_IN:
		count.in++;
		break;
	case POLL_HUP:
		count.hup++;
		break;
	}
}

FIXTURE(refresh_signal)
{
	struct sigaction old_sa;
	int fd;
};

FIXTURE_SETUP(refresh_signal)
{
	struct sigaction sa = { 0 };
	struct perf_event_attr attr = { 0 };

	sa.sa_sigaction = sigio_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);

	/* Use a real-time signal so notifications are reliably queued */
	EXPECT_EQ(sigaction(RT_SIG, &sa, &self->old_sa), 0);

	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_TASK_CLOCK;
	attr.disabled = 1;
	attr.sample_period = 1000;	/* 1 us */
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;

	self->fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
	ASSERT_NE(self->fd, -1);

	/* Enable async notification */
	ASSERT_EQ(fcntl(self->fd, F_SETFL, fcntl(self->fd, F_GETFL) | O_ASYNC), 0);

	/* Receive the signal for current process */
	ASSERT_EQ(fcntl(self->fd, F_SETOWN, getpid()), 0);

	/* Use signo instead of the default SIGIO */
	ASSERT_EQ(fcntl(self->fd, F_SETSIG, RT_SIG), 0);
}

FIXTURE_TEARDOWN(refresh_signal)
{
	EXPECT_EQ(ioctl(self->fd, PERF_EVENT_IOC_DISABLE, 0), 0);

	close(self->fd);
	sigaction(RT_SIG, &self->old_sa, NULL);
}

TEST_F(refresh_signal, refresh_stress)
{
	int i;

	ASSERT_EQ(ioctl(self->fd, PERF_EVENT_IOC_RESET, 0), 0);

	for (i = 0; i < ITERATIONS; i++) {
		sig_atomic_t old_count = count.hup;

		/* Set event limit and the event is enabled */
		ASSERT_EQ(ioctl(self->fd, PERF_EVENT_IOC_REFRESH, EVENT_LIMIT), 0);

		/*
		 * Wait for new the POLL_HUP notification. The test is bounded
		 * by the default timeout.
		 */
		while (old_count == count.hup)
			;
	}

	/*
	 * Events before EVENT_LIMIT are reported as POLL_IN. When the limit
	 * is reached, the final event is reported as POLL_HUP. The total
	 * number of events is scaled by the number of iterations.
	 *
	 * Events are delivered via irq_work. If timers expire too close to
	 * each other, irq_work may not run before the next timer fires,
	 * causing some POLL_IN events to be missed. Therefore, use a
	 * less-or-equal comparison for POLL_IN.
	 *
	 * The last event stops the timer, so the POLL_HUP signal must be
	 * observed once per iteration when the limit is reached.
	 */
	EXPECT_LE(count.in, (EVENT_LIMIT - 1) * ITERATIONS);
	EXPECT_EQ(count.hup, ITERATIONS);
}

TEST_HARNESS_MAIN
