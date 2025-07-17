// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Userspace testing API.
 *
 * Copyright (C) 2025, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 */

#include <linux/binfmts.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/pid.h>
#include <linux/pipe_fs_i.h>
#include <linux/sched/task.h>
#include <linux/types.h>

#include <kunit/test.h>
#include <kunit/uapi.h>

#define KSFT_PASS	0
#define KSFT_FAIL	1
#define KSFT_XFAIL	2
#define KSFT_XPASS	3
#define KSFT_SKIP	4

KUNIT_UAPI_EMBED_BLOB(kunit_uapi_preinit, "uapi-preinit");

static struct vfsmount *kunit_uapi_mount_ramfs(void)
{
	struct file_system_type *type;
	struct vfsmount *mnt;

	type = get_fs_type("ramfs");
	if (!type)
		return ERR_PTR(-ENODEV);

	/* FIXME
	 * The mount setup is supposed to look like this:
	 * kunit_uapi_mount_ramfs() sets up a private mount,
	 * with nothing visible except the new tmpfs.
	 * Then each executable execution gets a new namespace on top of that
	 * on which it can mount whatever it needs.
	 * However I didn't manage to set this up, so keep everything simple
	 * for now and let somebody familiar with the VFS figure this out.
	 */

	mnt = kern_mount(type);
	put_filesystem(type);

	return mnt;
}

static int kunit_uapi_write_file(struct vfsmount *mnt, const char *name, mode_t mode,
				 const u8 *data, size_t size)
{
	struct file *file;
	ssize_t written;

	file = file_open_root_mnt(mnt, name, O_CREAT | O_WRONLY, mode);
	if (IS_ERR(file))
		return PTR_ERR(file);

	written = kernel_write(file, data, size, NULL);
	filp_close(file, NULL);
	if (written != size) {
		if (written >= 0)
			return -ENOMEM;
		return written;
	}

	/* Flush delayed fput so exec can open the file read-only */
	flush_delayed_fput();

	return 0;
}

static int kunit_uapi_write_executable(struct vfsmount *mnt,
				       const struct kunit_uapi_blob *executable)
{
	return kunit_uapi_write_file(mnt, kbasename(executable->path), 0755,
				     executable->data, executable->end - executable->data);
}

struct kunit_uapi_user_mode_thread_ctx {
	const char *executable;

	/* Signals mnt, out, pwd and tgid */
	struct completion setup_done;
	struct vfsmount *mnt;
	struct file *out;
	struct path pwd;
	pid_t tgid;

	/* Valid after wait(tgid) */
	int exec_err;
};

static int kunit_uapi_user_mode_thread_init(void *data)
{
	struct kunit_uapi_user_mode_thread_ctx *ctx = data;
	const char *const argv[] = {
		ctx->executable,
		NULL
	};
	struct file *out[2];
	int err;

	err = create_pipe_files(out, 0);
	if (err)
		return err;

	/* stdin, use the *write* end to the pipe to have an unreadable input */
	err = replace_fd(0, out[1], 0);
	if (err < 0) {
		fput(out[0]);
		fput(out[1]);
		return err;
	}

	/* stdout */
	err = replace_fd(1, out[1], 0);
	if (err < 0) {
		replace_fd(0, NULL, 0);
		fput(out[0]);
		fput(out[1]);
		return err;
	}

	/* stderr */
	err = replace_fd(2, out[1], 0);
	if (err < 0) {
		replace_fd(0, NULL, 0);
		replace_fd(1, NULL, 0);
		fput(out[0]);
		fput(out[1]);
		return err;
	}

	fput(out[1]);

	ctx->out = out[0];
	ctx->tgid = current->tgid;

	set_fs_pwd(current->fs, &ctx->pwd);
	kernel_sigaction(SIGKILL, SIG_DFL);
	kernel_sigaction(SIGABRT, SIG_DFL);

	complete(&ctx->setup_done);
	ctx->exec_err = kernel_execve(kbasename(kunit_uapi_preinit.path), argv, NULL);
	if (!ctx->exec_err)
		return 0;
	do_exit(0);
}

static size_t kunit_uapi_printk_subtest_lines(struct kunit *test, char *buf, size_t s)
{
	const char *ptr = buf, *newline;
	size_t n;

	while (s) {
		newline = strnchr(ptr, s, '\n');
		if (!newline)
			break;

		n = newline - ptr + 1;

		kunit_log(KERN_INFO, test, KUNIT_SUBSUBTEST_INDENT "%.*s", (int)n, ptr);
		ptr += n;
		s -= n;
	}

	memmove(buf, ptr, s);

	return s;
}

static int kunit_uapi_forward_to_printk(struct kunit *test, struct file *output)
{
	/*
	 * printk() automatically adds a newline after each message.
	 * Therefore only fully accumulated lines can be forwarded.
	 * Each line needs to fit into the buffer below.
	 */
	char buf[512];
	size_t s = 0;
	ssize_t n;

	while (1) {
		n = kernel_read(output, buf + s, sizeof(buf) - s, NULL);
		if (n <= 0)
			return n;
		s = kunit_uapi_printk_subtest_lines(test, buf, s + n);
	}
}

static void kunit_uapi_kill_pid(pid_t pid)
{
	struct pid *p;

	p = find_get_pid(pid);
	kill_pid(p, SIGKILL, 1);
	put_pid(p);
}

static int kunit_uapi_run_executable_in_mount(struct kunit *test, const char *executable,
						   struct vfsmount *mnt)
{
	struct kunit_uapi_user_mode_thread_ctx ctx = {
		.setup_done	= COMPLETION_INITIALIZER_ONSTACK(ctx.setup_done),
		.executable	= executable,
		.pwd		= {
			.mnt	= mnt,
			.dentry	= mnt->mnt_root,
		},
	};
	int forward_err, wait_err, ret = 0;
	pid_t pid;

	/* If SIGCHLD is ignored do_wait won't populate the status. */
	kernel_sigaction(SIGCHLD, SIG_DFL);
	pid = user_mode_thread(kunit_uapi_user_mode_thread_init, &ctx, SIGCHLD);
	if (pid < 0) {
		kernel_sigaction(SIGCHLD, SIG_IGN);
		return pid;
	}

	wait_for_completion(&ctx.setup_done);

	forward_err = kunit_uapi_forward_to_printk(test, ctx.out);
	if (forward_err)
		kunit_uapi_kill_pid(ctx.tgid);

	wait_err = kernel_wait(ctx.tgid, &ret);

	/* Restore default kernel sig handler */
	kernel_sigaction(SIGCHLD, SIG_IGN);

	if (ctx.exec_err)
		return ctx.exec_err;
	if (forward_err)
		return forward_err;
	if (wait_err < 0)
		return wait_err;
	return ret;
}

static int kunit_uapi_run_executable(struct kunit *test,
				     const struct kunit_uapi_blob *executable)
{
	const char *exe_name = kbasename(executable->path);
	struct vfsmount *mnt;
	int err;

	mnt = kunit_uapi_mount_ramfs();
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	err = kunit_uapi_write_executable(mnt, &kunit_uapi_preinit);

	if (!err)
		err = kunit_uapi_write_executable(mnt, executable);

	if (!err)
		err = kunit_uapi_run_executable_in_mount(test, exe_name, mnt);

	kern_unmount(mnt);

	return err;
}

void kunit_uapi_run_kselftest(struct kunit *test, const struct kunit_uapi_blob *executable)
{
	u8 exit_code, exit_signal;
	int err;

	err = kunit_uapi_run_executable(test, executable);
	if (err < 0)
		KUNIT_FAIL_AND_ABORT(test, "Could not run test executable: %pe\n", ERR_PTR(err));

	exit_code = err >> 8;
	exit_signal = err & 0xff;

	if (exit_signal)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with signal: %d\n", exit_signal);
	else if (exit_code == KSFT_PASS)
		; /* Noop */
	else if (exit_code == KSFT_FAIL)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with code KSFT_FAIL\n");
	else if (exit_code == KSFT_XPASS)
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with code KSFT_XPASS\n");
	else if (exit_code == KSFT_XFAIL)
		; /* Noop */
	else if (exit_code == KSFT_SKIP)
		kunit_mark_skipped(test, "kselftest exited with code KSFT_SKIP\n");
	else
		KUNIT_FAIL_AND_ABORT(test, "kselftest exited with unknown exit code: %d\n",
				     exit_code);
}
EXPORT_SYMBOL_GPL(kunit_uapi_run_kselftest);

MODULE_DESCRIPTION("KUnit UAPI testing framework");
MODULE_AUTHOR("Thomas Weißschuh <thomas.weissschuh@linutronix.de");
MODULE_LICENSE("GPL");
