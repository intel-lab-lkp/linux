/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/compiler.h>
#include <uapi/asm-generic/unistd.h> // for __NR_landlock_add_rule
#include <unistd.h>
#include <linux/landlock.h>
#include "../tests.h"

static int landlock_add_rule(int argc __maybe_unused, const char **argv __maybe_unused)
{
	int fd = 11;
	int flags = 45;

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
}

DEFINE_WORKLOAD(landlock_add_rule);
