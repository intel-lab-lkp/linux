// SPDX-License-Identifier: GPL-2.0
#include <unistd.h>
#include <fcntl.h>

#define MSG_RIGHTS_DENIAL 0x200000
#define MSG_RIGHTS_FILTER 0x400000

#define CMSG_IS_SCM_RIGHTS(cmsg) ({		\
	typeof(cmsg) _cmsg = (cmsg);		\
	_cmsg &&				\
	_cmsg->cmsg_level == SOL_SOCKET &&	\
	_cmsg->cmsg_type == SCM_RIGHTS;		\
})

#define MIN(a, b) ({ \
	typeof(a) _a = (a); \
	typeof(b) _b = (b); \
	_a < _b ? _a : _b; \
})

#define MAX_FDS 10

static inline int read_current_label(char *label, size_t size)
{
	int fd = open("/proc/self/attr/current", O_RDONLY);
	if (fd < 0)
		return fd;

	ssize_t r = read(fd, label, size - 1);
	close(fd);
	if (r <= 0)
		return r;

	label[r] = '\0';

	return 0;
}
