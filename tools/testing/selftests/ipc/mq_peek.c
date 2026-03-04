// SPDX-License-Identifier: GPL-2.0
/*
 * mq_peek.c - Selftest for mq_timedreceive2(MQ_PEEK)
 *
 * Tests the peek-without-dequeue extension to POSIX message queues:
 *   - Correct priority-rank indexed access (index 0 = highest priority)
 *   - FIFO ordering within same priority level
 *   - Non-destructive semantics (queue is never modified on peek)
 *   - All error paths (-EBADF, -EMSGSIZE, -EFAULT, -EAGAIN, -ENOENT)
 *   - Large (multi-segment) message payload copy
 *   - Concurrent peek + receive / peek + send races
 */

#include "linux/array_size.h"
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <time.h>

#include "kselftest.h"

#ifndef __NR_mq_timedreceive2
#if defined(__x86_64__)
#define __NR_mq_timedreceive2 548
#elif defined(__aarch64__)
#define __NR_mq_timedreceive2 548
#elif defined(__riscv)
#define __NR_mq_timedreceive2 548
#else
#define __NR_mq_timedreceive2 548
#endif
#endif

#define MQ_PEEK 2U

struct mq_timedreceive2_args {
	size_t msg_len;
	unsigned int *msg_prio;
	char *msg_ptr;
};

static long mq_timedreceive2(mqd_t mqdes, struct mq_timedreceive2_args *uargs,
			     unsigned int flags, unsigned long index,
			     const struct timespec *timeout)
{
	return syscall(__NR_mq_timedreceive2, (long)mqdes, uargs, (long)flags,
		       index, timeout);
}

#define MQ_NAME_PREFIX "/mq_peek_test_"
#define MAX_MSG_SIZE 128
#define LARGE_MSG_SIZE 4064
#define MQ_MAXMSG 16

#define PRIO_HIGH 9
#define PRIO_MED 5
#define PRIO_LOW 1

static mqd_t open_queue(const char *suffix, long msgsize)
{
	char name[64];
	struct mq_attr attr = {
		.mq_flags = 0,
		.mq_maxmsg = MQ_MAXMSG,
		.mq_msgsize = msgsize,
		.mq_curmsgs = 0,
	};
	mqd_t mqd;

	snprintf(name, sizeof(name), "%s%s", MQ_NAME_PREFIX, suffix);
	mq_unlink(name);

	mqd = mq_open(name, O_NONBLOCK | O_RDWR | O_CREAT | O_EXCL, 0600, &attr);
	if (mqd == (mqd_t)-1) {
		ksft_test_result_fail("mq_open(%s): %m\n", name);
		ksft_exit_fail();
	}

	mq_unlink(name);
	return mqd;
}

static void send_msg(mqd_t mqd, unsigned int prio, const char *text, size_t len)
{
	if (mq_send(mqd, text, len, prio) != 0) {
		ksft_test_result_fail("mq_send(prio=%u): %m\n", prio);
		ksft_exit_fail();
	}
}

static long peek(mqd_t mqd, unsigned long index, char *buf, size_t bufsz,
		 unsigned int *prio)
{
	struct mq_timedreceive2_args args = {
		.msg_len = bufsz,
		.msg_prio = prio,
		.msg_ptr = buf,
	};
	return mq_timedreceive2(mqd, &args, MQ_PEEK, index, NULL);
}

static long queue_depth(mqd_t mqd)
{
	struct mq_attr attr;

	if (mq_getattr(mqd, &attr) != 0)
		return -1;
	return attr.mq_curmsgs;
}

static void test_peek_empty_queue(void)
{
	mqd_t mqd = open_queue("empty", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;

	ret = peek(mqd, 0, buf, sizeof(buf), &prio);
	if (ret == -1 && errno == EAGAIN)
		ksft_test_result_pass("peek on empty queue [EAGAIN]\n");
	else
		ksft_test_result_fail("peek on empty queue: expected EAGAIN, got ret=%ld errno=%d (%m)\n",
							ret, errno);

	mq_close(mqd);
}

static void test_peek_invalid_fd(void)
{
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;

	ret = peek((mqd_t)-1, 0, buf, sizeof(buf), &prio);
	if (ret == -1 && errno == EBADF)
		ksft_test_result_pass("peek invalid fd [ EBADF]\n");
	else
		ksft_test_result_fail("peek invalid fd: expected EBADF, got ret=%ld errno=%d\n",
							   ret, errno);
}


static void test_peek_non_mqueue_fd(void)
{
	int pipefd[2];
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;

	if (pipe(pipefd) != 0) {
		ksft_test_result_skip("pipe() failed, skipping non-mqueue-fd test\n");
		return;
	}

	ret = peek((mqd_t)pipefd[0], 0, buf, sizeof(buf), &prio);
	if (ret == -1 && errno == EBADF)
		ksft_test_result_pass("peek on pipe fd [EBADF]\n");
	else
		ksft_test_result_fail("peek non-mqueue fd: expected EBADF, got ret=%ld errno=%d\n",
								ret, errno);

	close(pipefd[0]);
	close(pipefd[1]);
}


static void test_peek_writeonly_fd(void)
{
	char name[] = "/ksft_mq_peek_wo";
	struct mq_attr attr = { .mq_maxmsg = 4, .mq_msgsize = MAX_MSG_SIZE };
	mqd_t rw, wo;
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;

	mq_unlink(name);
	rw = mq_open(name, O_RDWR | O_CREAT, 0600, &attr);
	if (rw == (mqd_t)-1) {
		ksft_test_result_skip("mq_open RW failed: %m\n");
		return;
	}
	wo = mq_open(name, O_WRONLY);
	mq_unlink(name);

	if (wo == (mqd_t)-1) {
		ksft_test_result_skip("mq_open WO failed: %m\n");
		mq_close(rw);
		return;
	}

	send_msg(rw, PRIO_HIGH, "x", 1);

	ret = peek(wo, 0, buf, sizeof(buf), &prio);
	if (ret == -1 && errno == EBADF)
		ksft_test_result_pass("peek on O_WRONLY fd [EBADF]\n");
	else
		ksft_test_result_fail("peek WO fd: expected EBADF, got ret=%ld errno=%d\n",
								ret, errno);

	mq_close(wo);
	mq_close(rw);
}


static void test_peek_buffer_too_small(void)
{
	mqd_t mqd = open_queue("small", MAX_MSG_SIZE);
	char tiny[1]; /* deliberately too small */
	unsigned int prio;
	struct mq_timedreceive2_args args = {
		.msg_len = sizeof(tiny),
		.msg_prio = &prio,
		.msg_ptr = tiny,
	};
	long ret;

	send_msg(mqd, PRIO_HIGH, "hello", 5);

	ret = mq_timedreceive2(mqd, &args, MQ_PEEK, 0, NULL);
	if (ret == -1 && errno == EMSGSIZE)
		ksft_test_result_pass("peek with small buf [EMSGSIZE]\n");
	else
		ksft_test_result_fail("peek small buf: expected EMSGSIZE, got ret=%ld errno=%d\n",
							ret, errno);

	mq_close(mqd);
}




static void test_peek_bad_msg_ptr(void)
{
	mqd_t mqd = open_queue("bad_ptr", MAX_MSG_SIZE);
	unsigned int prio;

	struct mq_timedreceive2_args args = {
		.msg_len = MAX_MSG_SIZE,
		.msg_prio = &prio,
		.msg_ptr = (char *)0x1,
	};
	long ret;
	send_msg(mqd, PRIO_HIGH, "payload", 7);
	ret = mq_timedreceive2(mqd, &args, MQ_PEEK, 0, NULL);
	if (ret == -1 && errno == EFAULT)
		ksft_test_result_pass("peek bad msg_ptr [EFAULT]\n");
	else
		ksft_test_result_fail("peek bad msg_ptr: expected EFAULT, got ret=%ld errno=%d\n",
								ret, errno);

	mq_close(mqd);
}


static void test_peek_index_out_of_range(void)
{
	mqd_t mqd = open_queue("oob", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;
	send_msg(mqd, PRIO_MED, "one", 3);
	ret = peek(mqd, 1, buf, sizeof(buf), &prio);
	if (ret == -1 && errno == ENOENT)
		ksft_test_result_pass("peek OOB index [ENOENT]\n");
	else
		ksft_test_result_fail("peek OOB: expected ENOENT, got ret=%ld errno=%d\n",
								ret, errno);

	mq_close(mqd);
}


static void test_peek_basic_data(void)
{
	mqd_t mqd = open_queue("basic", MAX_MSG_SIZE);
	const char *payload = "peek-test-payload";
	char buf[MAX_MSG_SIZE];
	unsigned int prio = 0;
	long ret;

	send_msg(mqd, PRIO_HIGH, payload, strlen(payload));

	memset(buf, 0, sizeof(buf));
	ret = peek(mqd, 0, buf, sizeof(buf), &prio);

	if (ret < 0) {
		ksft_test_result_fail("basic peek failed: ret=%ld errno=%d (%m)\n", ret, errno);
		goto out;
	}
	if ((size_t)ret != strlen(payload)) {
		ksft_test_result_fail("basic peek: wrong size %ld (expected %zu)\n", ret, strlen(payload));
		goto out;
	}
	if (memcmp(buf, payload, strlen(payload)) != 0) {
		ksft_test_result_fail("basic peek: payload mismatch\n");
		goto out;
	}
	if (prio != PRIO_HIGH) {
		ksft_test_result_fail("basic peek: wrong prio %u (expected %d)\n", prio, PRIO_HIGH);
		goto out;
	}
	ksft_test_result_pass("basic peek: correct data and priority\n");
out:
	mq_close(mqd);
}


static void test_peek_nondestructive(void)
{
	mqd_t mqd = open_queue("nodestr", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	int i;

	send_msg(mqd, PRIO_HIGH, "A", 1);
	send_msg(mqd, PRIO_MED, "B", 1);
	send_msg(mqd, PRIO_LOW, "C", 1);

	if (queue_depth(mqd) != 3) {
		ksft_test_result_fail("initial depth != 3\n");
		mq_close(mqd);
		return;
	}

	for (i = 0; i < 10; i++) {
		peek(mqd, 0, buf, sizeof(buf), &prio);
		peek(mqd, 1, buf, sizeof(buf), &prio);
		peek(mqd, 2, buf, sizeof(buf), &prio);
	}

	if (queue_depth(mqd) == 3)
		ksft_test_result_pass(
			"peek is non-destructive (depth stays 3)\n");
	else
		ksft_test_result_fail("peek modified queue: depth=%ld (expected 3)\n", queue_depth(mqd));

	mq_close(mqd);
}


static void test_peek_priority_order(void)
{
	mqd_t mqd = open_queue("prio_order", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;
	int pass = 1;

	send_msg(mqd, PRIO_LOW, "low", 3);
	send_msg(mqd, PRIO_HIGH, "high", 4);
	send_msg(mqd, PRIO_MED, "med", 3);

	/* index 0 must return highest priority */
	ret = peek(mqd, 0, buf, sizeof(buf), &prio);
	if (ret < 0 || prio != PRIO_HIGH) {
		ksft_test_result_fail(
			"prio_order index0: prio=%u ret=%ld errno=%d\n", prio,
			ret, errno);
		pass = 0;
	}
	if (pass && memcmp(buf, "high", 4) != 0) {
		ksft_test_result_fail("prio_order index0: wrong payload\n");
		pass = 0;
	}

	/* index 1 must return medium priority */
	ret = peek(mqd, 1, buf, sizeof(buf), &prio);
	if (pass && (ret < 0 || prio != PRIO_MED)) {
		ksft_test_result_fail("prio_order index1: prio=%u ret=%ld\n",
				      prio, ret);
		pass = 0;
	}
	if (pass && memcmp(buf, "med", 3) != 0) {
		ksft_test_result_fail("prio_order index1: wrong payload\n");
		pass = 0;
	}

	/* index 2 must return lowest priority */
	ret = peek(mqd, 2, buf, sizeof(buf), &prio);
	if (pass && (ret < 0 || prio != PRIO_LOW)) {
		ksft_test_result_fail("prio_order index2: prio=%u ret=%ld\n",
				      prio, ret);
		pass = 0;
	}
	if (pass && memcmp(buf, "low", 3) != 0) {
		ksft_test_result_fail("prio_order index2: wrong payload\n");
		pass = 0;
	}

	if (pass)
		ksft_test_result_pass(
			"priority ordering: index0=HIGH, index1=MED, index2=LOW\n");

	mq_close(mqd);
}


static void test_peek_fifo_within_priority(void)
{
	mqd_t mqd = open_queue("fifo", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;
	int pass = 1;

	send_msg(mqd, PRIO_HIGH, "first", 5);
	send_msg(mqd, PRIO_HIGH, "second", 6);
	send_msg(mqd, PRIO_HIGH, "third", 5);

	memset(buf, 0, sizeof(buf));
	ret = peek(mqd, 0, buf, sizeof(buf), &prio);
	if (ret < 0 || memcmp(buf, "first", 5) != 0) {
		ksft_test_result_fail(
			"FIFO peek[0]: expected 'first', got '%.*s' ret=%ld\n",
			(int)ret, buf, ret);
		pass = 0;
	}

	if (pass) {
		char rbuf[MAX_MSG_SIZE];
		unsigned int rprio;
		ssize_t r = mq_receive(mqd, rbuf, sizeof(rbuf), &rprio);
		if (r < 0 || memcmp(rbuf, "first", 5) != 0) {
			ksft_test_result_fail("mq_receive 'first' failed\n");
			pass = 0;
		}
	}

	if (pass) {
		memset(buf, 0, sizeof(buf));
		ret = peek(mqd, 0, buf, sizeof(buf), &prio);
		if (ret < 0 || memcmp(buf, "second", 6) != 0) {
			ksft_test_result_fail("FIFO peek after receive: expected 'second', got '%.*s'\n",
				(int)ret, buf);
			pass = 0;
		}
}

	if (pass)
		ksft_test_result_pass("FIFO within same priority is correct\n");

	mq_close(mqd);
}


static void test_peek_all_indices(void)
{
	const unsigned int prios[] = { 2, 7, 4, 9, 1, 6 };
	const int N = (int)(sizeof(prios) / sizeof(prios[0]));
	mqd_t mqd = open_queue("all_idx", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	char expected_payload[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;
	int i, pass = 1;
	unsigned int sorted[6];

	for (i = 0; i < N; i++) {
		snprintf(expected_payload, sizeof(expected_payload),
			 "msg_prio_%u", prios[i]);
		send_msg(mqd, prios[i], expected_payload,
			 strlen(expected_payload));
		sorted[i] = prios[i];
	}

	for (i = 0; i < N - 1; i++) {
		int j;
		for (j = i + 1; j < N; j++) {
			if (sorted[j] > sorted[i]) {
				unsigned int tmp = sorted[i];
				sorted[i] = sorted[j];
				sorted[j] = tmp;
			}
		}
	}

	for (i = 0; i < N && pass; i++) {
		memset(buf, 0, sizeof(buf));
		ret = peek(mqd, (unsigned long)i, buf, sizeof(buf), &prio);
		if (ret < 0) {
			ksft_test_result_fail("all_indices peek[%d] failed: ret=%ld errno=%d\n",
									i, ret, errno);
			pass = 0;
			break;
		}
		if (prio != sorted[i]) {
			ksft_test_result_fail("all_indices peek[%d]: prio=%u expected=%u\n",
									i, prio, sorted[i]);
			pass = 0;
		}

		snprintf(expected_payload, sizeof(expected_payload),
			 "msg_prio_%u", sorted[i]);
		if (memcmp(buf, expected_payload, strlen(expected_payload))) {
			ksft_test_result_fail("all_indices peek[%d]: payload mismatch\n", i);
			pass = 0;
		}
	}

	if (pass && queue_depth(mqd) != N) {
		ksft_test_result_fail("all_indices: depth=%ld expected=%d after peek\n",
			queue_depth(mqd), N);
		pass = 0;
	}

	if (pass) {
		ret = peek(mqd, (unsigned long)N, buf, sizeof(buf), &prio);
		if (!(ret == -1 && errno == ENOENT)) {
			ksft_test_result_fail("all_indices OOB[%d]: expected ENOENT, got ret=%ld errno=%d\n",
									N, ret, errno);
			pass = 0;
		}
	}

	if (pass)
		ksft_test_result_pass("all-indices: correct prio order + OOB ENOENT\n");

	mq_close(mqd);
}

static void test_peek_large_message(void)
{
	mqd_t mqd = open_queue("large", LARGE_MSG_SIZE);
	char *send_buf, *recv_buf;
	unsigned int prio = 0;
	long ret;
	int pass = 1;

	send_buf = malloc(LARGE_MSG_SIZE);
	recv_buf = calloc(1, LARGE_MSG_SIZE);
	if (!send_buf || !recv_buf) {
		ksft_test_result_skip("OOM allocating large message buffers\n");
		goto out;
	}

	for (int i = 0; i < LARGE_MSG_SIZE; i++)
		send_buf[i] = (char)(i & 0xFF);

	send_msg(mqd, PRIO_HIGH, send_buf, LARGE_MSG_SIZE);

	ret = peek(mqd, 0, recv_buf, LARGE_MSG_SIZE, &prio);
	if (ret != LARGE_MSG_SIZE) {
		ksft_test_result_fail("large msg peek: ret=%ld expected=%d\n",
				      ret, LARGE_MSG_SIZE);
		pass = 0;
	}
	if (pass && memcmp(send_buf, recv_buf, LARGE_MSG_SIZE) != 0) {
		ksft_test_result_fail("large msg peek: payload mismatch\n");
		pass = 0;
	}
	if (pass && prio != PRIO_HIGH) {
		ksft_test_result_fail("large msg peek: prio=%u expected=%d\n",
				      prio, PRIO_HIGH);
		pass = 0;
	}
	if (pass && queue_depth(mqd) != 1) {
		ksft_test_result_fail(
			"large msg peek: queue modified (depth=%ld)\n",
			queue_depth(mqd));
		pass = 0;
	}
	if (pass)
		ksft_test_result_pass(
			"large (%d B) 	multi-segment peek: correct\n",
			LARGE_MSG_SIZE);
out:
	free(send_buf);
	free(recv_buf);
	mq_close(mqd);
}

static void test_no_peek_flag_is_receive(void)
{
	mqd_t mqd = open_queue("nopeek", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	unsigned int prio = 0;
	struct mq_timedreceive2_args args = {
		.msg_len = sizeof(buf),
		.msg_prio = &prio,
		.msg_ptr = buf,
	};
	long ret;

	send_msg(mqd, PRIO_HIGH, "consume-me", 10);

    ret = mq_timedreceive2(mqd, &args, 0, 0, NULL);
	if (ret < 0) {
		ksft_test_result_fail("no-peek receive failed: ret=%ld errno=%d\n", ret, errno);
		mq_close(mqd);
		return;
	}
	if (queue_depth(mqd) != 0)
		ksft_test_result_fail("no-peek: queue still has messages (depth=%ld)\n", queue_depth(mqd));
	else
		ksft_test_result_pass("without MQ_PEEK the message is consumed normally\n");

	mq_close(mqd);
}


struct race_ctx {
	mqd_t mqd;
	int errors;
};

static void *receiver_thread(void *arg)
{
	struct race_ctx *ctx = arg;
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	ssize_t r;

	while ((r = mq_receive(ctx->mqd, buf, sizeof(buf), &prio)) > 0)
		;

	return NULL;
}

static void test_peek_concurrent_receive(void)
{
	struct race_ctx ctx;
	pthread_t tid;
	char buf[MAX_MSG_SIZE];
	unsigned int prio;
	long ret;
	int i;

	ctx.mqd = open_queue("concurrent", MAX_MSG_SIZE);
	ctx.errors = 0;


	for (i = 0; i < MQ_MAXMSG; i++) {
		char payload[32];
		snprintf(payload, sizeof(payload), "msg%d", i);
		send_msg(ctx.mqd, (unsigned int)(i % 5) + 1, payload,
			 strlen(payload));
	}

	if (pthread_create(&tid, NULL, receiver_thread, &ctx) != 0) {
		ksft_test_result_skip("pthread_create failed\n");
		mq_close(ctx.mqd);
		return;
	}

	/*
     Peek repeatedly.The queue is being drained concurrently.
	 */
	for (i = 0; i < 200; i++) {
		ret = peek(ctx.mqd, (unsigned long)(i % 4), buf, sizeof(buf),
			   &prio);
		if (ret < 0 && errno != EAGAIN && errno != ENOENT) {
			ctx.errors++;
		}
	}

	pthread_join(tid, NULL);

	if (ctx.errors == 0)
		ksft_test_result_pass("concurrent peek+receive: no unexpected errors\n");
	else
		ksft_test_result_fail("concurrent peek+receive: %d unexpected errors\n", ctx.errors);

	mq_close(ctx.mqd);
}

static void test_peek_null_prio_ptr(void)
{
	mqd_t mqd = open_queue("null_prio", MAX_MSG_SIZE);
	char buf[MAX_MSG_SIZE];
	struct mq_timedreceive2_args args = {
		.msg_len = sizeof(buf),
		.msg_prio = NULL,
		.msg_ptr = buf,
	};
	long ret;

	send_msg(mqd, PRIO_MED, "no-prio-needed", 14);

	ret = mq_timedreceive2(mqd, &args, MQ_PEEK, 0, NULL);
	if (ret >= 0)
		ksft_test_result_pass("peek with NULL msg_prio ptr: OK\n");
	else
		ksft_test_result_fail("peek NULL msg_prio: ret=%ld errno=%d (%m)\n", ret, errno);

	mq_close(mqd);
}


static void test_peek_priority_matches_receive(void)
{
	mqd_t mqd = open_queue("prio_match", MAX_MSG_SIZE);
	char peek_buf[MAX_MSG_SIZE], recv_buf[MAX_MSG_SIZE];
	unsigned int peek_prio = 0, recv_prio = 0;
	long peek_ret;
	ssize_t recv_ret;
	int pass = 1;

	send_msg(mqd, PRIO_MED, "consistent-prio", 15);

	peek_ret = peek(mqd, 0, peek_buf, sizeof(peek_buf), &peek_prio);
	if (peek_ret < 0) {
		ksft_test_result_fail("peek failed: %m\n");
		mq_close(mqd);
		return;
	}

	recv_ret = mq_receive(mqd, recv_buf, sizeof(recv_buf), &recv_prio);
	if (recv_ret < 0) {
		ksft_test_result_fail("mq_receive failed: %m\n");
		mq_close(mqd);
		return;
	}

	if (peek_prio != recv_prio) {
		ksft_test_result_fail("prio mismatch: peek=%u receive=%u\n",
								peek_prio, recv_prio);
		pass = 0;
	}
	if (pass && peek_ret != recv_ret) {
		ksft_test_result_fail("size mismatch: peek=%ld receive=%zd\n",
				      peek_ret, recv_ret);
		pass = 0;
	}
	if (pass && memcmp(peek_buf, recv_buf, (size_t)recv_ret) != 0) {
		ksft_test_result_fail("payload mismatch between peek and receive\n");
		pass = 0;
	}
	if (pass)
		ksft_test_result_pass("peeked priority/payload matches mq_receive output\n");

	mq_close(mqd);
}



static const struct {
	const char *name;
	void (*fn)(void);
} tests[] = {
	{ "empty queue → EAGAIN", test_peek_empty_queue },
	{ "invalid fd → EBADF", test_peek_invalid_fd },
	{ "non-mqueue fd → EBADF", test_peek_non_mqueue_fd },
	{ "O_WRONLY fd → EBADF", test_peek_writeonly_fd },
	{ "buffer too small → EMSGSIZE", test_peek_buffer_too_small },
	{ "bad msg_ptr → EFAULT", test_peek_bad_msg_ptr },
	{ "OOB index → ENOENT", test_peek_index_out_of_range },
	{ "basic data+prio correctness", test_peek_basic_data },
	{ "non-destructive semantics", test_peek_nondestructive },
	{ "priority ordering across indices", test_peek_priority_order },
	{ "FIFO within same priority", test_peek_fifo_within_priority },
	{ "all distinct priority indices", test_peek_all_indices },
	{ "large multi-segment message", test_peek_large_message },
	{ "no MQ_PEEK → normal receive", test_no_peek_flag_is_receive },
	{ "concurrent peek + receive", test_peek_concurrent_receive },
	{ "NULL msg_prio ptr", test_peek_null_prio_ptr },
	{ "peeked prio matches mq_receive",
	  test_peek_priority_matches_receive },
};

int main(void)
{
	unsigned int i;
	long sc_ret;

	ksft_print_header();
	ksft_set_plan(sizeof(tests) / sizeof(tests[0]));


	{
		struct mq_timedreceive2_args probe_args = { 0 };
		sc_ret = mq_timedreceive2((mqd_t)-1, &probe_args, MQ_PEEK, 0,
					  NULL);
		if (sc_ret == -1 && errno == ENOSYS)
			ksft_exit_skip("mq_timedreceive2 syscall not available "
				"(NR=%d ENOSYS) — is the kernel too old?\n",
				__NR_mq_timedreceive2);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		ksft_print_msg("--- [%02u] %s ---\n", i + 1, tests[i].name);
		tests[i].fn();
	}

	return ksft_get_fail_cnt() ? 1 : 0;
}
