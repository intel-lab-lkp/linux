// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/spawn_template.h>

#include "kselftest.h"

#ifndef __NR_spawn_template_create
#define __NR_spawn_template_create 472
#endif

#ifndef __NR_spawn_template_spawn
#define __NR_spawn_template_spawn 473
#endif

#define SPAWN_TEMPLATE_MISSING_SYSCALL_ERRNO	38
#define SPAWN_TEMPLATE_KERNEL_NSIG		64
#define SPAWN_TEMPLATE_KERNEL_SIGSET_WORDS	\
	(SPAWN_TEMPLATE_KERNEL_NSIG / (8 * sizeof(unsigned long)))

static const char *true_path;
static char self_path[PATH_MAX];

struct spawn_template_kernel_sigset {
	unsigned long sig[SPAWN_TEMPLATE_KERNEL_SIGSET_WORDS];
};

static void spawn_template_kernel_sigempty(struct spawn_template_kernel_sigset *set)
{
	memset(set, 0, sizeof(*set));
}

static void spawn_template_kernel_sigadd(struct spawn_template_kernel_sigset *set,
					 int sig)
{
	sig--;
	set->sig[sig / (8 * sizeof(unsigned long))] |=
		1UL << (sig % (8 * sizeof(unsigned long)));
}

static int read_fd_string(int fd, const char *expected)
{
	char buf[128];
	ssize_t nread;

	nread = read(fd, buf, sizeof(buf) - 1);
	if (nread < 0)
		return -errno;

	buf[nread] = '\0';
	return strcmp(buf, expected) ? -EINVAL : 0;
}

static int write_file(const char *path, const char *data, mode_t mode)
{
	size_t left = strlen(data);
	const char *p = data;
	int fd;
	int ret = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -errno;

	while (left) {
		ssize_t written = write(fd, p, left);

		if (written < 0) {
			ret = -errno;
			break;
		}
		left -= written;
		p += written;
	}

	close(fd);
	return ret;
}

static int create_template_path(const char *path)
{
	struct spawn_template_create_args args = {
		.flags = SPAWN_TEMPLATE_CREATE_CLOEXEC,
		.execfd = -1,
		.filename = (uintptr_t)path,
	};

	return syscall(__NR_spawn_template_create, &args, sizeof(args));
}

static int create_template_fd(int execfd)
{
	struct spawn_template_create_args args = {
		.flags = SPAWN_TEMPLATE_CREATE_CLOEXEC,
		.execfd = execfd,
	};

	return syscall(__NR_spawn_template_create, &args, sizeof(args));
}

static int spawn_template_start(int template_fd, char *const argv[],
				struct spawn_template_action *actions,
				unsigned int actions_len,
				unsigned long long flags, pid_t *pid_out,
				int *pidfd_out)
{
	char *const envp[] = { "PATH=/usr/bin:/bin", NULL };
	struct spawn_template_spawn_args args = {
		.flags = flags,
		.argv = (uintptr_t)argv,
		.envp = (uintptr_t)envp,
		.actions = (uintptr_t)actions,
		.actions_len = actions_len,
	};
	int pidfd = -1;
	pid_t pid;
	int ret;

	args.pidfd = (uintptr_t)&pidfd;

	pid = syscall(__NR_spawn_template_spawn, template_fd, &args,
		      sizeof(args));
	if (pid < 0) {
		ret = -errno;
		if (pidfd >= 0) {
			siginfo_t info;

			waitid(P_PIDFD, pidfd, &info, WEXITED);
			close(pidfd);
		}
		return ret;
	}

	*pid_out = pid;
	*pidfd_out = pidfd;
	return 0;
}

static int spawn_template(int template_fd, char *const argv[],
			  struct spawn_template_action *actions,
			  unsigned int actions_len, unsigned long long flags)
{
	siginfo_t info = {};
	int pidfd;
	pid_t pid;
	int ret;

	ret = spawn_template_start(template_fd, argv, actions, actions_len, flags,
				   &pid, &pidfd);
	if (ret)
		return ret;
	(void)pid;

	ret = waitid(P_PIDFD, pidfd, &info, WEXITED);
	if (ret < 0) {
		ret = -errno;
		goto out_close_pidfd;
	}

	if (info.si_code != CLD_EXITED) {
		ret = -EINVAL;
		goto out_close_pidfd;
	}

	ret = info.si_status;

out_close_pidfd:
	if (pidfd >= 0)
		close(pidfd);
	return ret;
}

static const char *find_true(void)
{
	static const char * const paths[] = {
		"/usr/bin/true",
		"/bin/true",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		if (access(paths[i], X_OK) == 0)
			return paths[i];
	}
	return NULL;
}

static int copy_file(const char *src, const char *dst)
{
	char buf[8192];
	ssize_t nread;
	int infd;
	int outfd;
	int ret = 0;

	infd = open(src, O_RDONLY | O_CLOEXEC);
	if (infd < 0)
		return -errno;

	outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
	if (outfd < 0) {
		ret = -errno;
		goto out_close_in;
	}

	while ((nread = read(infd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		ssize_t left = nread;

		while (left > 0) {
			ssize_t written = write(outfd, p, left);

			if (written < 0) {
				ret = -errno;
				goto out_close_out;
			}
			left -= written;
			p += written;
		}
	}
	if (nread < 0)
		ret = -errno;

out_close_out:
	close(outfd);
out_close_in:
	close(infd);
	return ret;
}

static int test_basic_spawn(void)
{
	char *const argv[] = { (char *)true_path, NULL };
	int template_fd;
	int ret;

	template_fd = create_template_path(true_path);
	if (template_fd < 0)
		return -errno;

	ret = spawn_template(template_fd, argv, NULL, 0, 0);
	close(template_fd);
	return ret;
}

static int test_relative_path_rejected(void)
{
	int template_fd;

	template_fd = create_template_path("true");
	if (template_fd >= 0) {
		close(template_fd);
		return -EINVAL;
	}

	return errno == EINVAL ? 0 : -errno;
}

static int test_execfd_requires_execute(void)
{
	char path[] = "/tmp/spawn-template-noexec-XXXXXX";
	int template_fd;
	int fd;
	int ret = 0;

	fd = mkstemp(path);
	if (fd < 0)
		return -errno;

	if (fchmod(fd, 0600)) {
		ret = -errno;
		goto out;
	}

	template_fd = create_template_fd(fd);
	if (template_fd >= 0) {
		close(template_fd);
		ret = -EINVAL;
		goto out;
	}

	ret = errno == EACCES ? 0 : -errno;

out:
	close(fd);
	unlink(path);
	return ret;
}

static int test_default_closes_extra_fds(void)
{
	char fdarg[32];
	char *const argv[] = {
		self_path,
		"--check-fd-closed",
		fdarg,
		NULL,
	};
	int template_fd;
	int extra_fd;
	int ret;

	extra_fd = open("/dev/null", O_RDONLY);
	if (extra_fd < 0)
		return -errno;

	snprintf(fdarg, sizeof(fdarg), "%d", extra_fd);

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_close_extra;
	}

	ret = spawn_template(template_fd, argv, NULL, 0, 0);
	close(template_fd);

out_close_extra:
	close(extra_fd);
	return ret;
}

static int test_close_range_max_action(void)
{
	char fdarg[32];
	char *const argv[] = {
		self_path,
		"--check-fd-closed",
		fdarg,
		NULL,
	};
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_CLOSE_RANGE,
		.fd = -1,
		.newfd = -1,
	};
	int template_fd;
	int extra_fd;
	int ret;

	extra_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (extra_fd < 0)
		return -errno;

	action.fd = extra_fd;
	snprintf(fdarg, sizeof(fdarg), "%d", extra_fd);

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_close_extra;
	}

	ret = spawn_template(template_fd, argv, &action, 1,
			     SPAWN_TEMPLATE_SPAWN_INHERIT_FDS);
	close(template_fd);

out_close_extra:
	close(extra_fd);
	return ret;
}

static int test_dup2_stdio_actions(void)
{
	char *const argv[] = { self_path, "--write-stdio", NULL };
	struct spawn_template_action actions[2];
	char out_buf[32];
	char err_buf[32];
	int out_pipe[2];
	int err_pipe[2];
	int template_fd;
	int ret = 0;

	if (pipe2(out_pipe, O_CLOEXEC))
		return -errno;
	if (pipe2(err_pipe, O_CLOEXEC)) {
		ret = -errno;
		goto out_close_out_pipe;
	}

	actions[0] = (struct spawn_template_action) {
		.type = SPAWN_TEMPLATE_ACTION_DUP2,
		.fd = out_pipe[1],
		.newfd = STDOUT_FILENO,
	};
	actions[1] = (struct spawn_template_action) {
		.type = SPAWN_TEMPLATE_ACTION_DUP2,
		.fd = err_pipe[1],
		.newfd = STDERR_FILENO,
	};

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_close_err_pipe;
	}

	ret = spawn_template(template_fd, argv, actions, ARRAY_SIZE(actions), 0);
	close(template_fd);
	if (ret)
		goto out_close_err_pipe;

	close(out_pipe[1]);
	out_pipe[1] = -1;
	close(err_pipe[1]);
	err_pipe[1] = -1;

	memset(out_buf, 0, sizeof(out_buf));
	memset(err_buf, 0, sizeof(err_buf));
	if (read(out_pipe[0], out_buf, sizeof(out_buf) - 1) < 0) {
		ret = -errno;
		goto out_close_err_pipe;
	}
	if (read(err_pipe[0], err_buf, sizeof(err_buf) - 1) < 0) {
		ret = -errno;
		goto out_close_err_pipe;
	}
	if (strcmp(out_buf, "stdout-token\n") ||
	    strcmp(err_buf, "stderr-token\n"))
		ret = -EINVAL;

out_close_err_pipe:
	if (err_pipe[1] >= 0)
		close(err_pipe[1]);
	close(err_pipe[0]);
out_close_out_pipe:
	if (out_pipe[1] >= 0)
		close(out_pipe[1]);
	close(out_pipe[0]);
	return ret;
}

static int test_open_action_stdin(void)
{
	char dir[] = "/tmp/spawn-template-open-XXXXXX";
	char path[PATH_MAX];
	char *const argv[] = {
		self_path,
		"--check-fd-content",
		"0",
		"open-action-token\n",
		NULL,
	};
	struct spawn_template_open open_arg = {
		.path = (uintptr_t)path,
		.how = {
			.flags = O_RDONLY,
		},
	};
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_OPEN,
		.fd = AT_FDCWD,
		.newfd = STDIN_FILENO,
		.arg = (uintptr_t)&open_arg,
	};
	int template_fd;
	int ret;

	if (!mkdtemp(dir))
		return -errno;

	snprintf(path, sizeof(path), "%s/input", dir);
	ret = write_file(path, "open-action-token\n", 0600);
	if (ret)
		goto out_unlink;

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_unlink;
	}

	ret = spawn_template(template_fd, argv, &action, 1, 0);
	close(template_fd);

out_unlink:
	unlink(path);
	rmdir(dir);
	return ret;
}

static int test_fchdir_action(void)
{
	char dir[] = "/tmp/spawn-template-fchdir-XXXXXX";
	char resolved[PATH_MAX];
	char *const argv[] = {
		self_path,
		"--check-cwd",
		resolved,
		NULL,
	};
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_FCHDIR,
	};
	int template_fd;
	int dirfd;
	int ret;

	if (!mkdtemp(dir))
		return -errno;
	if (!realpath(dir, resolved)) {
		ret = -errno;
		goto out_rmdir;
	}

	dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0) {
		ret = -errno;
		goto out_rmdir;
	}
	action.fd = dirfd;

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_close_dirfd;
	}

	ret = spawn_template(template_fd, argv, &action, 1, 0);
	close(template_fd);

out_close_dirfd:
	close(dirfd);
out_rmdir:
	rmdir(dir);
	return ret;
}

static int test_sigmask_action(void)
{
	char sigarg[16];
	char *const argv[] = {
		self_path,
		"--check-sigmask",
		sigarg,
		NULL,
	};
	struct spawn_template_kernel_sigset mask;
	struct spawn_template_sigset sigset_arg = {
		.sigset = (uintptr_t)&mask,
		.sigsetsize = sizeof(mask),
	};
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_SIGMASK,
		.arg = (uintptr_t)&sigset_arg,
	};
	int template_fd;
	int ret;

	spawn_template_kernel_sigempty(&mask);
	spawn_template_kernel_sigadd(&mask, SIGUSR1);
	snprintf(sigarg, sizeof(sigarg), "%d", SIGUSR1);

	template_fd = create_template_path(self_path);
	if (template_fd < 0)
		return -errno;

	ret = spawn_template(template_fd, argv, &action, 1, 0);
	close(template_fd);
	return ret;
}

static int test_sigdefault_action(void)
{
	char sigarg[16];
	char *const argv[] = {
		self_path,
		"--check-sigdefault",
		sigarg,
		NULL,
	};
	struct spawn_template_kernel_sigset mask;
	struct sigaction old_sa;
	struct sigaction ignore_sa = {
		.sa_handler = SIG_IGN,
	};
	struct spawn_template_sigset sigset_arg = {
		.sigset = (uintptr_t)&mask,
		.sigsetsize = sizeof(mask),
	};
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_SIGDEFAULT,
		.arg = (uintptr_t)&sigset_arg,
	};
	int template_fd;
	int ret;

	spawn_template_kernel_sigempty(&mask);
	spawn_template_kernel_sigadd(&mask, SIGUSR1);
	snprintf(sigarg, sizeof(sigarg), "%d", SIGUSR1);

	if (sigaction(SIGUSR1, &ignore_sa, &old_sa))
		return -errno;

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_restore_signal;
	}

	ret = spawn_template(template_fd, argv, &action, 1, 0);
	close(template_fd);

out_restore_signal:
	sigaction(SIGUSR1, &old_sa, NULL);
	return ret;
}

static int test_inherit_fds_flag(void)
{
	char fdarg[32];
	char *const argv[] = {
		self_path,
		"--check-fd-open",
		fdarg,
		NULL,
	};
	int template_fd;
	int extra_fd;
	int ret;

	extra_fd = open("/dev/null", O_RDONLY);
	if (extra_fd < 0)
		return -errno;
	snprintf(fdarg, sizeof(fdarg), "%d", extra_fd);

	template_fd = create_template_path(self_path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_close_extra;
	}

	ret = spawn_template(template_fd, argv, NULL, 0,
			     SPAWN_TEMPLATE_SPAWN_INHERIT_FDS);
	close(template_fd);

out_close_extra:
	close(extra_fd);
	return ret;
}

static int test_pidfd_waitid(void)
{
	char *const argv[] = { (char *)true_path, NULL };
	siginfo_t info = {};
	int template_fd;
	int pidfd;
	pid_t pid;
	int ret;

	template_fd = create_template_path(true_path);
	if (template_fd < 0)
		return -errno;

	ret = spawn_template_start(template_fd, argv, NULL, 0, 0, &pid, &pidfd);
	close(template_fd);
	if (ret)
		return ret;

	ret = waitid(P_PIDFD, pidfd, &info, WEXITED);
	if (ret < 0) {
		ret = -errno;
		waitpid(pid, NULL, 0);
		goto out_close_pidfd;
	}
	if (info.si_code != CLD_EXITED || info.si_status)
		ret = -EINVAL;

out_close_pidfd:
	close(pidfd);
	return ret;
}

static int test_create_actions_rejected(void)
{
	struct spawn_template_action action = {
		.type = SPAWN_TEMPLATE_ACTION_CLOSE,
		.fd = STDIN_FILENO,
	};
	struct spawn_template_create_args args = {
		.flags = SPAWN_TEMPLATE_CREATE_CLOEXEC,
		.execfd = -1,
		.filename = (uintptr_t)true_path,
		.actions = (uintptr_t)&action,
		.actions_len = 1,
	};
	int template_fd;

	template_fd = syscall(__NR_spawn_template_create, &args, sizeof(args));
	if (template_fd >= 0) {
		close(template_fd);
		return -EINVAL;
	}

	return errno == EINVAL ? 0 : -errno;
}

static int test_script_template_unsupported(void)
{
	char dir[] = "/tmp/spawn-template-script-XXXXXX";
	char path[PATH_MAX];
	int template_fd;
	int ret;

	if (!mkdtemp(dir))
		return -errno;

	snprintf(path, sizeof(path), "%s/script", dir);
	ret = write_file(path, "#!/bin/sh\nexit 0\n", 0700);
	if (ret)
		goto out_unlink;

	template_fd = create_template_path(path);
	if (template_fd >= 0) {
		close(template_fd);
		ret = -EINVAL;
		goto out_unlink;
	}
	ret = errno == ENOEXEC ? 0 : -errno;

out_unlink:
	unlink(path);
	rmdir(dir);
	return ret;
}

static int test_deny_write_while_template_alive(void)
{
	char dir[] = "/tmp/spawn-template-deny-write-XXXXXX";
	char path[PATH_MAX];
	int template_fd;
	int write_fd;
	int ret = 0;

	if (!mkdtemp(dir))
		return -errno;

	snprintf(path, sizeof(path), "%s/copy", dir);
	ret = copy_file(self_path, path);
	if (ret)
		goto out_unlink;

	template_fd = create_template_path(path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_unlink;
	}

	write_fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	if (write_fd >= 0) {
		close(write_fd);
		ret = -EINVAL;
	} else {
		ret = errno == ETXTBSY ? 0 : -errno;
	}

	close(template_fd);
out_unlink:
	unlink(path);
	rmdir(dir);
	return ret;
}

static int test_stale_path_rejected(void)
{
	char dir[] = "/tmp/spawn-template-stale-XXXXXX";
	char path[PATH_MAX];
	char *const argv[] = { path, "--exit-zero", NULL };
	int template_fd;
	int ret = 0;

	if (!mkdtemp(dir))
		return -errno;

	snprintf(path, sizeof(path), "%s/copy", dir);
	ret = copy_file(self_path, path);
	if (ret)
		goto out_unlink;

	template_fd = create_template_path(path);
	if (template_fd < 0) {
		ret = -errno;
		goto out_unlink;
	}

	if (chmod(path, 0600)) {
		ret = -errno;
		goto out_close_template;
	}

	ret = spawn_template(template_fd, argv, NULL, 0, 0);
	if (ret >= 0)
		ret = -EINVAL;
	else
		ret = ret == -ESTALE ? 0 : ret;

out_close_template:
	close(template_fd);
out_unlink:
	unlink(path);
	rmdir(dir);
	return ret;
}

static int test_path_replacement_allows_tool_update(void)
{
	char dir[] = "/tmp/spawn-template-update-XXXXXX";
	char path[PATH_MAX];
	char new_path[PATH_MAX];
	char *const argv[] = { path, "--exit-zero", NULL };
	int new_template_fd = -1;
	int template_fd = -1;
	int ret;

	if (!mkdtemp(dir))
		return -errno;

	snprintf(path, sizeof(path), "%s/tool", dir);
	snprintf(new_path, sizeof(new_path), "%s/tool.new", dir);
	ret = copy_file(self_path, path);
	if (ret)
		goto out;
	ret = copy_file(self_path, new_path);
	if (ret)
		goto out;

	template_fd = create_template_path(path);
	if (template_fd < 0) {
		ret = -errno;
		goto out;
	}

	if (rename(new_path, path)) {
		ret = -errno;
		goto out;
	}

	ret = spawn_template(template_fd, argv, NULL, 0, 0);
	if (ret != -ESTALE) {
		ret = ret < 0 ? ret : -EINVAL;
		goto out;
	}

	new_template_fd = create_template_path(path);
	if (new_template_fd < 0) {
		ret = -errno;
		goto out;
	}

	ret = spawn_template(new_template_fd, argv, NULL, 0, 0);

out:
	if (new_template_fd >= 0)
		close(new_template_fd);
	if (template_fd >= 0)
		close(template_fd);
	unlink(new_path);
	unlink(path);
	rmdir(dir);
	return ret;
}

static void run_test(const char *name, int (*fn)(void))
{
	int ret = fn();

	if (!ret)
		ksft_test_result_pass("%s\n", name);
	else
		ksft_test_result_fail("%s failed: %s (%d)\n",
				      name, strerror(-ret), -ret);
}

static void check_syscall_available(void)
{
	int template_fd;

	template_fd = create_template_path(true_path);
	if (template_fd >= 0) {
		close(template_fd);
		return;
	}

	if (errno == SPAWN_TEMPLATE_MISSING_SYSCALL_ERRNO)
		ksft_exit_skip("spawn_template syscalls are not available\n");

	ksft_exit_fail_msg("spawn_template_create failed: %s (%d)\n",
			   strerror(errno), errno);
}

int main(int argc, char **argv)
{
	ssize_t len;

	if (argc == 2 && !strcmp(argv[1], "--exit-zero"))
		return 0;

	if (argc == 3 && !strcmp(argv[1], "--check-fd-closed")) {
		int fd = atoi(argv[2]);

		return fcntl(fd, F_GETFD) < 0 && errno == EBADF ? 0 : 1;
	}

	if (argc == 3 && !strcmp(argv[1], "--check-fd-open")) {
		int fd = atoi(argv[2]);

		return fcntl(fd, F_GETFD) >= 0 ? 0 : 1;
	}

	if (argc == 4 && !strcmp(argv[1], "--check-fd-content"))
		return read_fd_string(atoi(argv[2]), argv[3]) ? 1 : 0;

	if (argc == 3 && !strcmp(argv[1], "--check-cwd")) {
		char cwd[PATH_MAX];

		if (!getcwd(cwd, sizeof(cwd)))
			return 1;
		return strcmp(cwd, argv[2]) ? 1 : 0;
	}

	if (argc == 3 && !strcmp(argv[1], "--check-sigmask")) {
		sigset_t mask;
		int sig = atoi(argv[2]);

		if (sigprocmask(SIG_BLOCK, NULL, &mask))
			return 1;
		return sigismember(&mask, sig) == 1 ? 0 : 1;
	}

	if (argc == 3 && !strcmp(argv[1], "--check-sigdefault")) {
		struct sigaction sa;
		int sig = atoi(argv[2]);

		if (sigaction(sig, NULL, &sa))
			return 1;
		return sa.sa_handler == SIG_DFL ? 0 : 1;
	}

	if (argc == 2 && !strcmp(argv[1], "--write-stdio")) {
		if (write(STDOUT_FILENO, "stdout-token\n", 13) != 13)
			return 1;
		if (write(STDERR_FILENO, "stderr-token\n", 13) != 13)
			return 1;
		return 0;
	}

	true_path = find_true();
	if (!true_path)
		ksft_exit_skip("could not find true executable\n");

	len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
	if (len < 0)
		ksft_exit_fail_msg("readlink(/proc/self/exe) failed: %s\n",
				   strerror(errno));
	self_path[len] = '\0';

	check_syscall_available();

	ksft_print_header();
	ksft_set_plan(17);

	run_test("basic spawn", test_basic_spawn);
	run_test("relative path rejected", test_relative_path_rejected);
	run_test("execfd execute permission checked",
		 test_execfd_requires_execute);
	run_test("default fd close", test_default_closes_extra_fds);
	run_test("close_range action max fd", test_close_range_max_action);
	run_test("dup2 stdio actions", test_dup2_stdio_actions);
	run_test("open action stdin", test_open_action_stdin);
	run_test("fchdir action", test_fchdir_action);
	run_test("sigmask action", test_sigmask_action);
	run_test("sigdefault action", test_sigdefault_action);
	run_test("inherit fds flag", test_inherit_fds_flag);
	run_test("pidfd waitid", test_pidfd_waitid);
	run_test("create-time actions rejected", test_create_actions_rejected);
	run_test("script template unsupported", test_script_template_unsupported);
	run_test("deny write while template alive",
		 test_deny_write_while_template_alive);
	run_test("stale path rejected", test_stale_path_rejected);
	run_test("path replacement allows tool update",
		 test_path_replacement_allows_tool_update);

	ksft_finished();
}
