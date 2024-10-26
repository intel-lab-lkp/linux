/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __PIDFD_HELPERS_H
#define __PIDFD_HELPERS_H

#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../kselftest.h"

static inline int wait_for_pid(pid_t pid)
{
	int status, ret;

again:
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
		if (errno == EINTR)
			goto again;

		ksft_print_msg("waitpid returned -1, errno=%d\n", errno);
		return -1;
	}

	if (!WIFEXITED(status)) {
		ksft_print_msg(
		       "waitpid !WIFEXITED, WIFSIGNALED=%d, WTERMSIG=%d\n",
		       WIFSIGNALED(status), WTERMSIG(status));
		return -1;
	}

	ret = WEXITSTATUS(status);
	return ret;
}

#endif /* __PIDFD_HELPERS_H */
