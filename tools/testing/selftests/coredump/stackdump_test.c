// SPDX-License-Identifier: GPL-2.0

#include <fcntl.h>
#include <libgen.h>
#include <linux/limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../kselftest_harness.h"
#include "../pidfd/pidfd.h"

#define STACKDUMP_FILE "stack_values"
#define STACKDUMP_SCRIPT "stackdump"
#define NUM_THREAD_SPAWN 128

static void *do_nothing(void *)
{
	while (1)
		pause();
}

static void crashing_child(void)
{
	pthread_t thread;
	int i;

	for (i = 0; i < NUM_THREAD_SPAWN; ++i)
		pthread_create(&thread, NULL, do_nothing, NULL);

	/* crash on purpose */
	i = *(int *)NULL;
}

FIXTURE(coredump)
{
	char original_core_pattern[256];
	pid_t pid_socat;
};

FIXTURE_SETUP(coredump)
{
	char buf[PATH_MAX];
	FILE *file;
	char *dir;
	int ret;

	self->pid_socat = -ESRCH;
	file = fopen("/proc/sys/kernel/core_pattern", "r");
	ASSERT_NE(NULL, file);

	ret = fread(self->original_core_pattern, 1, sizeof(self->original_core_pattern), file);
	ASSERT_TRUE(ret || feof(file));
	ASSERT_LT(ret, sizeof(self->original_core_pattern));

	self->original_core_pattern[ret] = '\0';

	ret = fclose(file);
	ASSERT_EQ(0, ret);
}

FIXTURE_TEARDOWN(coredump)
{
	const char *reason;
	FILE *file;
	int ret, status;

	unlink(STACKDUMP_FILE);

	if (self->pid_socat > 0) {
		kill(self->pid_socat, SIGTERM);
		waitpid(self->pid_socat, &status, 0);
	}

	file = fopen("/proc/sys/kernel/core_pattern", "w");
	if (!file) {
		reason = "Unable to open core_pattern";
		goto fail;
	}

	ret = fprintf(file, "%s", self->original_core_pattern);
	if (ret < 0) {
		reason = "Unable to write to core_pattern";
		goto fail;
	}

	ret = fclose(file);
	if (ret) {
		reason = "Unable to close core_pattern";
		goto fail;
	}

	return;
fail:
	/* This should never happen */
	fprintf(stderr, "Failed to cleanup stackdump test: %s\n", reason);
}

TEST_F_TIMEOUT(coredump, stackdump, 120)
{
	struct sigaction action = {};
	unsigned long long stack;
	char *test_dir, *line;
	size_t line_length;
	char buf[PATH_MAX];
	int ret, i, status;
	FILE *file;
	pid_t pid;

	/*
	 * Step 1: Setup core_pattern so that the stackdump script is executed when the child
	 * process crashes
	 */
	ret = readlink("/proc/self/exe", buf, sizeof(buf));
	ASSERT_NE(-1, ret);
	ASSERT_LT(ret, sizeof(buf));
	buf[ret] = '\0';

	test_dir = dirname(buf);

	file = fopen("/proc/sys/kernel/core_pattern", "w");
	ASSERT_NE(NULL, file);

	ret = fprintf(file, "|%1$s/%2$s %%P %1$s/%3$s", test_dir, STACKDUMP_SCRIPT, STACKDUMP_FILE);
	ASSERT_LT(0, ret);

	ret = fclose(file);
	ASSERT_EQ(0, ret);

	/* Step 2: Create a process who spawns some threads then crashes */
	pid = fork();
	ASSERT_TRUE(pid >= 0);
	if (pid == 0)
		crashing_child();

	/*
	 * Step 3: Wait for the stackdump script to write the stack pointers to the stackdump file
	 */
	waitpid(pid, &status, 0);
	ASSERT_TRUE(WIFSIGNALED(status));
	ASSERT_TRUE(WCOREDUMP(status));

	for (i = 0; i < 10; ++i) {
		file = fopen(STACKDUMP_FILE, "r");
		if (file)
			break;
		sleep(1);
	}
	ASSERT_NE(file, NULL);

	/* Step 4: Make sure all stack pointer values are non-zero */
	line = NULL;
	for (i = 0; -1 != getline(&line, &line_length, file); ++i) {
		stack = strtoull(line, NULL, 10);
		ASSERT_NE(stack, 0);
	}
	free(line);

	ASSERT_EQ(i, 1 + NUM_THREAD_SPAWN);

	fclose(file);
}

TEST_F(coredump, socket)
{
	int fd, pidfd, ret, status;
	FILE *file;
	pid_t pid, pid_socat;
	struct stat st;
	char core_file[PATH_MAX];
	struct pidfd_info info = {};

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL), 0);
	ASSERT_EQ(mount(NULL, "/tmp", "tmpfs", 0, NULL), 0);

	pid_socat = fork();
	ASSERT_GE(pid_socat, 0);
	if (pid_socat == 0) {
		execlp("socat", "socat",
		       "abstract-listen:linuxafsk/coredump.socket,fork",
		       "FILE:/tmp/coredump_file,create,append,trunc",
		       (char *)NULL);
		_exit(EXIT_FAILURE);
	}
	self->pid_socat = pid_socat;

	file = fopen("/proc/sys/kernel/core_pattern", "w");
	ASSERT_NE(NULL, file);

	ret = fprintf(file, "@linuxafsk/coredump.socket");
	ASSERT_EQ(ret, strlen("@linuxafsk/coredump.socket"));
	ASSERT_EQ(fclose(file), 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
		crashing_child();

	pidfd = sys_pidfd_open(pid, 0);
	ASSERT_GE(pidfd, 0);

	waitpid(pid, &status, 0);
	ASSERT_TRUE(WIFSIGNALED(status));
	ASSERT_TRUE(WCOREDUMP(status));

	info.mask = PIDFD_INFO_EXIT | PIDFD_INFO_COREDUMP;
	ASSERT_EQ(ioctl(pidfd, PIDFD_GET_INFO, &info), 0);
	ASSERT_GT((info.mask & PIDFD_INFO_COREDUMP), 0);
	ASSERT_GT((info.coredump_mask & PIDFD_COREDUMPED), 0);

	ASSERT_EQ(kill(pid_socat, SIGTERM), 0);
	waitpid(pid_socat, &status, 0);
	self->pid_socat = -ESRCH;
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 143);

	/* We should somehow validate the produced core file. */
	ASSERT_EQ(stat("/tmp/coredump_file", &st), 0);
	ASSERT_GT(st.st_size, 0);
}

TEST_HARNESS_MAIN
