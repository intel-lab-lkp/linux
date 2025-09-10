// SPDX-License-Identifier: GPL-2.0+
/* devmem test secret.c
 *
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by Alessandro Carminati (acarmina@redhat.com)
 */

#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>


static int memfd_secret(unsigned int flags)
{
	return syscall(SYS_memfd_secret, flags);
}

void *secret_alloc(size_t size)
{
	int fd = -1;
	void *m;
	void *result = NULL;

	fd = memfd_secret(0);
	if (fd < 0)
		goto out;

	if (ftruncate(fd, size) < 0)
		goto out;

	m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		goto out;

	result = m;

out:
	if (fd >= 0)
		close(fd);
	return result;
}

void secret_free(void *p, size_t size)
{
	munmap(p, size);
}
