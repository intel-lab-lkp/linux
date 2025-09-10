// SPDX-License-Identifier: GPL-2.0+
/* devmem test tests.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tests.h"
#include "debug.h"
#include "utils.h"
#include "ram_map.h"
#include "secret.h"

#define KPROBE_EVENTS_PATH "%s/kprobe_events"
#define KPROBE_EVENTS_ENABLE "%s/events/kprobes/enable"
#define TRACE_PIPE_PATH "%s/trace_pipe"
#define MAX_LINE_LENGTH 256
#define RETPROBE_NAME "open_retprobe"

struct open_res {
	int open_resv;
	bool test_res;
};
char *tracing_dir;
char *tracingdirs[3] = {
	NULL,
	"/sys/kernel/tracing",
	"/sys/kernel/debug/tracing"
};

int check_and_set_tracefs_mount(void)
{
	FILE *mounts_file;
	char line[256];
	char device[64], mount_point[128], fs_type[32];
	int retval = 0;

	mounts_file = fopen("/proc/mounts", "r");
	if (mounts_file == NULL) {
		perror("Failed to open /proc/mounts");
		return 0; // Cannot verify, assume not mounted
	}

	while (fgets(line, sizeof(line), mounts_file)) {
		if (sscanf(line, "%s %s %s", device, mount_point, fs_type) >= 3) {
			if (strcmp(mount_point, "/sys/kernel/tracing") == 0 &&
			    strcmp(fs_type, "tracefs") == 0) {
				retval = 1;
				break;
			}
			if (strcmp(mount_point, "/sys/kernel/debug/tracing") == 0 &&
			    strcmp(fs_type, "tracefs") == 0) {
				retval = 2;
				break;
			}
		}
	}
	tracing_dir = tracingdirs[retval];
	return retval;
}

int get_device_numbers(int fd, unsigned int *major_num,
			unsigned int *minor_num)
{
	struct stat file_stat;

	if (fstat(fd, &file_stat) == -1) {
		perror("fstat failed");
		return -1;
	}

	if (S_ISCHR(file_stat.st_mode) || S_ISBLK(file_stat.st_mode)) {
		*major_num = major(file_stat.st_rdev);
		*minor_num = minor(file_stat.st_rdev);
		return 0;
	}
	fprintf(stderr, "File descriptor does not refer to a device file.\n");
	return -1;
}

static int write_file(const char *path, const char *data)
{
	int fd = open(path, O_WRONLY | O_TRUNC);
	ssize_t ret;

	if (fd < 0) {
		deb_printf("Error opening file %s: %s\n",
		    path, strerror(errno));
		return -1;
	}
	deb_printf("echo \"%s\" >%s\n", data, path);
	ret = write(fd, data, strlen(data));
	close(fd);
	if (ret < 0) {
		deb_printf("Error writing to file %s: %s\n",
		   path, strerror(errno));
		return -1;
	}
	return 0;
}

static void cleanup_probes(void)
{
	deb_printf("Cleaning up kprobes and tracing...\n");
	char buf[100];

	sprintf(buf, KPROBE_EVENTS_PATH, tracing_dir);
	if (write_file(buf, "\n") != 0)
		deb_printf("Failed to clear retprobes. Manual cleanup may be required.\n");

	sprintf(buf, KPROBE_EVENTS_ENABLE, tracing_dir);
	if (write_file(buf, "0") != 0)
		deb_printf("Failed to clear retprobes. Manual cleanup may be required.\n");

}

static void traced_open(const char *filename, const char *expected_func_name,
			struct open_res *r)
{
	pid_t child_pid, parent_pid, traced_pid, result;
	char retprobe_setup_cmd[MAX_LINE_LENGTH];
	char tmp_path[MAX_LINE_LENGTH];
	char line[MAX_LINE_LENGTH];
	int open_resv, retval = -1;
	struct open_res res;
	int status, timeout;
	FILE *trace_file;
	time_t start;
	int pfd[2];
	int sn;

	r->open_resv = -1;
	r->test_res = false;

	parent_pid = getpid();

	if (pipe(pfd) == -1) {
		perror("pipe failed");
		return;
	}

	deb_printf("Configuring kprobes on '%s'...\n", expected_func_name);
	snprintf(tmp_path, sizeof(tmp_path), KPROBE_EVENTS_PATH, tracing_dir);
	snprintf(retprobe_setup_cmd, sizeof(retprobe_setup_cmd),
		 "r2:kprobes/%s_ret %s retval=$retval ", RETPROBE_NAME,
		 expected_func_name);
	if (write_file(tmp_path, retprobe_setup_cmd) != 0) {
		cleanup_probes();
		return;
	}
	snprintf(tmp_path, sizeof(tmp_path), KPROBE_EVENTS_ENABLE,
		 tracing_dir);
	if (write_file(tmp_path, "1") != 0) {
		cleanup_probes();
		return;
	}

	child_pid = fork();
	if (child_pid == -1) {
		deb_printf("fork failed\n");
		cleanup_probes();
		return;
	}

	if (child_pid == 0) {
		close(pfd[0]);
		snprintf(line, sizeof(line), TRACE_PIPE_PATH, tracing_dir);
		trace_file = fopen(line, "r");
		if (!trace_file) {
			deb_printf("fopen trace_pipe failed in child\n");
			exit(EXIT_FAILURE);
		}

		open_resv = -1;

		sleep(2);
		while (fgets(line, sizeof(line), trace_file) != NULL) {
			traced_pid = -1;
			deb_printf("Received =>%s\n", line);
			deb_printf("matching against: RETPROBE_NAME=\"%s\" and expected_func_name=\"%s\"\n",
				   RETPROBE_NAME, expected_func_name);
			deb_printf("matching against: RETPROBE_NAME=\"%s\" => %p\n",
				   RETPROBE_NAME, strstr(line, RETPROBE_NAME));
			deb_printf("matching against: expected_func_name=\"%s\" =>%p\n",
			   expected_func_name, strstr(line, expected_func_name));

			if (strstr(line, RETPROBE_NAME) &&
			    strstr(line, expected_func_name)) {
				sn = sscanf(line, " %*[^-]-%d%*[^=]=%x", &traced_pid, &open_resv);
				deb_printf("scanned (%d)traced_pid=%d, open_resv=%d parent_pid=%d\n",
				    sn, traced_pid, open_resv, parent_pid);
				if (traced_pid == parent_pid && open_resv == 0) {
					deb_printf("found!\n");
					res.open_resv = open_resv;
					res.test_res = true;
					write(pfd[1], &res, sizeof(res));
					fclose(trace_file);
					exit(EXIT_SUCCESS);
				}
			}
		}
		fclose(trace_file);
		res.open_resv = -1;
		res.test_res = false;
		write(pfd[1], &res, sizeof(res));
		exit(EXIT_FAILURE);
	} else {
		close(pfd[1]);
		sleep(1);
		deb_printf("Parent process (PID %d) is calling open()...\n",
		    parent_pid);
		retval = open(filename, O_RDONLY);
		if (retval == -1) {
			deb_printf("open failed\n");
			kill(child_pid, SIGTERM);
			waitpid(child_pid, NULL, 0);
			cleanup_probes();
			return;
		}

		start = time(NULL);
		timeout = 15;

		while (1) {
			result = waitpid(-1, &status, WNOHANG);
			if (result == -1) {
				perror("waitpid");
				break;
			} else if (result > 0) {
				deb_printf("Child exited normally\n");
				break;
			}

			if (time(NULL) - start >= timeout) {
				printf("Timeout reached! Killing child...\n");
				kill(child_pid, SIGKILL);
				waitpid(child_pid, NULL, 0);
				break;
			}
			usleep(100000);
		}

		if (read(pfd[0], r, sizeof(struct open_res)) !=
		   sizeof(struct open_res)) {
			deb_printf("Failed to read data from child process.\n");
			r->test_res = false;
		}

		close(pfd[0]);

		cleanup_probes();

		r->open_resv = retval;
		if (r->open_resv >= 0 && r->test_res)
			r->test_res = true;
		else
			r->test_res = false;
	}
}

int test_read_at_addr_32bit_ge(struct test_context *t)
{
	if (is_64bit_arch()) {
		deb_printf("Skipped (64-bit architecture)\n");
		return SKIPPED;
	}

	uint64_t target_addr = 0x100000000ULL;
	int ret = try_read_dev_mem(t->fd, target_addr, 0, NULL);

	if (ret == 0) {
		deb_printf("PASS: Read beyond 4 GiB at 0x%llx returned 0 bytes\n",
		    target_addr);
		return PASS;
	}
	deb_printf("FAIL: Expected 0 bytes at 0x%llx, got %d (errno=%d)\n",
		    target_addr, ret, -ret);
	return FAIL;
}

int test_read_outside_linear_map(struct test_context *t)
{
	uint64_t tolerance, start_addr, max_addr, last_linear;

	if (sizeof(void *) == 8) {
		deb_printf("Skipped: 64-bit architecture\n");
		return SKIPPED;
	}

	if (!t->map || t->map->count == 0) {
		deb_printf("No memory map provided!\n");
		return SKIPPED;
	}

	start_addr = t->map->regions[0].start;
	max_addr = t->map->regions[t->map->count - 1].end;

	deb_printf("Scanning between 0x%llx and 0x%llx\n",
		   (unsigned long long)start_addr, (unsigned long long)max_addr);

	last_linear = find_last_linear_byte(t->fd, start_addr, max_addr);

	deb_printf("Last readable linear address: 0x%llx\n",
		   (unsigned long long)last_linear);

	tolerance = 16 * 1024 * 1024;
	if (last_linear + 1 >= EXPECTED_LINEAR_LIMIT - tolerance &&
		last_linear + 1 <= EXPECTED_LINEAR_LIMIT + tolerance) {
		deb_printf("PASS: Linear map ends near 1 GiB boundary.\n");
		return PASS;
	}
	deb_printf("FAIL: Linear map ends unexpectedly (expected ~890MB).\n");
	return FAIL;
}

int test_write_outside_linear_map(struct test_context *t)
{
	uint64_t tolerance, start_addr, max_addr, last_linear;

	if (sizeof(void *) == 8) {
		deb_printf("Skipped: 64-bit architecture\n");
		return SKIPPED;
	}

	if (!t->map || t->map->count == 0) {
		deb_printf("No memory map provided!\n");
		return SKIPPED;
	}

	start_addr = t->map->regions[0].start;
	max_addr = t->map->regions[t->map->count - 1].end;

	deb_printf("Scanning between 0x%llx and 0x%llx\n", (unsigned long long)start_addr,
		   (unsigned long long)max_addr);

	last_linear = find_last_linear_byte(t->fd, start_addr, max_addr);

	deb_printf("Last readable linear address: 0x%llx\n",
		   (unsigned long long)last_linear);

	tolerance = 16 * 1024 * 1024;
	if (last_linear + 1 >= EXPECTED_LINEAR_LIMIT - tolerance &&
	    last_linear + 1 <= EXPECTED_LINEAR_LIMIT + tolerance) {
		deb_printf("PASS: Linear map ends near 1 GiB boundary.\n");
		fill_random_chars(t->srcbuf, BOUNCE_BUF_SIZE);
		if (try_write_dev_mem(t->fd, last_linear + 0x1000,
		    BOUNCE_BUF_SIZE, t->srcbuf) < 0) {
			return FAIL;
		}
		return PASS;
	}
	deb_printf("FAIL: Linear map ends unexpectedly (expected ~890MB).\n");
	return FAIL;
}

int test_strict_devmem(struct test_context *t)
{
	int res = FAIL;
	uint64_t addr;
	ssize_t ret;
	uint8_t buf;

	addr = find_high_system_ram_addr(t->map);
	if (addr == 0) {
		deb_printf("No high System RAM region found.\n");
		res = SKIPPED;
		return res;
	}

	deb_printf("Testing physical address: 0x%llx\n", addr);

	ret = pread(t->fd, &buf, 1, addr);
	if (ret < 0) {
		if (errno == EPERM) {
			deb_printf("CONFIG_STRICT_DEVMEM is ENABLED\n");
		} else if (errno == EFAULT || errno == ENXIO) {
			deb_printf("Invalid address (errno=%d). Try another region.\n", errno);
			res = SKIPPED;
		} else if (errno == EACCES) {
			deb_printf("Access blocked by LSM or lockdown (errno=EACCES).\n");
			res = SKIPPED;
		} else {
			perror("pread");
		}
	} else {
		deb_printf("CONFIG_STRICT_DEVMEM is DISABLED\n");
		res = PASS;
	}

	if (res != PASS)
		t->strict_devmem_state = true;

	return res;
}

int test_devmem_access(struct test_context *t)
{
	struct open_res res;

	if (!check_and_set_tracefs_mount()) {
		deb_printf("Tracing directory not found. This test requires debugfs mounted.\n");
		return FAIL;
	}

	traced_open("/dev/mem", "memory_open", &res);
	if ((res.test_res) && (res.open_resv >= 0)) {
		deb_printf("test_res=%d, open_resv=%d\n",
		    res.test_res, res.open_resv);
		t->fd = res.open_resv;
		t->devmem_init_state = true;
		return PASS;
	}
	return FAIL;
}

int test_read_secret_area(struct test_context *t)
{
	void *tmp_ptr;

	deb_printf("\ntest_read_secret_area - start\n");
	tmp_ptr = secret_alloc(BOUNCE_BUF_SIZE);

	if (tmp_ptr) {
		deb_printf("secret_alloc [ok] tmp_ptr va addr = 0x%lx\n",
		    tmp_ptr);
		fill_random_chars(tmp_ptr, BOUNCE_BUF_SIZE); // lazy alloc
		if (t->verbose)
			print_hex(tmp_ptr, 32);
		t->tst_addr = virt_to_phys(tmp_ptr);
		if (t->tst_addr) {
			deb_printf("filled with things -> tst_addr phy addr = 0x%lx\n",
				   t->tst_addr);
			if (try_read_dev_mem(t->fd, t->tst_addr,
			    BOUNCE_BUF_SIZE, t->dstbuf) < 0)
				return PASS;
		}
	}
	return FAIL;
}

int test_read_restricted_area(struct test_context *t)
{
	fill_random_chars(t->dstbuf, BOUNCE_BUF_SIZE);
	if (t->verbose)
		print_hex(t->dstbuf, 32);
	t->tst_addr = pick_restricted_address(t->map);
	if (t->tst_addr) {
		if (try_read_dev_mem(t->fd, t->tst_addr, BOUNCE_BUF_SIZE,
		    t->dstbuf) >= 0) {
			if (t->verbose)
				print_hex(t->dstbuf, 32);

			if (is_zero(t->dstbuf, BOUNCE_BUF_SIZE))
				return PASS;

		}
	}
	return FAIL;
}

int test_read_allowed_area(struct test_context *t)
{
	fill_random_chars(t->srcbuf, BOUNCE_BUF_SIZE);
	t->tst_addr = virt_to_phys(t->srcbuf);
	if (t->tst_addr) {
		if (try_read_dev_mem(t->fd, t->tst_addr, BOUNCE_BUF_SIZE,
		    t->dstbuf) >= 0) {
			deb_printf("Read OK  compare twos\n", t->tst_addr);
			if (t->verbose) {
				print_hex(t->srcbuf, BOUNCE_BUF_SIZE);
				print_hex(t->dstbuf, BOUNCE_BUF_SIZE);
			}
			if (!memcmp(t->srcbuf, t->dstbuf, BOUNCE_BUF_SIZE))
				return PASS;
		}
	}
	return FAIL;
}

int test_read_allowed_area_ppos_advance(struct test_context *t)
{
	fill_random_chars(t->srcbuf, BOUNCE_BUF_SIZE);
	memset(t->dstbuf, 0, BOUNCE_BUF_SIZE);
	if (t->verbose)
		print_hex(t->srcbuf, 32);
	t->tst_addr = virt_to_phys(t->srcbuf);
	if (t->tst_addr) {
		if ((try_read_dev_mem(t->fd, t->tst_addr,
		    BOUNCE_BUF_SIZE / 2, t->dstbuf) >= 0) &&
			(try_read_inplace(t->fd, BOUNCE_BUF_SIZE / 2,
			    t->dstbuf) >= 0)){
			if (t->verbose)
				print_hex(t->dstbuf, 32);

			if (!memcmp(t->srcbuf + BOUNCE_BUF_SIZE / 2,
			    t->dstbuf, BOUNCE_BUF_SIZE / 2)) {
				return PASS;
			}
		}
	}
	return FAIL;
}

int test_write_outside_area(struct test_context *t)
{
	fill_random_chars(t->srcbuf, BOUNCE_BUF_SIZE);
	t->tst_addr = pick_outside_address(t->map);
	if (try_write_dev_mem(t->fd, t->tst_addr, BOUNCE_BUF_SIZE,
	    t->srcbuf) < 0)
		return PASS;

	return FAIL;
}

/*
 * this test needs to follow test_seek_seek_set
 */
int test_seek_seek_cur(struct test_context *t)
{
	t->tst_addr = pick_valid_ram_address(t->map);
	if (lseek(t->fd, 0, SEEK_SET) == (off_t)-1)
		return FAIL;

	if (lseek(t->fd, t->tst_addr, SEEK_CUR) == (off_t)-1)
		return FAIL;

	return PASS;
}

int test_seek_seek_set(struct test_context *t)
{
	t->tst_addr = pick_valid_ram_address(t->map);
	if (lseek(t->fd, t->tst_addr, SEEK_SET) == (off_t)-1)
		return FAIL;

	return PASS;
}

int test_seek_seek_other(struct test_context *t)
{
	if (lseek(t->fd, 0, SEEK_END) == (off_t)-1)
		return PASS;

	return FAIL;
}

int test_open_devnum(struct test_context *t)
{
	unsigned int major_num, minor_num;

	if (get_device_numbers(t->fd, &major_num, &minor_num) == 0) {
		if ((major_num == 1) && (minor_num == 1))
			return PASS;
	}
	return FAIL;
}
