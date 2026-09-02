// SPDX-License-Identifier: GPL-2.0
/*
 * Assign the guest->host vsock transport, i.e. the guest's virtio-vsock
 * device, to the network namespace of the invoking process.
 *
 * Exits with the ioctl's errno, so that callers can tell why it was refused.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#ifndef IOCTL_VM_SOCKETS_ASSIGN_G2H_NETNS
#define IOCTL_VM_SOCKETS_ASSIGN_G2H_NETNS	_IO(7, 0xba)
#endif

int main(void)
{
	int fd, ret;

	fd = open("/dev/vsock", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open /dev/vsock: %s\n", strerror(errno));
		return -1;
	}

	ret = ioctl(fd, IOCTL_VM_SOCKETS_ASSIGN_G2H_NETNS);
	if (ret < 0) {
		ret = errno;
		fprintf(stderr,
			"IOCTL_VM_SOCKETS_ASSIGN_G2H_NETNS: %s (errno %d)\n",
			strerror(errno), errno);
	}

	close(fd);

	return ret;
}
