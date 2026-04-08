// SPDX-License-Identifier: GPL-2.0
/*
 * Regression test for CUSE device-node leak (missing device_del on
 * error path).
 *
 * When cdev_alloc() fails after device_add() succeeds inside
 * cuse_process_init_reply(), the error path must call device_del()
 * before put_device().  Otherwise the /dev/<name> entry leaks and
 * permanently poisons the device name.
 *
 * Test 1 (normal_cleanup):
 *   Completes a CUSE_INIT handshake, verifies the device appears,
 *   closes the channel fd, verifies the device is removed.
 *
 * Test 2 (leak_regression):
 *   Forces cdev_alloc() to fail via the cuse_inject_cdev_failure
 *   module parameter (preferred, requires CONFIG_FAULT_INJECTION in
 *   the CUSE build) or via failslab with stack-trace filtering
 *   (fallback).  Verifies the /dev node is NOT leaked after close.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kselftest.h"

#define CUSE_DEV		"/dev/cuse"
#define BUF_SIZE		(FUSE_MIN_READ_BUFFER + 4096)

#define INJECT_PARAM		"/sys/module/cuse/parameters/cuse_inject_cdev_failure"

#define FAILSLAB		"/sys/kernel/debug/failslab"
#define FAILSLAB_PROBABILITY	FAILSLAB "/probability"
#define FAILSLAB_TIMES		FAILSLAB "/times"
#define FAILSLAB_TASK_FILTER	FAILSLAB "/task-filter"
#define FAILSLAB_STACK_DEPTH	FAILSLAB "/stacktrace-depth"
#define FAILSLAB_REQUIRE_START	FAILSLAB "/require-start"
#define FAILSLAB_REQUIRE_END	FAILSLAB "/require-end"
#define MAKE_IT_FAIL		"/proc/self/make-it-fail"

static int write_file(const char *path, const char *val)
{
	int fd, len;
	ssize_t n;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = strlen(val);
	n = write(fd, val, len);
	close(fd);
	return n == len ? 0 : -1;
}

static int write_file_ul(const char *path, unsigned long val)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%lu", val);
	return write_file(path, buf);
}

static int read_file_int(const char *path)
{
	char buf[32];
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static unsigned long kallsym_lookup(const char *name)
{
	unsigned long addr = 0;
	char line[256];
	FILE *f;

	f = fopen("/proc/kallsyms", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char sym[128];
		unsigned long a;
		char type;

		if (sscanf(line, "%lx %c %127s", &a, &type, sym) == 3 &&
		    strcmp(sym, name) == 0) {
			addr = a;
			break;
		}
	}
	fclose(f);
	return addr;
}

static void failslab_cleanup(void)
{
	write_file(MAKE_IT_FAIL, "0");
	write_file(FAILSLAB_PROBABILITY, "0");
	write_file(FAILSLAB_TIMES, "0");
	write_file(FAILSLAB_TASK_FILTER, "0");
	write_file(FAILSLAB_REQUIRE_START, "0");
	write_file(FAILSLAB_REQUIRE_END, "0");
}

static int cuse_read_init(int fd, struct fuse_in_header *hdr_out)
{
	char buf[BUF_SIZE];
	struct fuse_in_header *hdr;
	ssize_t n;

	n = read(fd, buf, sizeof(buf));
	if (n < (ssize_t)(sizeof(*hdr) + sizeof(struct cuse_init_in)))
		return -1;
	hdr = (struct fuse_in_header *)buf;
	if (hdr->opcode != CUSE_INIT)
		return -1;
	memcpy(hdr_out, hdr, sizeof(*hdr));
	return 0;
}

static int cuse_send_init_reply(int fd, const struct fuse_in_header *hdr,
				const char *devname)
{
	char reply[sizeof(struct fuse_out_header) +
		   sizeof(struct cuse_init_out) + 64];
	struct fuse_out_header *out_hdr;
	struct cuse_init_out *init_out;
	size_t info_len, reply_len;
	char *info;
	ssize_t n;

	memset(reply, 0, sizeof(reply));
	out_hdr = (struct fuse_out_header *)reply;
	init_out = (struct cuse_init_out *)(reply + sizeof(*out_hdr));
	info = reply + sizeof(*out_hdr) + sizeof(*init_out);

	info_len = snprintf(info, 64, "DEVNAME=%s", devname) + 1;
	reply_len = sizeof(*out_hdr) + sizeof(*init_out) + info_len;

	out_hdr->len = reply_len;
	out_hdr->unique = hdr->unique;
	init_out->major = FUSE_KERNEL_VERSION;
	init_out->minor = FUSE_KERNEL_MINOR_VERSION;
	init_out->flags = CUSE_UNRESTRICTED_IOCTL;
	init_out->max_read = BUF_SIZE;
	init_out->max_write = 4096;

	n = write(fd, reply, reply_len);
	return n == (ssize_t)reply_len ? 0 : -1;
}

static int dev_exists(const char *devname)
{
	char path[128];

	snprintf(path, sizeof(path), "/dev/%s", devname);
	return access(path, F_OK) == 0;
}

/*
 * Injection methods for forcing cdev_alloc() failure.
 *
 * METHOD_PARAM (preferred): cuse_inject_cdev_failure module parameter.
 *   Available when CUSE is built with CONFIG_FAULT_INJECTION.  Directly
 *   forces cdev_alloc() to "fail" inside cuse_process_init_reply().
 *   Works on all architectures including UML.
 *
 * METHOD_FAILSLAB (fallback): failslab with stack-trace filtering.
 *   Uses the kernel's generic slab fault injection to fail the kzalloc
 *   inside cdev_alloc().  Requires CONFIG_FAILSLAB and
 *   CONFIG_FAULT_INJECTION_STACKTRACE_FILTER.  Depends on the stack
 *   unwinder producing matching addresses (may not work under UML).
 */
enum inject_method {
	METHOD_NONE,
	METHOD_PARAM,
	METHOD_FAILSLAB,
};

static enum inject_method inject_available(void)
{
	if (access(INJECT_PARAM, W_OK) == 0)
		return METHOD_PARAM;

	if (access(FAILSLAB_REQUIRE_START, F_OK) == 0 &&
	    kallsym_lookup("cdev_alloc") != 0)
		return METHOD_FAILSLAB;

	return METHOD_NONE;
}

static int inject_arm(enum inject_method m)
{
	unsigned long addr;

	switch (m) {
	case METHOD_PARAM:
		return write_file(INJECT_PARAM, "Y");

	case METHOD_FAILSLAB:
		addr = kallsym_lookup("cdev_alloc");
		if (!addr)
			return -1;
		write_file(FAILSLAB_TASK_FILTER, "1");
		write_file(FAILSLAB_STACK_DEPTH, "16");
		write_file_ul(FAILSLAB_REQUIRE_START, addr);
		write_file_ul(FAILSLAB_REQUIRE_END, addr + 0x200);
		write_file(FAILSLAB_TIMES, "1");
		write_file(FAILSLAB_PROBABILITY, "100");
		write_file(MAKE_IT_FAIL, "1");
		return 0;

	default:
		return -1;
	}
}

static void inject_disarm(enum inject_method m)
{
	switch (m) {
	case METHOD_PARAM:
		write_file(INJECT_PARAM, "N");
		break;
	case METHOD_FAILSLAB:
		write_file(MAKE_IT_FAIL, "0");
		failslab_cleanup();
		break;
	default:
		break;
	}
}

static int inject_fired(enum inject_method m)
{
	switch (m) {
	case METHOD_PARAM:
		return 1;
	case METHOD_FAILSLAB:
		return read_file_int(FAILSLAB_TIMES) <= 0;
	default:
		return 0;
	}
}

static const char *inject_name(enum inject_method m)
{
	switch (m) {
	case METHOD_PARAM:	return "module parameter";
	case METHOD_FAILSLAB:	return "failslab";
	default:		return "none";
	}
}

/*
 * Test 1: normal CUSE init and cleanup.
 *
 * Verify a successfully initialized CUSE device is properly removed
 * when the channel fd is closed (basic sanity).
 */
static void test_normal_cleanup(void)
{
	const char *name = "cuse_ksft_norm";
	struct fuse_in_header hdr;
	int fd;

	fd = open(CUSE_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ksft_test_result_skip("open %s: %s\n", CUSE_DEV,
				      strerror(errno));
		return;
	}

	if (cuse_read_init(fd, &hdr)) {
		close(fd);
		ksft_test_result_fail("CUSE_INIT read failed\n");
		return;
	}

	if (cuse_send_init_reply(fd, &hdr, name)) {
		close(fd);
		ksft_test_result_fail("CUSE_INIT reply write failed\n");
		return;
	}

	usleep(100000);
	if (!dev_exists(name)) {
		close(fd);
		ksft_test_result_fail("/dev/%s not created after init\n", name);
		return;
	}

	close(fd);
	usleep(100000);

	if (dev_exists(name)) {
		ksft_test_result_fail("/dev/%s not removed after close\n",
				      name);
		return;
	}

	ksft_test_result_pass("normal init and cleanup\n");
}

/*
 * Test 2: regression test for device-node leak.
 *
 * Force cdev_alloc() to fail after device_add() has succeeded, then
 * verify the /dev node is properly cleaned up by device_del().
 */
static void test_leak_on_cdev_alloc_failure(void)
{
	const char *name = "cuse_ksft_leak";
	struct fuse_in_header hdr;
	enum inject_method m;
	int fd, fired;

	if (dev_exists(name)) {
		ksft_test_result_skip("/dev/%s already exists (previous leak? reboot to clear)\n",
				      name);
		return;
	}

	m = inject_available();
	if (m == METHOD_NONE) {
		ksft_test_result_skip(
			"no injection method available (need CONFIG_FAULT_INJECTION)\n");
		return;
	}

	fd = open(CUSE_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ksft_test_result_skip("open %s: %s\n", CUSE_DEV,
				      strerror(errno));
		return;
	}

	if (cuse_read_init(fd, &hdr)) {
		close(fd);
		ksft_test_result_fail("CUSE_INIT read failed\n");
		return;
	}

	if (inject_arm(m)) {
		close(fd);
		ksft_test_result_fail("failed to arm injection (%s)\n",
				      inject_name(m));
		return;
	}

	cuse_send_init_reply(fd, &hdr, name);

	fired = inject_fired(m);
	inject_disarm(m);

	usleep(100000);
	close(fd);
	usleep(100000);

	if (!fired) {
		ksft_test_result_skip("injection did not trigger (%s)\n",
				      inject_name(m));
		return;
	}

	if (dev_exists(name)) {
		ksft_test_result_fail(
			"/dev/%s leaked: device_del() not called on cdev_alloc error path\n",
			name);
		return;
	}

	ksft_test_result_pass("device node cleaned up after cdev_alloc failure (via %s)\n",
			      inject_name(m));
}

int main(int argc, char **argv)
{
	ksft_print_header();
	ksft_set_plan(2);

	if (geteuid() != 0)
		ksft_exit_skip("must be run as root\n");

	if (access(CUSE_DEV, F_OK) != 0)
		ksft_exit_skip(CUSE_DEV " not available (try: modprobe cuse)\n");

	test_normal_cleanup();
	test_leak_on_cdev_alloc_failure();

	ksft_finished();
}
