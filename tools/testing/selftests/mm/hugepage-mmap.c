// SPDX-License-Identifier: GPL-2.0
/*
 * hugepage-mmap:
 *
 * Example of using huge page memory in a user application using the mmap
 * system call.  Before running this application, make sure that the
 * administrator has mounted the hugetlbfs filesystem (on some directory
 * like /mnt) using the command mount -t hugetlbfs nodev /mnt. In this
 * example, the app is requesting memory of size 256MB that is backed by
 * huge pages.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <limits.h>
#include "kselftest_harness.h"

static int hugetlbfs_unlinked_fd(const char *mount)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/hugepage-mmap-XXXXXX", mount);
	fd = mkstemp(path);
	if (fd < 0)
		return fd;

	unlink(path);
	return fd;
}

#define LENGTH (256UL*1024*1024)
#define PROTECTION (PROT_READ | PROT_WRITE)

enum fd_kind {
	MEMFD,
	HUGETLBFS_FD,
};

FIXTURE(hugepage_mmap)
{
	int fd;
	char *addr;
	char mount_dir[PATH_MAX];
};

static int fd_create(struct __test_metadata *_metadata,
		FIXTURE_DATA(hugepage_mmap) *self, enum fd_kind fd_kind)
{
	switch (fd_kind) {
	case MEMFD:
		return memfd_create("hugepage-mmap", MFD_HUGETLB);
	case HUGETLBFS_FD: {
		snprintf(self->mount_dir, sizeof(self->mount_dir),
			 "/tmp/hugepage-mmap-XXXXXX");
		ASSERT_NE(NULL, mkdtemp(self->mount_dir))
		{
			TH_LOG("mkdtemp() failed: %s", strerror(errno));
		}
		ASSERT_EQ(0, mount("none", self->mount_dir, "hugetlbfs", 0,
				   "pagesize=2M"))
		{
			TH_LOG("mount() failed: %s", strerror(errno));
		}
		return hugetlbfs_unlinked_fd(self->mount_dir);
	}
	default:
		return -1;
	};
}

FIXTURE_VARIANT(hugepage_mmap)
{
	enum fd_kind fd_kind;
};

FIXTURE_VARIANT_ADD(hugepage_mmap, memfd)
{
	.fd_kind = MEMFD,
};

FIXTURE_VARIANT_ADD(hugepage_mmap, hugetlbfs_fd)
{
	.fd_kind = HUGETLBFS_FD,
};

FIXTURE_SETUP(hugepage_mmap)
{
	self->fd = -1;
	self->addr = MAP_FAILED;
	self->mount_dir[0] = '\0';
	/* Enable teardown to cleanup on any assertions in FIXTURE_SETUP(). */
	*_metadata->no_teardown = false;

	self->fd = fd_create(_metadata, self, variant->fd_kind);
	ASSERT_GE(self->fd, 0) {
		TH_LOG("fd creation failed: %s", strerror(errno));
	}

	self->addr = mmap(NULL, LENGTH, PROTECTION, MAP_SHARED, self->fd, 0);
	ASSERT_NE(MAP_FAILED, self->addr) {
		TH_LOG("mmap(): %s", strerror(errno));
	}
	TH_LOG("Returned address is %p", self->addr);
}

FIXTURE_TEARDOWN(hugepage_mmap)
{
	if (self->addr != MAP_FAILED)
		munmap(self->addr, LENGTH);
	if (self->fd >= 0)
		close(self->fd);

	if (variant->fd_kind == HUGETLBFS_FD && self->mount_dir[0]) {
		umount(self->mount_dir);
		rmdir(self->mount_dir);
	}
}

TEST_F(hugepage_mmap, read_write)
{
	unsigned long i;

	TH_LOG("First hex is %x", *((unsigned int *)self->addr));

	for (i = 0; i < LENGTH; i++)
		self->addr[i] = (char)i;

	TH_LOG("First hex is %x", *((unsigned int *)self->addr));

	for (i = 0; i < LENGTH; i++) {
		ASSERT_EQ(self->addr[i], (char)i) {
			TH_LOG("Error: Mismatch at %lu\n", i);
		};
	}
}

TEST_HARNESS_MAIN
