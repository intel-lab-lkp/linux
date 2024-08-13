// SPDX-License-Identifier: GPL-2.0
#include "workingset_report.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "../kselftest.h"

#define SYSFS_NODE_ONLINE "/sys/devices/system/node/online"
#define PROC_DROP_CACHES "/proc/sys/vm/drop_caches"

/* Returns read len on success, or -errno on failure. */
static ssize_t read_text(const char *path, char *buf, size_t max_len)
{
	ssize_t len;
	int fd, err;
	size_t bytes_read = 0;

	if (!max_len)
		return -EINVAL;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	while (bytes_read < max_len - 1) {
		len = read(fd, buf + bytes_read, max_len - 1 - bytes_read);

		if (len <= 0)
			break;
		bytes_read += len;
	}

	buf[bytes_read] = '\0';

	err = -errno;
	close(fd);
	return len < 0 ? err : bytes_read;
}

/* Returns written len on success, or -errno on failure. */
static ssize_t write_text(const char *path, const char *buf, ssize_t max_len)
{
	int fd, len, err;
	size_t bytes_written = 0;

	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0)
		return -errno;

	while (bytes_written < max_len) {
		len = write(fd, buf + bytes_written, max_len - bytes_written);

		if (len < 0)
			break;
		bytes_written += len;
	}

	err = -errno;
	close(fd);
	return len < 0 ? err : bytes_written;
}

static long read_num(const char *path)
{
	char buf[21];

	if (read_text(path, buf, sizeof(buf)) <= 0)
		return -1;
	return (long)strtoul(buf, NULL, 10);
}

static int write_num(const char *path, unsigned long n)
{
	char buf[21];

	sprintf(buf, "%lu", n);
	if (write_text(path, buf, strlen(buf)) < 0)
		return -1;
	return 0;
}

long sysfs_get_refresh_interval(int nid)
{
	char file[128];

	snprintf(file, sizeof(file),
		"/sys/devices/system/node/node%d/workingset_report/refresh_interval",
		nid);
	return read_num(file);
}

int sysfs_set_refresh_interval(int nid, long interval)
{
	char file[128];

	snprintf(file, sizeof(file),
		"/sys/devices/system/node/node%d/workingset_report/refresh_interval",
		nid);
	return write_num(file, interval);
}

int sysfs_get_page_age_intervals_str(int nid, char *buf, int len)
{
	char path[128];

	snprintf(path, sizeof(path),
		"/sys/devices/system/node/node%d/workingset_report/page_age_intervals",
		nid);
	return read_text(path, buf, len);

}

int sysfs_set_page_age_intervals_str(int nid, const char *buf, int len)
{
	char path[128];

	snprintf(path, sizeof(path),
		"/sys/devices/system/node/node%d/workingset_report/page_age_intervals",
		nid);
	return write_text(path, buf, len);
}

int sysfs_set_page_age_intervals(int nid, const char *const intervals[],
				 int nr_intervals)
{
	char file[128];
	char buf[1024];
	int i;
	int err, len = 0;

	for (i = 0; i < nr_intervals; ++i) {
		err = snprintf(buf + len, sizeof(buf) - len, "%s", intervals[i]);

		if (err < 0)
			return err;
		len += err;

		if (i < nr_intervals - 1) {
			err = snprintf(buf + len, sizeof(buf) - len, ",");
			if (err < 0)
				return err;
			len += err;
		}
	}

	snprintf(file, sizeof(file),
		"/sys/devices/system/node/node%d/workingset_report/page_age_intervals",
		nid);
	return write_text(file, buf, len);
}

int get_nr_nodes(void)
{
	char buf[22];
	char *found;

	if (read_text(SYSFS_NODE_ONLINE, buf, sizeof(buf)) <= 0)
		return -1;
	found = strstr(buf, "-");
	if (found)
		return (int)strtoul(found + 1, NULL, 10) + 1;
	return (long)strtoul(buf, NULL, 10) + 1;
}

int drop_pagecache(void)
{
	return write_num(PROC_DROP_CACHES, 1);
}

ssize_t sysfs_page_age_read(int nid, char *buf, size_t len)

{
	char file[128];

	snprintf(file, sizeof(file),
		 "/sys/devices/system/node/node%d/workingset_report/page_age",
		 nid);
	return read_text(file, buf, len);
}

/*
 * Finds the first occurrence of "N<nid>\n"
 * Modifies buf to terminate before the next occurrence of "N".
 * Returns a substring of buf starting after "N<nid>\n"
 */
char *page_age_split_node(char *buf, int nid, char **next)
{
	char node_str[5];
	char *found;
	int node_str_len;

	node_str_len = snprintf(node_str, sizeof(node_str), "N%u\n", nid);

	/* find the node prefix first */
	found = strstr(buf, node_str);
	if (!found) {
		ksft_print_msg("cannot find '%s' in page_idle_age", node_str);
		return NULL;
	}
	found += node_str_len;

	*next = strchr(found, 'N');
	if (*next)
		*(*next - 1) = '\0';

	return found;
}

ssize_t page_age_read(const char *buf, const char *interval, int pagetype)
{
	static const char * const type[ANON_AND_FILE] = { "anon=", "file=" };
	char *found;

	found = strstr(buf, interval);
	if (!found) {
		ksft_print_msg("cannot find %s in page_age", interval);
		return -1;
	}
	found = strstr(found, type[pagetype]);
	if (!found) {
		ksft_print_msg("cannot find %s in page_age", type[pagetype]);
		return -1;
	}
	found += strlen(type[pagetype]);
	return (long)strtoul(found, NULL, 10);
}

static const char *TEMP_FILE = "/tmp/workingset_selftest";
void cleanup_file_workingset(void)
{
	remove(TEMP_FILE);
}

int alloc_file_workingset(void *arg)
{
	int err = 0;
	char *ptr;
	int fd;
	int ppid;
	char *mapped;
	size_t size = (size_t)arg;
	size_t page_size = getpagesize();

	ppid = getppid();

	fd = open(TEMP_FILE, O_RDWR | O_CREAT);
	if (fd < 0) {
		err = -errno;
		ksft_perror("failed to open temp file\n");
		goto cleanup;
	}

	if (fallocate(fd, 0, 0, size) < 0) {
		err = -errno;
		ksft_perror("fallocate");
		goto cleanup;
	}

	mapped = (char *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			      fd, 0);
	if (mapped == NULL) {
		err = -errno;
		ksft_perror("mmap");
		goto cleanup;
	}

	while (getppid() == ppid) {
		sync();
		for (ptr = mapped; ptr < mapped + size; ptr += page_size)
			*ptr = *ptr ^ 0xFF;
	}

cleanup:
	cleanup_file_workingset();
	return err;
}

int alloc_anon_workingset(void *arg)
{
	char *buf, *ptr;
	int ppid = getppid();
	size_t size = (size_t)arg;
	size_t page_size = getpagesize();

	buf = malloc(size);

	if (!buf) {
		ksft_print_msg("cannot allocate anon workingset");
		exit(1);
	}

	while (getppid() == ppid) {
		for (ptr = buf; ptr < buf + size; ptr += page_size)
			*ptr = *ptr ^ 0xFF;
	}

	free(buf);
	return 0;
}
