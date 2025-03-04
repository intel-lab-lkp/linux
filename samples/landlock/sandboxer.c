// SPDX-License-Identifier: BSD-3-Clause
/*
 * Simple Landlock sandbox manager able to execute a process restricted by
 * user-defined file system and network access control policies.
 *
 * Copyright © 2017-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2020 ANSSI
 */

#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <linux/socket.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdbool.h>
#include <poll.h>
#include <pthread.h>
#include <sys/wait.h>
#include <termios.h>
#include <linux/limits.h>
#include <stdint.h>

#ifndef landlock_create_ruleset
static inline int
landlock_create_ruleset(struct landlock_ruleset_attr *const attr,
			const size_t size, const __u32 flags)
{
	return syscall(__NR_landlock_create_ruleset, attr, size, flags);
}
#endif

#ifndef landlock_add_rule
static inline int landlock_add_rule(const int ruleset_fd,
				    const enum landlock_rule_type rule_type,
				    const void *const rule_attr,
				    const __u32 flags)
{
	return syscall(__NR_landlock_add_rule, ruleset_fd, rule_type, rule_attr,
		       flags);
}
#endif

#ifndef landlock_restrict_self
static inline int landlock_restrict_self(const int ruleset_fd,
					 const __u32 flags)
{
	return syscall(__NR_landlock_restrict_self, ruleset_fd, flags);
}
#endif

#define ENV_FS_RO_NAME "LL_FS_RO"
#define ENV_FS_RW_NAME "LL_FS_RW"
#define ENV_TCP_BIND_NAME "LL_TCP_BIND"
#define ENV_TCP_CONNECT_NAME "LL_TCP_CONNECT"
#define ENV_SCOPED_NAME "LL_SCOPED"
#define ENV_SUPERVISE "LL_SUPERVISE"
#define ENV_DELIMITER ":"

static int str2num(const char *numstr, __u64 *num_dst)
{
	char *endptr = NULL;
	int err = 0;
	__u64 num;

	errno = 0;
	num = strtoull(numstr, &endptr, 10);
	if (errno != 0)
		err = errno;
	/* Was the string empty, or not entirely parsed successfully? */
	else if ((*numstr == '\0') || (*endptr != '\0'))
		err = EINVAL;
	else
		*num_dst = num;

	return err;
}

static int parse_path(char *env_path, const char ***const path_list)
{
	int i, num_paths = 0;

	if (env_path) {
		num_paths++;
		for (i = 0; env_path[i]; i++) {
			if (env_path[i] == ENV_DELIMITER[0])
				num_paths++;
		}
	}
	*path_list = malloc(num_paths * sizeof(**path_list));
	if (!*path_list)
		return -1;

	for (i = 0; i < num_paths; i++)
		(*path_list)[i] = strsep(&env_path, ENV_DELIMITER);

	return num_paths;
}

/* clang-format off */

#define ACCESS_FILE ( \
	LANDLOCK_ACCESS_FS_EXECUTE | \
	LANDLOCK_ACCESS_FS_WRITE_FILE | \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_TRUNCATE | \
	LANDLOCK_ACCESS_FS_IOCTL_DEV)

/* clang-format on */

static int populate_ruleset_fs(const char *const env_var, const int ruleset_fd,
			       const __u64 allowed_access)
{
	int num_paths, i, ret = 1;
	char *env_path_name;
	const char **path_list = NULL;
	struct landlock_path_beneath_attr path_beneath = {
		.parent_fd = -1,
	};

	env_path_name = getenv(env_var);
	if (!env_path_name) {
		/* Prevents users to forget a setting. */
		fprintf(stderr, "Missing environment variable %s\n", env_var);
		return 1;
	}
	env_path_name = strdup(env_path_name);
	unsetenv(env_var);
	num_paths = parse_path(env_path_name, &path_list);
	if (num_paths < 0) {
		fprintf(stderr, "Failed to allocate memory\n");
		goto out_free_name;
	}
	if (num_paths == 1 && path_list[0][0] == '\0') {
		/*
		 * Allows to not use all possible restrictions (e.g. use
		 * LL_FS_RO without LL_FS_RW).
		 */
		ret = 0;
		goto out_free_name;
	}

	for (i = 0; i < num_paths; i++) {
		struct stat statbuf;

		path_beneath.parent_fd = open(path_list[i], O_PATH | O_CLOEXEC);
		if (path_beneath.parent_fd < 0) {
			fprintf(stderr, "Failed to open \"%s\": %s\n",
				path_list[i], strerror(errno));
			continue;
		}
		if (fstat(path_beneath.parent_fd, &statbuf)) {
			fprintf(stderr, "Failed to stat \"%s\": %s\n",
				path_list[i], strerror(errno));
			close(path_beneath.parent_fd);
			goto out_free_name;
		}
		path_beneath.allowed_access = allowed_access;
		if (!S_ISDIR(statbuf.st_mode))
			path_beneath.allowed_access &= ACCESS_FILE;
		if (landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
				      &path_beneath, 0)) {
			fprintf(stderr,
				"Failed to update the ruleset with \"%s\": %s\n",
				path_list[i], strerror(errno));
			close(path_beneath.parent_fd);
			goto out_free_name;
		}
		close(path_beneath.parent_fd);
	}
	ret = 0;

out_free_name:
	free(path_list);
	free(env_path_name);
	return ret;
}

static int populate_ruleset_net(const char *const env_var, const int ruleset_fd,
				const __u64 allowed_access)
{
	int ret = 1;
	char *env_port_name, *env_port_name_next, *strport;
	struct landlock_net_port_attr net_port = {
		.allowed_access = allowed_access,
	};

	env_port_name = getenv(env_var);
	if (!env_port_name)
		return 0;
	env_port_name = strdup(env_port_name);
	unsetenv(env_var);

	env_port_name_next = env_port_name;
	while ((strport = strsep(&env_port_name_next, ENV_DELIMITER))) {
		__u64 port;

		if (strcmp(strport, "") == 0)
			continue;

		if (str2num(strport, &port)) {
			fprintf(stderr, "Failed to parse port at \"%s\"\n",
				strport);
			goto out_free_name;
		}
		net_port.port = port;
		if (landlock_add_rule(ruleset_fd, LANDLOCK_RULE_NET_PORT,
				      &net_port, 0)) {
			fprintf(stderr,
				"Failed to update the ruleset with port \"%llu\": %s\n",
				net_port.port, strerror(errno));
			goto out_free_name;
		}
	}
	ret = 0;

out_free_name:
	free(env_port_name);
	return ret;
}

/* Returns true on error, false otherwise. */
static bool check_ruleset_scope(const char *const env_var,
				struct landlock_ruleset_attr *ruleset_attr)
{
	char *env_type_scope, *env_type_scope_next, *ipc_scoping_name;
	bool error = false;
	bool abstract_scoping = false;
	bool signal_scoping = false;

	/* Scoping is not supported by Landlock ABI */
	if (!(ruleset_attr->scoped &
	      (LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL)))
		goto out_unset;

	env_type_scope = getenv(env_var);
	/* Scoping is not supported by the user */
	if (!env_type_scope || strcmp("", env_type_scope) == 0)
		goto out_unset;

	env_type_scope = strdup(env_type_scope);
	env_type_scope_next = env_type_scope;
	while ((ipc_scoping_name =
			strsep(&env_type_scope_next, ENV_DELIMITER))) {
		if (strcmp("a", ipc_scoping_name) == 0 && !abstract_scoping) {
			abstract_scoping = true;
		} else if (strcmp("s", ipc_scoping_name) == 0 &&
			   !signal_scoping) {
			signal_scoping = true;
		} else {
			fprintf(stderr, "Unknown or duplicate scope \"%s\"\n",
				ipc_scoping_name);
			error = true;
			goto out_free_name;
		}
	}

out_free_name:
	free(env_type_scope);

out_unset:
	if (!abstract_scoping)
		ruleset_attr->scoped &= ~LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET;
	if (!signal_scoping)
		ruleset_attr->scoped &= ~LANDLOCK_SCOPE_SIGNAL;

	unsetenv(env_var);
	return error;
}

/* clang-format off */

#define ACCESS_FS_ROUGHLY_READ ( \
	LANDLOCK_ACCESS_FS_EXECUTE | \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_READ_DIR)

#define ACCESS_FS_ROUGHLY_CREATE ( \
	LANDLOCK_ACCESS_FS_MAKE_CHAR | \
	LANDLOCK_ACCESS_FS_MAKE_DIR | \
	LANDLOCK_ACCESS_FS_MAKE_REG | \
	LANDLOCK_ACCESS_FS_MAKE_SOCK | \
	LANDLOCK_ACCESS_FS_MAKE_FIFO | \
	LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
	LANDLOCK_ACCESS_FS_MAKE_SYM)

#define ACCESS_FS_ROUGHLY_REMOVE ( \
	LANDLOCK_ACCESS_FS_REMOVE_DIR | \
	LANDLOCK_ACCESS_FS_REMOVE_FILE)

#define ACCESS_FS_ROUGHLY_WRITE ( \
	LANDLOCK_ACCESS_FS_WRITE_FILE | \
	ACCESS_FS_ROUGHLY_CREATE | \
	ACCESS_FS_ROUGHLY_REMOVE | \
	LANDLOCK_ACCESS_FS_REFER | \
	LANDLOCK_ACCESS_FS_TRUNCATE | \
	LANDLOCK_ACCESS_FS_IOCTL_DEV)

/* clang-format on */

#define LANDLOCK_ABI_LAST 7

#define XSTR(s) #s
#define STR(s) XSTR(s)

/* clang-format off */

static const char help[] =
	"usage: " ENV_FS_RO_NAME "=\"...\" " ENV_FS_RW_NAME "=\"...\" "
	"[other environment variables] %1$s <cmd> [args]...\n"
	"\n"
	"Execute the given command in a restricted environment.\n"
	"Multi-valued settings (lists of ports, paths, scopes) are colon-delimited.\n"
	"\n"
	"Mandatory settings:\n"
	"* " ENV_FS_RO_NAME ": paths allowed to be used in a read-only way\n"
	"* " ENV_FS_RW_NAME ": paths allowed to be used in a read-write way\n"
	"\n"
	"Optional settings (when not set, their associated access check "
	"is always allowed, which is different from an empty string which "
	"means an empty list):\n"
	"* " ENV_TCP_BIND_NAME ": ports allowed to bind (server)\n"
	"* " ENV_TCP_CONNECT_NAME ": ports allowed to connect (client)\n"
	"* " ENV_SCOPED_NAME ": actions denied on the outside of the landlock domain\n"
	"  - \"a\" to restrict opening abstract unix sockets\n"
	"  - \"s\" to restrict sending signals\n"
	"* " ENV_SUPERVISE ": set to 1 to enable supervisor mode\n"
	"\n"
	"Example:\n"
	ENV_FS_RO_NAME "=\"${PATH}:/lib:/usr:/proc:/etc:/dev/urandom\" "
	ENV_FS_RW_NAME "=\"/dev/null:/dev/full:/dev/zero:/dev/pts:/tmp\" "
	ENV_TCP_BIND_NAME "=\"9418\" "
	ENV_TCP_CONNECT_NAME "=\"80:443\" "
	ENV_SCOPED_NAME "=\"a:s\" "
	"%1$s bash -i\n"
	"\n"
	"This sandboxer can use Landlock features up to ABI version "
	STR(LANDLOCK_ABI_LAST) ".\n";

/* clang-format on */

int verbose_exec(const char *cmd_path, char *const *cmd_argv,
		 char *const *envp);
int interactive_sandboxer(int supervisor_fd, int child_stdin, int child_stdout,
			  int child_stderr, pid_t child_pid);

int main(const int argc, char *const argv[], char *const *const envp)
{
	const char *cmd_path;
	char *const *cmd_argv;
	int ruleset_fd = -1, supervisor_fd = -1, abi;
	char *env_port_name;
	__u64 access_fs_ro = ACCESS_FS_ROUGHLY_READ,
	      access_fs_rw = ACCESS_FS_ROUGHLY_READ | ACCESS_FS_ROUGHLY_WRITE;
	bool supervise = false;
	__u32 flags;
	char *env_supervise;

	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = access_fs_rw,
		.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP |
				      LANDLOCK_ACCESS_NET_CONNECT_TCP,
		.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET |
			  LANDLOCK_SCOPE_SIGNAL,
		.supervisor_fd = 0,
		.pad = 0,
	};

	if (argc < 2) {
		fprintf(stderr, help, argv[0]);
		return 1;
	}

	env_supervise = getenv(ENV_SUPERVISE);
	if (env_supervise && strcmp(env_supervise, "1") == 0) {
		supervise = true;
	}

	abi = landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0) {
		const int err = errno;

		perror("Failed to check Landlock compatibility");
		switch (err) {
		case ENOSYS:
			fprintf(stderr,
				"Hint: Landlock is not supported by the current kernel. "
				"To support it, build the kernel with "
				"CONFIG_SECURITY_LANDLOCK=y and prepend "
				"\"landlock,\" to the content of CONFIG_LSM.\n");
			break;
		case EOPNOTSUPP:
			fprintf(stderr,
				"Hint: Landlock is currently disabled. "
				"It can be enabled in the kernel configuration by "
				"prepending \"landlock,\" to the content of CONFIG_LSM, "
				"or at boot time by setting the same content to the "
				"\"lsm\" kernel parameter.\n");
			break;
		}
		return 1;
	}

	/* Best-effort security. */
	switch (abi) {
	case 1:
		/*
		 * Removes LANDLOCK_ACCESS_FS_REFER for ABI < 2
		 *
		 * Note: The "refer" operations (file renaming and linking
		 * across different directories) are always forbidden when using
		 * Landlock with ABI 1.
		 *
		 * If only ABI 1 is available, this sandboxer knowingly forbids
		 * refer operations.
		 *
		 * If a program *needs* to do refer operations after enabling
		 * Landlock, it can not use Landlock at ABI level 1.  To be
		 * compatible with different kernel versions, such programs
		 * should then fall back to not restrict themselves at all if
		 * the running kernel only supports ABI 1.
		 */
		ruleset_attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_REFER;
		__attribute__((fallthrough));
	case 2:
		/* Removes LANDLOCK_ACCESS_FS_TRUNCATE for ABI < 3 */
		ruleset_attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_TRUNCATE;
		__attribute__((fallthrough));
	case 3:
		/* Removes network support for ABI < 4 */
		ruleset_attr.handled_access_net &=
			~(LANDLOCK_ACCESS_NET_BIND_TCP |
			  LANDLOCK_ACCESS_NET_CONNECT_TCP);
		__attribute__((fallthrough));
	case 4:
		/* Removes LANDLOCK_ACCESS_FS_IOCTL_DEV for ABI < 5 */
		ruleset_attr.handled_access_fs &= ~LANDLOCK_ACCESS_FS_IOCTL_DEV;

		__attribute__((fallthrough));
	case 5:
		/* Removes LANDLOCK_SCOPE_* for ABI < 6 */
		ruleset_attr.scoped &= ~(LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET |
					 LANDLOCK_SCOPE_SIGNAL);
		__attribute__((fallthrough));
	case 6:
		/* Removes supervisor mode for ABI < 7 */
		supervise = false;
		fprintf(stderr,
			"Hint: You should update the running kernel "
			"to leverage Landlock features "
			"provided by ABI version %d (instead of %d).\n",
			LANDLOCK_ABI_LAST, abi);
		__attribute__((fallthrough));
	case LANDLOCK_ABI_LAST:
		break;
	default:
		fprintf(stderr,
			"Hint: You should update this sandboxer "
			"to leverage Landlock features "
			"provided by ABI version %d (instead of %d).\n",
			abi, LANDLOCK_ABI_LAST);
	}
	access_fs_ro &= ruleset_attr.handled_access_fs;
	access_fs_rw &= ruleset_attr.handled_access_fs;

	/* Removes bind access attribute if not supported by a user. */
	env_port_name = getenv(ENV_TCP_BIND_NAME);
	if (!env_port_name) {
		ruleset_attr.handled_access_net &=
			~LANDLOCK_ACCESS_NET_BIND_TCP;
	}
	/* Removes connect access attribute if not supported by a user. */
	env_port_name = getenv(ENV_TCP_CONNECT_NAME);
	if (!env_port_name) {
		ruleset_attr.handled_access_net &=
			~LANDLOCK_ACCESS_NET_CONNECT_TCP;
	}

	if (check_ruleset_scope(ENV_SCOPED_NAME, &ruleset_attr))
		return 1;

	flags = 0;
	if (supervise)
		flags |= LANDLOCK_CREATE_RULESET_SUPERVISE;

	ruleset_fd = landlock_create_ruleset(&ruleset_attr,
					     sizeof(ruleset_attr), flags);
	if (ruleset_fd < 0) {
		perror("Failed to create a ruleset");
		return 1;
	}
	if (supervise) {
		supervisor_fd = ruleset_attr.supervisor_fd;
		if (supervisor_fd < 0) {
			fprintf(stderr, "supervisor_fd is invalid");
			return 1;
		}
		if (supervisor_fd == 0) {
			fprintf(stderr, "supervisor_fd not set by kernel");
			return 1;
		}
	} else if (ruleset_attr.supervisor_fd != 0) {
		fprintf(stderr,
			"supervisor_fd should not be set by kernel, but it is not 0");
		return 1;
	}

	if (populate_ruleset_fs(ENV_FS_RO_NAME, ruleset_fd, access_fs_ro)) {
		goto err_close_ruleset;
	}
	if (populate_ruleset_fs(ENV_FS_RW_NAME, ruleset_fd, access_fs_rw)) {
		goto err_close_ruleset;
	}

	if (populate_ruleset_net(ENV_TCP_BIND_NAME, ruleset_fd,
				 LANDLOCK_ACCESS_NET_BIND_TCP)) {
		goto err_close_ruleset;
	}
	if (populate_ruleset_net(ENV_TCP_CONNECT_NAME, ruleset_fd,
				 LANDLOCK_ACCESS_NET_CONNECT_TCP)) {
		goto err_close_ruleset;
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		perror("Failed to restrict privileges");
		goto err_close_ruleset;
	}

	cmd_path = argv[1];
	cmd_argv = argv + 1;

	if (!supervise) {
		if (landlock_restrict_self(ruleset_fd, 0)) {
			perror("Failed to enforce ruleset");
			goto err_close_ruleset;
		}
		close(ruleset_fd);
		verbose_exec(cmd_path, cmd_argv, envp);
	} else {
		pid_t child;
		int child_stdin_pipe[2], child_stdout_pipe[2],
			child_stderr_pipe[2];
		// read from [0], write to [1]
		if (pipe(child_stdin_pipe) || pipe(child_stdout_pipe) ||
		    pipe(child_stderr_pipe)) {
			perror("Failed to create pipes");
			goto err_close_ruleset;
		}
		child = fork();
		if (child < 0) {
			perror("Failed to fork");
			goto err_close_ruleset;
		}
		if (child == 0) {
			close(supervisor_fd);

			if (landlock_restrict_self(ruleset_fd, 0)) {
				perror("Failed to enforce ruleset");
				goto err_close_ruleset;
			}

			close(child_stdin_pipe[1]);
			close(child_stdout_pipe[0]);
			close(child_stderr_pipe[0]);
			if (dup2(child_stdin_pipe[0], STDIN_FILENO) < 0 ||
			    dup2(child_stdout_pipe[1], STDOUT_FILENO) < 0 ||
			    dup2(child_stderr_pipe[1], STDERR_FILENO) < 0) {
				perror("Failed to redirect child I/O");
				exit(1);
			}
			close(child_stdin_pipe[0]);
			close(child_stdout_pipe[1]);
			close(child_stderr_pipe[1]);

			close(ruleset_fd);
			verbose_exec(cmd_path, cmd_argv, envp);
		} else {
			close(ruleset_fd);
			close(child_stdin_pipe[0]);
			close(child_stdout_pipe[1]);
			close(child_stderr_pipe[1]);
			return interactive_sandboxer(supervisor_fd,
						     child_stdin_pipe[1],
						     child_stdout_pipe[0],
						     child_stderr_pipe[0],
						     child);
		}
	}

err_close_ruleset:
	close(ruleset_fd);
	return 1;
}

int verbose_exec(const char *cmd_path, char *const *cmd_argv, char *const *envp)
{
	fprintf(stderr, "Executing the sandboxed command...\n");
	execvpe(cmd_path, cmd_argv, envp);
	int err = errno;
	fprintf(stderr, "Failed to execute \"%s\": %s\n", cmd_path,
		strerror(err));
	fprintf(stderr, "Hint: access to the binary, the interpreter or "
			"shared libraries may be denied.\n");
	return err;
}

enum SandboxAccessType {
	ACCESS_READ,
	ACCESS_READWRITE,
	ACCESS_CREATE,
	ACCESS_REMOVE,
};

struct context {
	int supervisor_fd;
	char **allowed_paths;
	size_t num_allowed_paths;
};

static int f_set_noblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		perror("Failed to get flags");
		return -1;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("Failed to set flags");
		return -1;
	}
	return 0;
}

static int write_all(int fd, const char *buf, size_t count)
{
	while (count > 0) {
		ssize_t written = write(fd, buf, count);
		if (written < 0) {
			return written;
		}
		count -= written;
		buf += written;
	}
	return 0;
}

static int readlink_fd_s(int fd, char *buf, size_t buf_len)
{
	if (buf_len == 0) {
		errno = EINVAL;
		return -1;
	}
	char procfd[100];
	snprintf(procfd, sizeof(procfd), "/proc/self/fd/%d", fd);
	ssize_t len = readlink(procfd, buf, buf_len - 1);
	if (len < 0) {
		return -1;
	}
	buf[len] = '\0';
	return len;
}

static bool show_sandbox_prompt_fs(enum SandboxAccessType access,
				   const char *file1, const char *file2,
				   int pid, const char *comm, const char *exe,
				   struct context *context)
{
	const char *access_kv;
	switch (access) {
	case ACCESS_READ:
		access_kv = "read";
		break;
	case ACCESS_READWRITE:
		access_kv = "read/write";
		break;
	case ACCESS_CREATE:
		access_kv = "create";
		break;
	case ACCESS_REMOVE:
		access_kv = "remove";
		break;
	default:
		abort();
		return false;
	}
	if (isatty(STDIN_FILENO)) {
		tcflush(STDIN_FILENO, TCIOFLUSH);
	}
	fprintf(stderr,
		"------------- Sandboxer access request -------------\n");
	fprintf(stderr, "Process %s[%d] (%s) wants to %s\n  %s\n", comm, pid,
		exe, access_kv, file1);
	if (file2) {
		fprintf(stderr, "  %s\n", file2);
	}
	bool allow = false;
	while (true) {
		char answer[10];
		fprintf(stderr, "(y)es/(a)lways/(n)o > ");
		fflush(stderr);
		int rc = read(STDIN_FILENO, answer, sizeof(answer));
		if (rc < 0) {
			perror("Failed to read answer");
			break;
		}
		if (rc == 0) {
			break;
		}
		answer[rc] = '\0';
		if (strcmp(answer, "y\n") == 0) {
			allow = true;
			break;
		} else if (strcmp(answer, "a\n") == 0) {
			allow = true;
			/* +2 in case file2 is also set */
			context->allowed_paths =
				realloc(context->allowed_paths,
					(context->num_allowed_paths + 2) *
						sizeof(char *));
			if (!context->allowed_paths) {
				abort();
			}
			char *dup_str = strdup(file1);
			if (!dup_str) {
				abort();
			}
			context->allowed_paths[context->num_allowed_paths] =
				dup_str;
			context->num_allowed_paths++;

			if (file2) {
				dup_str = strdup(file2);
				if (!dup_str) {
					abort();
				}
				context->allowed_paths
					[context->num_allowed_paths] = dup_str;
				context->num_allowed_paths++;
			}
			break;
		} else if (strcmp(answer, "n\n") == 0) {
			allow = false;
			break;
		} else {
			fprintf(stderr,
				"Please answer \"y\", \"a\", or \"n\"\n");
		}
	}
	fprintf(stderr,
		"----------------------------------------------------\n");
	return allow;
}

static bool show_sandbox_prompt_network(__u16 port, struct context *context)
{
	/* TODO: unimplemented in kernel */
	return true;
}

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

static bool path_join(char *dest_buf, size_t dest_buf_len, const char *last)
{
	if (dest_buf_len <= 1) {
		return false;
	}
	size_t last_len = strlen(last);
	size_t dest_len = strnlen(dest_buf, dest_buf_len);
	if (dest_len == 1 && dest_buf[0] == '/') {
		dest_buf[0] = '\0';
		dest_len = 0;
	}
	size_t dest_space = dest_buf_len - dest_len;
	if (dest_space <= 1) {
		return false;
	}
	if (dest_space == 2) {
		dest_buf[dest_len] = '/';
		dest_buf[dest_len + 1] = '\0';
		return false;
	}
	size_t copy_count = min(dest_space - 2, last_len);
	dest_buf[dest_len] = '/';
	memcpy(dest_buf + dest_len + 1, last, copy_count);
	dest_buf[dest_len + 1 + copy_count] = '\0';
	return copy_count == last_len;
}

static int process_event(struct landlock_supervise_event *evt,
			 struct context *context)
{
	char *target_path_1 = NULL;
	char *target_path_2 = NULL;
	char *comm = NULL;
	char *exe = NULL;
	int pid;
	int fd = -1;
	ssize_t len;
	enum SandboxAccessType access = -1;
	char proc_exe[100], proc_comm[100];
	struct landlock_supervise_response response;
	bool allow = false;
	int ret = 0;
	int supervisor_fd = context->supervisor_fd;

	memset(&response, 0, sizeof(response));

	if (((uintptr_t)evt) % __alignof__(struct landlock_supervise_event) !=
	    0) {
		/*
		 * Check that the kernel hasn't messed up given we're
		 * reading an array of varable length struct
		 */
		fprintf(stderr, "evt = %p is badly aligned\n", evt);
		abort();
	}

	switch (evt->hdr.type) {
	case LANDLOCK_SUPERVISE_EVENT_TYPE_FS_ACCESS:
		if (evt->fd1 != -1) {
			target_path_1 = malloc(PATH_MAX);
			if (!target_path_1) {
				abort();
			}
			if (readlink_fd_s(evt->fd1, target_path_1, PATH_MAX) <
			    -1) {
				close(evt->fd1);
				perror("Failed to readlink");
				ret = -1;
				goto ret;
			}
			close(evt->fd1);
		} else {
			fprintf(stderr, "fd1 is -1 which should not happen.");
			abort();
		}
		if (evt->fd2 != -1) {
			target_path_2 = malloc(PATH_MAX);
			if (!target_path_2) {
				abort();
			}
			if (readlink_fd_s(evt->fd2, target_path_2, PATH_MAX) <
			    -1) {
				perror("Failed to readlink");
				close(evt->fd2);
				ret = -1;
				goto ret;
			}
			close(evt->fd2);
		}
		if (evt->destname[0] != 0) {
			if (evt->fd2 != -1) {
				path_join(target_path_2, PATH_MAX,
					  evt->destname);
			} else {
				path_join(target_path_1, PATH_MAX,
					  evt->destname);
			}
		}
		if (evt->access_request & ACCESS_FS_ROUGHLY_CREATE) {
			access = ACCESS_CREATE;
		} else if (evt->access_request & ACCESS_FS_ROUGHLY_REMOVE) {
			access = ACCESS_REMOVE;
		} else if (evt->access_request & ACCESS_FS_ROUGHLY_WRITE) {
			access = ACCESS_READWRITE;
		} else {
			access = ACCESS_READ;
		}

		if (strcmp(target_path_1, "/dev/tty") == 0) {
			/*
			 * Deny TTY access to bash, as it messes with the
			 * supervisor input, causing the supervisor to
			 * receive SIGTTIN
			 */
			goto response;
		}

		for (size_t i = 0; i < context->num_allowed_paths; i++) {
			if (strcmp(target_path_1, context->allowed_paths[i]) ==
			    0) {
				allow = true;
				break;
			}
		}
		break;
	case LANDLOCK_SUPERVISE_EVENT_TYPE_NET_ACCESS:
		/* No pre-processing needed */
		break;
	default:
		fprintf(stderr, "Unknown event type: %d\n", evt->hdr.type);
		ret = -1;
		break;
	}

	pid = evt->accessor;
	snprintf(proc_exe, sizeof(proc_exe), "/proc/%d/exe", pid);
	exe = malloc(PATH_MAX);
	if (!exe) {
		abort();
	}
	len = readlink(proc_exe, exe, PATH_MAX - 1);
	if (len < 0) {
		perror("Failed to readlink proc exe");
		return -1;
	}
	exe[len] = '\0';
	snprintf(proc_comm, sizeof(proc_comm), "/proc/%d/comm", pid);
	comm = malloc(PATH_MAX);
	if (!comm) {
		abort();
	}
	fd = open(proc_comm, O_RDONLY);
	if (fd < 0) {
		snprintf(comm, PATH_MAX, "???");
	} else {
		len = read(fd, comm, PATH_MAX - 1);
		if (len < 0) {
			snprintf(comm, PATH_MAX, "???");
		} else {
			comm[len] = '\0';
			if (len > 0 && comm[len - 1] == '\n') {
				comm[len - 1] = '\0';
			}
		}
		close(fd);
	}

	switch (evt->hdr.type) {
	case LANDLOCK_SUPERVISE_EVENT_TYPE_FS_ACCESS:
		if (!allow) {
			allow = show_sandbox_prompt_fs(access, target_path_1,
						       target_path_2, pid, comm,
						       exe, context);
		}
		break;
	case LANDLOCK_SUPERVISE_EVENT_TYPE_NET_ACCESS:
		allow = show_sandbox_prompt_network(evt->port, context);
		break;
	}

response:
	/* Prepare and send response to the kernel */
	response.length = sizeof(response);
	response.decision = allow ? LANDLOCK_SUPERVISE_DECISION_ALLOW :
				    LANDLOCK_SUPERVISE_DECISION_DENY;
	response.cookie = evt->hdr.cookie;

	if (write(supervisor_fd, &response, sizeof(response)) !=
	    sizeof(response)) {
		perror("Failed to write supervisor response");
		ret = -1;
	}

ret:
	free(target_path_1);
	free(target_path_2);
	free(comm);
	free(exe);
	return ret;
}

static int process_events(void *data, size_t data_len, struct context *context)
{
	while (data_len > 0) {
		struct landlock_supervise_event *evt;
		int rc;
		if (data_len < sizeof(evt->hdr)) {
			fprintf(stderr,
				"Too few bytes for a event header - got %zu left, need %zu.",
				data_len, sizeof(evt->hdr));
			return -EINVAL;
		}
		evt = data;
		if (evt->hdr.length > data_len) {
			fprintf(stderr,
				"Length from event header is greater than remaining data.");
			return -EINVAL;
		}
		rc = process_event(evt, context);
		if (rc < 0) {
			return rc;
		}
		data_len -= evt->hdr.length;
		data += evt->hdr.length;
	}
	return 0;
}

int interactive_sandboxer(int supervisor_fd, int child_stdin, int child_stdout,
			  int child_stderr, pid_t child_pid)
{
	char *write_buf = NULL;
	size_t write_buf_len = 0;

	size_t io_buf_len = 4096;
	char *io_buf = malloc(io_buf_len);
	if (!io_buf) {
		fprintf(stderr, "Failed to allocate I/O buffer");
		return -1;
	}

	int status = 0;

	struct pollfd pfds[5] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = child_stdout, .events = POLLIN },
		{ .fd = child_stderr, .events = POLLIN },
		{ .fd = supervisor_fd, .events = POLLIN },
		{ .fd = child_stdin, .events = POLLOUT },
	};
	const int pfd_idx_stdin = 0;
	const int pfd_idx_child_stdout = 1;
	const int pfd_idx_child_stderr = 2;
	const int pfd_idx_supervisor = 3;
	const int pfd_idx_child_stdin = 4;
	const int poll_len = 5;

	struct context context = {
		.supervisor_fd = supervisor_fd,
		.allowed_paths = NULL,
		.num_allowed_paths = 0,
	};

	bool child_stdin_closed = false;

	/*
	 * Don't deadlock by us trying to write to child, and child
	 * waiting to write to us.
	 */
	f_set_noblock(child_stdin);

	/* Don't get killed by SIGPIPE when child closes stdout/err */
	signal(SIGPIPE, SIG_IGN);

	while (1) {
		if (write_buf_len > 0 && !child_stdin_closed) {
			pfds[pfd_idx_child_stdin].fd = child_stdin;
		} else {
			pfds[pfd_idx_child_stdin].fd = -1;
		}

		for (int i = 0; i < poll_len; i++) {
			pfds[i].revents = 0;
		}

		if (ppoll(pfds, poll_len, NULL, NULL) < 0) {
			if (errno != EINTR) {
				perror("ppoll");
				goto err_kill_child;
			}
		}

		if (pfds[0].revents & POLLIN) {
			/*
			 * Our stdin -> temp buffer for child's stdin.
			 * Need to do this before handling any supervisor
			 * events so that inputs intended for the child is
			 * not interperted as user decision.
			 */
			const int read_len = 4096;
			write_buf =
				realloc(write_buf, write_buf_len + read_len);
			if (!write_buf) {
				fprintf(stderr,
					"Failed to realloc write buffer\n");
				goto err_kill_child;
			}
			ssize_t count = read(STDIN_FILENO,
					     write_buf + write_buf_len,
					     read_len);
			if (count > 0) {
				write_buf_len += count;
			} else if (count == 0) {
				/* Our stdin is closed. Don't read from it anymore. */
				pfds[pfd_idx_stdin].fd = -1;
			} else {
				perror("Failed to read from stdin");
				goto err_kill_child;
			}
		}

		if (write_buf_len > 0) {
			/* Attempt to write any outstanding stdin to child */
			ssize_t written =
				write(child_stdin, write_buf, write_buf_len);
			if (written > 0) {
				if (written > write_buf_len) {
					abort();
				} else if (written == write_buf_len) {
					write_buf_len = 0;
				} else {
					memmove(write_buf, write_buf + written,
						write_buf_len - written);
					write_buf_len -= written;
				}
			} else {
				if (errno == EPIPE) {
					close(child_stdin);
					child_stdin_closed = true;
					pfds[pfd_idx_child_stdin].fd = -1;
					write_buf_len = 0;
				} else if (errno != EAGAIN) {
					perror("Failed to write to child stdin");
					goto err_kill_child;
				}
			}
		}

		if (pfds[pfd_idx_stdin].fd == -1 && write_buf_len == 0) {
			/* We can safely close child's stdin now */
			close(child_stdin);
			child_stdin_closed = true;
			pfds[pfd_idx_child_stdin].fd = -1;
		}

		if (pfds[pfd_idx_child_stdout].revents & POLLIN) {
			/* Child stdout -> our stdout */
			ssize_t count = read(child_stdout, io_buf, io_buf_len);
			if (count > 0) {
				if (write_all(STDOUT_FILENO, io_buf, count) <
				    0) {
					perror("Failed to write to stdout");
					goto err_kill_child;
				}
			} else if (count == 0 ||
				   (count < 0 && errno == EPIPE)) {
				close(child_stdout);
				pfds[pfd_idx_child_stdout].fd = -1;
			} else if (count < 0 && errno != EAGAIN) {
				perror("Failed to read from child stdout");
				goto err_kill_child;
			}
		}

		if (pfds[2].revents & POLLIN) {
			/* Child stderr -> our stderr */
			ssize_t count = read(child_stderr, io_buf, io_buf_len);
			if (count > 0) {
				if (write_all(STDERR_FILENO, io_buf, count) <
				    0) {
					perror("Failed to write to stderr");
					goto err_kill_child;
				}
			} else if (count == 0 ||
				   (count < 0 && errno == EPIPE)) {
				close(child_stderr);
				pfds[pfd_idx_child_stderr].fd = -1;
			} else if (count < 0 && errno != EAGAIN) {
				perror("Failed to read from child stderr");
				goto err_kill_child;
			}
		}

		if (waitpid(child_pid, &status, WNOHANG) == child_pid) {
			/*
			 * Write out any remaining child stdout/stderr.
			 * If child died, read would just return EOF.
			 */
			while (1) {
				ssize_t count =
					read(child_stdout, io_buf, io_buf_len);
				if (count > 0)
					write_all(STDOUT_FILENO, io_buf, count);
				else
					break;
			}
			while (1) {
				ssize_t count =
					read(child_stderr, io_buf, io_buf_len);
				if (count > 0)
					write_all(STDERR_FILENO, io_buf, count);
				else
					break;
			}
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		}

		if (pfds[pfd_idx_supervisor].revents) {
retry:
			ssize_t count = read(supervisor_fd, io_buf, io_buf_len);
			if (count > 0) {
				process_events(io_buf, count, &context);
			} else if (count == 0) {
				fprintf(stderr,
					"Unexpected EOF on supervisor fd\n");
				goto err_kill_child;
			} else if (count < 0 && errno != EAGAIN) {
				if (errno == EINVAL) {
					io_buf_len *= 2;
					io_buf = realloc(io_buf, io_buf_len);
					if (!io_buf) {
						fprintf(stderr,
							"Failed to realloc I/O buffer\n");
						goto err_kill_child;
					}
					fprintf(stderr,
						"Got EINVAL - possibly event too big. Realloced I/O buffer to %zu\n",
						io_buf_len);
					goto retry;
				}
				perror("Failed to read from supervisor");
				goto err_kill_child;
			}
		}
	}

err_kill_child:
	close(supervisor_fd);
	kill(child_pid, SIGTERM);
	return -1;
}
