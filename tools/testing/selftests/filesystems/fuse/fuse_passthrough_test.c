// SPDX-License-Identifier: GPL-2.0
/*
 * Test that FUSE backing files are visible in /proc/self/fd
 *
 * When FUSE passthrough mode registers a backing file via
 * FUSE_DEV_IOC_BACKING_OPEN, that file should remain visible in
 * /proc/self/fd of the daemon process so that tools like lsof can
 * observe it. This test verifies that behavior.
 *
 * Requires: root (CAP_SYS_ADMIN), CONFIG_FUSE_PASSTHROUGH=y
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest.h"

#define FUSE_DEV	"/dev/fuse"
#define BUF_SIZE	131072

/*
 * FUSE_PASSTHROUGH is bit 37.  The protocol splits flags across two
 * 32-bit fields: flags holds bits 0-31, flags2 holds bits 32-63
 * shifted right by 32.
 */
#define FUSE_PASSTHROUGH_FLAGS2	((uint32_t)(FUSE_PASSTHROUGH >> 32))

/* Count /proc/self/fd symlinks that resolve to @path. */
static int count_proc_fd_refs(const char *path)
{
	DIR *dir;
	struct dirent *de;
	char link_target[PATH_MAX];
	char proc_entry[PATH_MAX];
	int count = 0;

	dir = opendir("/proc/self/fd");
	if (!dir)
		return -1;

	while ((de = readdir(dir)) != NULL) {
		ssize_t len;

		if (de->d_name[0] == '.')
			continue;

		snprintf(proc_entry, sizeof(proc_entry),
			 "/proc/self/fd/%s", de->d_name);
		len = readlink(proc_entry, link_target, sizeof(link_target) - 1);
		if (len < 0)
			continue;
		link_target[len] = '\0';

		if (strcmp(link_target, path) == 0)
			count++;
	}

	closedir(dir);
	return count;
}

/*
 * Minimal FUSE daemon: performs the INIT handshake with FUSE_PASSTHROUGH
 * enabled, registers @backing_path as a backing file, writes the
 * /proc/self/fd ref counts before and after BACKING_CLOSE into
 * @result_pipe, then drains requests until the mount is torn down.
 */
static void run_daemon(int fuse_fd, const char *backing_path, int result_pipe)
{
	char buf[BUF_SIZE];
	struct fuse_in_header *in_hdr;
	struct {
		struct fuse_out_header	hdr;
		struct fuse_init_out	init;
	} init_reply;
	struct {
		struct fuse_out_header	hdr;
	} err_reply;
	struct fuse_backing_map map = {};
	ssize_t len;
	int backing_fd;
	int backing_id;
	uint32_t close_id;
	int fd_count_open = -1;
	int fd_count_close = -1;

	len = read(fuse_fd, buf, sizeof(buf));
	if (len < 0) {
		ksft_print_msg("daemon: read INIT: %s\n", strerror(errno));
		goto out;
	}

	in_hdr = (struct fuse_in_header *)buf;
	if (in_hdr->opcode != FUSE_INIT) {
		ksft_print_msg("daemon: expected FUSE_INIT, got %u\n",
			       in_hdr->opcode);
		goto out;
	}

	memset(&init_reply, 0, sizeof(init_reply));
	init_reply.hdr.len	= sizeof(init_reply);
	init_reply.hdr.unique	= in_hdr->unique;
	init_reply.init.major		= FUSE_KERNEL_VERSION;
	init_reply.init.minor		= FUSE_KERNEL_MINOR_VERSION;
	init_reply.init.max_readahead	= 65536;
	init_reply.init.flags		= FUSE_INIT_EXT;
	init_reply.init.max_write	= 65536;
	init_reply.init.max_pages	= 256;
	init_reply.init.flags2		= FUSE_PASSTHROUGH_FLAGS2;
	init_reply.init.max_stack_depth	= 1;

	if (write(fuse_fd, &init_reply, sizeof(init_reply)) < 0) {
		ksft_print_msg("daemon: write INIT reply: %s\n", strerror(errno));
		goto out;
	}

	backing_fd = open(backing_path, O_RDWR);
	if (backing_fd < 0) {
		ksft_print_msg("daemon: open backing file: %s\n", strerror(errno));
		goto out;
	}

	map.fd = backing_fd;
	backing_id = ioctl(fuse_fd, FUSE_DEV_IOC_BACKING_OPEN, &map);
	if (backing_id < 0) {
		ksft_print_msg("daemon: FUSE_DEV_IOC_BACKING_OPEN: %s\n",
			       strerror(errno));
		close(backing_fd);
		goto out;
	}

	/*
	 * Close our own fd.  The kernel now holds the only reference via
	 * its backing_files_map.  Check whether the file is still visible
	 * in /proc/self/fd -- it should be, via the fd installed by the kernel.
	 */
	close(backing_fd);
	fd_count_open = count_proc_fd_refs(backing_path);

	close_id = (uint32_t)backing_id;
	ioctl(fuse_fd, FUSE_DEV_IOC_BACKING_CLOSE, &close_id);
	fd_count_close = count_proc_fd_refs(backing_path);

out:
	/* Signal results before draining so the parent can proceed to unmount. */
	if (write(result_pipe, &fd_count_open, sizeof(fd_count_open)) < 0 ||
	    write(result_pipe, &fd_count_close, sizeof(fd_count_close)) < 0)
		ksft_print_msg("daemon: write result pipe: %s\n", strerror(errno));

	while (1) {
		len = read(fuse_fd, buf, sizeof(buf));
		if (len <= 0)
			break;

		in_hdr = (struct fuse_in_header *)buf;
		memset(&err_reply, 0, sizeof(err_reply));
		err_reply.hdr.len    = sizeof(err_reply);
		err_reply.hdr.error  = -ENOSYS;
		err_reply.hdr.unique = in_hdr->unique;
		if (write(fuse_fd, &err_reply, sizeof(err_reply)) < 0)
			break;
	}
}

int main(void)
{
	char tmpdir[] = "/tmp/fuse_backing_test_XXXXXX";
	char mntpoint[PATH_MAX];
	char backing_path[PATH_MAX];
	char mount_opts[64];
	int fuse_fd = -1;
	int pipe_fds[2] = {-1, -1};
	int tmp_fd;
	pid_t daemon_pid;
	int fd_count_open = -1;
	int fd_count_close = -1;
	int status;

	ksft_print_header();
	ksft_set_plan(2);

	if (geteuid() != 0)
		ksft_exit_skip("requires root (CAP_SYS_ADMIN)\n");

	if (!mkdtemp(tmpdir))
		ksft_exit_fail_msg("mkdtemp: %s\n", strerror(errno));

	snprintf(mntpoint, sizeof(mntpoint), "%s/mnt", tmpdir);
	snprintf(backing_path, sizeof(backing_path), "%s/backing_file", tmpdir);

	if (mkdir(mntpoint, 0700) < 0) {
		ksft_print_msg("mkdir mntpoint: %s\n", strerror(errno));
		goto cleanup_tmpdir;
	}

	tmp_fd = open(backing_path, O_CREAT | O_RDWR, 0600);
	if (tmp_fd < 0) {
		ksft_print_msg("create backing file: %s\n", strerror(errno));
		goto cleanup_dirs;
	}
	close(tmp_fd);

	fuse_fd = open(FUSE_DEV, O_RDWR);
	if (fuse_fd < 0) {
		ksft_print_msg("open %s: %s\n", FUSE_DEV, strerror(errno));
		goto cleanup_files;
	}

	if (pipe(pipe_fds) < 0) {
		ksft_print_msg("pipe: %s\n", strerror(errno));
		goto cleanup_fuse;
	}

	daemon_pid = fork();
	if (daemon_pid < 0) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		goto cleanup_pipe;
	}

	if (daemon_pid == 0) {
		close(pipe_fds[0]);
		run_daemon(fuse_fd, backing_path, pipe_fds[1]);
		close(pipe_fds[1]);
		exit(0);
	}

	close(pipe_fds[1]);
	pipe_fds[1] = -1;

	snprintf(mount_opts, sizeof(mount_opts),
		 "fd=%d,rootmode=040000,user_id=0,group_id=0", fuse_fd);

	if (mount("fuse.test", mntpoint, "fuse", MS_NOSUID | MS_NODEV,
		  mount_opts) < 0) {
		ksft_print_msg("mount: %s\n", strerror(errno));
		kill(daemon_pid, SIGTERM);
		waitpid(daemon_pid, &status, 0);
		goto cleanup_pipe;
	}

	if (read(pipe_fds[0], &fd_count_open, sizeof(fd_count_open)) != sizeof(fd_count_open) ||
	    read(pipe_fds[0], &fd_count_close, sizeof(fd_count_close)) != sizeof(fd_count_close))
		ksft_print_msg("read result pipe: %s\n", strerror(errno));

	umount2(mntpoint, MNT_DETACH);
	waitpid(daemon_pid, &status, 0);

	if (fd_count_open == 1)
		ksft_test_result_pass(
			"backing file visible in /proc/self/fd after BACKING_OPEN\n");
	else if (fd_count_open == 0)
		ksft_test_result_fail(
			"backing file NOT visible in /proc/self/fd after BACKING_OPEN\n");
	else
		ksft_test_result_fail(
			"BACKING_OPEN: unexpected fd_count=%d\n", fd_count_open);

	if (fd_count_close == 0)
		ksft_test_result_pass(
			"backing file removed from /proc/self/fd after BACKING_CLOSE\n");
	else if (fd_count_close == 1)
		ksft_test_result_fail(
			"backing file still visible in /proc/self/fd after BACKING_CLOSE\n");
	else
		ksft_test_result_fail(
			"BACKING_CLOSE: unexpected fd_count=%d\n", fd_count_close);

cleanup_pipe:
	if (pipe_fds[0] >= 0)
		close(pipe_fds[0]);
	if (pipe_fds[1] >= 0)
		close(pipe_fds[1]);
cleanup_fuse:
	if (fuse_fd >= 0)
		close(fuse_fd);
cleanup_files:
	unlink(backing_path);
cleanup_dirs:
	rmdir(mntpoint);
cleanup_tmpdir:
	rmdir(tmpdir);

	ksft_finished();
}
