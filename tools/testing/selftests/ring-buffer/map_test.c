// SPDX-License-Identifier: GPL-2.0
/*
 * Ring-buffer memory mapping tests
 *
 * Copyright (c) 2024 Vincent Donnefort <vdonnefort@google.com>
 */
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <linux/trace_mmap.h>

#include <sys/mman.h>
#include <sys/ioctl.h>

#include "../user_events/user_events_selftests.h" /* share tracefs setup */
#include "../kselftest_harness.h"

#define TRACEFS_ROOT "/sys/kernel/tracing"

static int __tracefs_write(const char *path, const char *value)
{
	FILE *file;

	file = fopen(path, "w");
	if (!file)
		return -1;

	fputs(value, file);
	fclose(file);

	return 0;
}

static int __tracefs_write_int(const char *path, int value)
{
	char *str;
	int ret;

	if (asprintf(&str, "%d", value) < 0)
		return -1;

	ret = __tracefs_write(path, str);

	free(str);

	return ret;
}

#define tracefs_write_int(path, value) \
	ASSERT_EQ(__tracefs_write_int((path), (value)), 0)

static int tracefs_reset(void)
{
	if (__tracefs_write_int(TRACEFS_ROOT"/tracing_on", 0))
		return -1;
	if (__tracefs_write_int(TRACEFS_ROOT"/trace", 0))
		return -1;
	if (__tracefs_write(TRACEFS_ROOT"/set_event", ""))
		return -1;
	if (__tracefs_write(TRACEFS_ROOT"/current_tracer", "nop"))
		return -1;

	return 0;
}

FIXTURE(map) {
	struct trace_buffer_meta	*meta;
	void				*data;
	int				cpu_fd;
	bool				umount;
};

FIXTURE_VARIANT(map) {
	int	subbuf_size;
};

FIXTURE_VARIANT_ADD(map, subbuf_size_4k) {
	.subbuf_size = 4,
};

FIXTURE_VARIANT_ADD(map, subbuf_size_8k) {
	.subbuf_size = 8,
};

FIXTURE_SETUP(map)
{
	int cpu = sched_getcpu(), page_size = getpagesize();
	unsigned long meta_len, data_len;
	char *cpu_path, *message;
	bool fail, umount;
	cpu_set_t cpu_mask;
	void *map;

	if (!tracefs_enabled(&message, &fail, &umount)) {
		if (fail) {
			TH_LOG("Tracefs setup failed: %s", message);
			ASSERT_FALSE(fail);
		}
		SKIP(return, "Skipping: %s", message);
	}

	self->umount = umount;

	ASSERT_GE(cpu, 0);

	ASSERT_EQ(tracefs_reset(), 0);

	tracefs_write_int(TRACEFS_ROOT"/buffer_subbuf_size_kb", variant->subbuf_size);

	ASSERT_GE(asprintf(&cpu_path,
			   TRACEFS_ROOT"/per_cpu/cpu%d/trace_pipe_raw",
			   cpu), 0);

	self->cpu_fd = open(cpu_path, O_RDONLY | O_NONBLOCK);
	ASSERT_GE(self->cpu_fd, 0);
	free(cpu_path);

	map = mmap(NULL, page_size, PROT_READ, MAP_SHARED, self->cpu_fd, 0);
	ASSERT_NE(map, MAP_FAILED);
	self->meta = (struct trace_buffer_meta *)map;

	meta_len = self->meta->meta_page_size;
	data_len = self->meta->subbuf_size * self->meta->nr_subbufs;

	map = mmap(NULL, data_len, PROT_READ, MAP_SHARED, self->cpu_fd, meta_len);
	ASSERT_NE(map, MAP_FAILED);
	self->data = map;

	/*
	 * Ensure generated events will be found on this very same ring-buffer.
	 */
	CPU_ZERO(&cpu_mask);
	CPU_SET(cpu, &cpu_mask);
	ASSERT_EQ(sched_setaffinity(0, sizeof(cpu_mask), &cpu_mask), 0);
}

FIXTURE_TEARDOWN(map)
{
	tracefs_reset();

	if (self->umount)
		tracefs_unmount();

	munmap(self->data, self->meta->subbuf_size * self->meta->nr_subbufs);
	munmap(self->meta, self->meta->meta_page_size);
	close(self->cpu_fd);
}

TEST_F(map, meta_page_check)
{
	int cnt = 0;

	ASSERT_EQ(self->meta->entries, 0);
	ASSERT_EQ(self->meta->overrun, 0);
	ASSERT_EQ(self->meta->read, 0);
	ASSERT_EQ(self->meta->subbufs_touched, 0);
	ASSERT_EQ(self->meta->subbufs_lost, 0);

	ASSERT_EQ(self->meta->reader.id, 0);
	ASSERT_EQ(self->meta->reader.read, 0);

	ASSERT_EQ(ioctl(self->cpu_fd, TRACE_MMAP_IOCTL_GET_READER), 0);
	ASSERT_EQ(self->meta->reader.id, 0);

	tracefs_write_int(TRACEFS_ROOT"/tracing_on", 1);
	for (int i = 0; i < 16; i++)
		tracefs_write_int(TRACEFS_ROOT"/trace_marker", i);
again:
	ASSERT_EQ(ioctl(self->cpu_fd, TRACE_MMAP_IOCTL_GET_READER), 0);

	ASSERT_EQ(self->meta->entries, 16);
	ASSERT_EQ(self->meta->overrun, 0);
	ASSERT_EQ(self->meta->read, 16);
	/* subbufs_touched doesn't take into account the commit page */
	ASSERT_EQ(self->meta->subbufs_touched, 0);
	ASSERT_EQ(self->meta->subbufs_lost, 0);

	ASSERT_EQ(self->meta->reader.id, 1);

	if (!(cnt++))
		goto again;
}

TEST_HARNESS_MAIN
