/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/compiler.h>
#include <uapi/asm-generic/unistd.h> // for __NR_landlock_add_rule
#include <unistd.h>
#include "../tests.h"
#include <stdlib.h>
#ifdef __NR_landlock_add_rule
#include <linux/landlock.h>
#endif

/* This workload is used only to test enum augmentation with BTF in perf trace */
static int landlock(int argc __maybe_unused, const char **argv __maybe_unused)
{
#if defined(__NR_landlock_add_rule) && defined(HAVE_LIBBPF_SUPPORT)
	int fd = 11, flags = 45;

	struct landlock_path_beneath_attr path_beneath_attr = {
		.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE,
		.parent_fd = 14,
	};

	struct landlock_net_port_attr net_port_attr = {
		.port = 19,
		.allowed_access = LANDLOCK_ACCESS_NET_CONNECT_TCP,
	};

	syscall(__NR_landlock_add_rule, fd, LANDLOCK_RULE_PATH_BENEATH,
		&path_beneath_attr, flags);

	syscall(__NR_landlock_add_rule, fd, LANDLOCK_RULE_NET_PORT,
		&net_port_attr, flags);

	return 0;
#else
	return 2;
#endif
}

DEFINE_WORKLOAD(landlock);
