// SPDX-License-Identifier: GPL-2.0
/*
 * Selftests for SECCOMP_IOCTL_NOTIF_PIN_ARGS — atomic snapshot of
 * pointer-arg payloads for seccomp_unotify(2) supervisors.
 *
 * The motivating attack (see Documentation/userspace-api/seccomp_filter.rst):
 * an unprivileged supervisor inspects bytes that a sibling thread (or
 * CLONE_VM peer) mutates between supervisor read and kernel re-read,
 * defeating any decision the supervisor made on the bytes it saw.
 *
 * Each test sets up a USER_NOTIF filter, traps a syscall, calls
 * PIN_ARGS to atomically copy designated pointer-arg payloads into
 * kernel buffers, mutates the underlying user memory (simulating a
 * racy peer), sends NOTIF_SEND with CONTINUE | CONTINUE_PINNED, and
 * verifies the kernel used the snapshotted bytes rather than the
 * mutated ones.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>

#include "../kselftest_harness.h"

#ifndef SECCOMP_IOCTL_NOTIF_PIN_ARGS
# error "kernel UAPI lacks SECCOMP_IOCTL_NOTIF_PIN_ARGS"
#endif

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* Install a USER_NOTIF filter that traps the given syscall number and
 * allows everything else; returns the listener fd.
 */
static int install_user_notif_filter(int nr)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)ARRAY_SIZE(filter),
		.filter = filter,
	};

	return syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

/*
 * Helpers shared by the bind-on-shared-sockaddr tests below.
 * MAP_SHARED gives parent and child the same physical bytes, mirroring
 * the CLONE_VM peer in the documented attack scenario.
 */
struct bind_race {
	int listener;
	pid_t child;
	struct sockaddr_un *shared;	/* mmap'd MAP_SHARED, sockaddr_un */
	char path_a[64];		/* original path (set before fork) */
	char path_b[64];		/* path the parent mutates to before SEND */
};

/* Set up filter, mmap, fill path_a; fork the child to bind() against
 * @shared. On return, the child is trapped in the seccomp wait and the
 * supervisor (caller) is ready to NOTIF_RECV. Returns 0 on success or
 * -1 on a setup failure (with errno preserved).
 */
static int bind_race_setup(struct bind_race *r)
{
	r->listener = -1;
	r->child = -1;
	r->shared = MAP_FAILED;

	snprintf(r->path_a, sizeof(r->path_a),
		 "/tmp/seccomp-pin-%d-A", getpid());
	snprintf(r->path_b, sizeof(r->path_b),
		 "/tmp/seccomp-pin-%d-B", getpid());
	unlink(r->path_a);
	unlink(r->path_b);

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
		return -1;

	r->shared = mmap(NULL, sizeof(*r->shared), PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (r->shared == MAP_FAILED)
		return -1;
	memset(r->shared, 0, sizeof(*r->shared));
	r->shared->sun_family = AF_UNIX;
	strcpy(r->shared->sun_path, r->path_a);

	r->listener = install_user_notif_filter(__NR_bind);
	if (r->listener < 0)
		return -1;

	r->child = fork();
	if (r->child < 0)
		return -1;
	if (r->child == 0) {
		int fd = socket(AF_UNIX, SOCK_DGRAM, 0);

		if (fd < 0)
			_exit(10);
		if (bind(fd, (struct sockaddr *)r->shared,
			 sizeof(*r->shared)) < 0)
			_exit(11);
		_exit(0);
	}
	return 0;
}

static void bind_race_teardown(struct bind_race *r)
{
	if (r->child > 0)
		waitpid(r->child, NULL, WNOHANG);
	if (r->listener >= 0)
		close(r->listener);
	if (r->shared != MAP_FAILED)
		munmap(r->shared, sizeof(*r->shared));
	unlink(r->path_a);
	unlink(r->path_b);
}

/* Pin arg 1 (the sockaddr*) of the outstanding bind() notif. On success,
 * @readback (>= sizeof(sockaddr_un)) holds the snapshotted bytes.
 */
static int do_pin_sockaddr(int listener, __u64 id,
			   void *readback, size_t readback_size)
{
	struct seccomp_notif_pin_args pinreq;

	memset(&pinreq, 0, sizeof(pinreq));
	pinreq.id           = id;
	pinreq.nr_args      = 1;
	pinreq.buf_size     = readback_size;
	pinreq.buf          = (uintptr_t)readback;
	pinreq.args[0].arg_idx   = 1;
	pinreq.args[0].kind      = SECCOMP_PIN_FIXED;
	pinreq.args[0].max_bytes = sizeof(struct sockaddr_un);

	return ioctl(listener, SECCOMP_IOCTL_NOTIF_PIN_ARGS, &pinreq);
}

/*
 * Pin a sockaddr the trapped child is about to bind(), mutate the
 * underlying shared memory, send CONTINUE | CONTINUE_PINNED, and verify
 * that the kernel binds against the *pinned* path rather than the
 * mutated one.
 */
TEST(pin_args_sockaddr_bind)
{
	struct bind_race r;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[sizeof(struct sockaddr_un)];
	struct sockaddr_un *seen;
	struct stat st;
	int status;

	ASSERT_EQ(0, bind_race_setup(&r));

	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_bind);

	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(r.listener, req.id,
				     readback, sizeof(readback)));

	seen = (struct sockaddr_un *)readback;
	EXPECT_EQ(seen->sun_family, (sa_family_t)AF_UNIX);
	EXPECT_STREQ(seen->sun_path, r.path_a);

	/* Race: mutate shared memory before SEND. */
	strcpy(r.shared->sun_path, r.path_b);

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(r.child, &status, 0), r.child);
	r.child = -1;
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	/* Pinned path won. */
	EXPECT_EQ(stat(r.path_a, &st), 0);
	EXPECT_EQ(stat(r.path_b, &st), -1);
	EXPECT_EQ(errno, ENOENT);

	bind_race_teardown(&r);
}

/*
 * Negative pair of the above: pin then send CONTINUE *without* PINNED.
 * The pin must be discarded and the kernel re-read user memory, so the
 * bind should land at the mutated path (path_b) — the existing
 * SECCOMP_USER_NOTIF_FLAG_CONTINUE behavior is preserved.
 */
TEST(pin_args_continue_without_pinned)
{
	struct bind_race r;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[sizeof(struct sockaddr_un)];
	struct stat st;
	int status;

	ASSERT_EQ(0, bind_race_setup(&r));

	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_bind);

	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(r.listener, req.id,
				     readback, sizeof(readback)));

	strcpy(r.shared->sun_path, r.path_b);

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;	/* no PINNED */
	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(r.child, &status, 0), r.child);
	r.child = -1;
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	/* Pin discarded; mutated path won. */
	EXPECT_EQ(stat(r.path_a, &st), -1);
	EXPECT_EQ(errno, ENOENT);
	EXPECT_EQ(stat(r.path_b, &st), 0);

	bind_race_teardown(&r);
}

/*
 * CONTINUE_PINNED without CONTINUE must be rejected with -EINVAL by
 * NOTIF_SEND (the flag is meaningless in isolation). After the rejection
 * the supervisor can still send a normal CONTINUE to let the child run.
 */
TEST(pin_args_continue_pinned_alone)
{
	struct bind_race r;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[sizeof(struct sockaddr_un)];
	int status;

	ASSERT_EQ(0, bind_race_setup(&r));

	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(r.listener, req.id,
				     readback, sizeof(readback)));

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;	/* alone — invalid */
	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), -1);
	EXPECT_EQ(errno, EINVAL);

	/* Recover by sending a regular CONTINUE so the child can finish. */
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(r.child, &status, 0), r.child);
	r.child = -1;
	EXPECT_EQ(true, WIFEXITED(status));

	bind_race_teardown(&r);
}

/*
 * Two PIN_ARGS calls for the same notif id: the second must be rejected
 * with -EEXIST. The original snapshot stays in effect.
 */
TEST(pin_args_double_pin)
{
	struct bind_race r;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[sizeof(struct sockaddr_un)];
	char readback2[sizeof(struct sockaddr_un)];
	int status;

	ASSERT_EQ(0, bind_race_setup(&r));

	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(r.listener, req.id,
				     readback, sizeof(readback)));

	memset(readback2, 0, sizeof(readback2));
	EXPECT_EQ(do_pin_sockaddr(r.listener, req.id,
				  readback2, sizeof(readback2)), -1);
	EXPECT_EQ(errno, EEXIST);

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(r.child, &status, 0), r.child);
	r.child = -1;
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	bind_race_teardown(&r);
}

/*
 * SECCOMP_PIN_CSTRING: pin the path passed to openat(), mutate the
 * shared user-memory copy of the path between PIN_ARGS and SEND, and
 * verify that the kernel opens the *pinned* path rather than the
 * mutated one.
 *
 * Matches the motivating attack against path-based filters: supervisor
 * blesses /tmp/pin-A; sibling rewrites the path to /tmp/pin-B; the
 * kernel must still open /tmp/pin-A.
 */
TEST(pin_args_openat_cstring)
{
	char *shared_path;
	char path_a[64], path_b[64];
	struct seccomp_notif_pin_args pinreq;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[PATH_MAX];
	int listener, status, fd_a, fd_b;
	pid_t pid;
	long ret;

	snprintf(path_a, sizeof(path_a), "/tmp/seccomp-pin-cstr-%d-A", getpid());
	snprintf(path_b, sizeof(path_b), "/tmp/seccomp-pin-cstr-%d-B", getpid());

	/* Pre-create both targets so openat() succeeds either way; we
	 * verify *which* file got opened, not whether open succeeded.
	 */
	fd_a = open(path_a, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	ASSERT_GE(fd_a, 0);
	write(fd_a, "A", 1);
	close(fd_a);

	fd_b = open(path_b, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	ASSERT_GE(fd_b, 0);
	write(fd_b, "B", 1);
	close(fd_b);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

	shared_path = mmap(NULL, PATH_MAX, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, shared_path);
	memset(shared_path, 0, PATH_MAX);
	strcpy(shared_path, path_a);

	listener = install_user_notif_filter(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		char buf[2] = {0};
		int fd = openat(AT_FDCWD, shared_path, O_RDONLY);

		if (fd < 0)
			_exit(10);
		if (read(fd, buf, 1) != 1)
			_exit(11);
		close(fd);
		/* Encode which file we read in the exit code. */
		_exit(buf[0] == 'A' ? 0 : (buf[0] == 'B' ? 1 : 12));
	}

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_openat);

	memset(&pinreq, 0, sizeof(pinreq));
	memset(readback, 0, sizeof(readback));
	pinreq.id           = req.id;
	pinreq.nr_args      = 1;
	pinreq.buf_size     = sizeof(readback);
	pinreq.buf          = (uintptr_t)readback;
	pinreq.args[0].arg_idx   = 1;	/* openat: pathname */
	pinreq.args[0].kind      = SECCOMP_PIN_CSTRING;
	pinreq.args[0].max_bytes = PATH_MAX;

	ret = ioctl(listener, SECCOMP_IOCTL_NOTIF_PIN_ARGS, &pinreq);
	ASSERT_EQ(ret, 0) {
		TH_LOG("PIN_ARGS failed: %s", strerror(errno));
	}
	EXPECT_STREQ(readback, path_a);
	EXPECT_EQ(pinreq.args[0].truncated, 0);

	/* Race: mutate the path before SEND. */
	strcpy(shared_path, path_b);

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));
	/* Child read 'A' if pin won, 'B' if mutation won. */
	EXPECT_EQ(0, WEXITSTATUS(status)) {
		TH_LOG("opened %s instead of pinned %s",
		       WEXITSTATUS(status) == 1 ? "path_b" : "?", path_a);
	}

	unlink(path_a);
	unlink(path_b);
	munmap(shared_path, PATH_MAX);
	close(listener);
}

/* CSTRING truncation: ask for fewer bytes than the actual path; verify
 * the truncation flag is set and actual_size == max_bytes.
 */
TEST(pin_args_cstring_truncated)
{
	char *shared_path;
	char path[128];
	struct seccomp_notif_pin_args pinreq;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[16];
	int listener, status;
	pid_t pid;

	snprintf(path, sizeof(path),
		 "/tmp/seccomp-pin-trunc-%d-LONG-PATH-NAME", getpid());

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

	shared_path = mmap(NULL, PATH_MAX, PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, shared_path);
	memset(shared_path, 0, PATH_MAX);
	strcpy(shared_path, path);

	listener = install_user_notif_filter(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		/* Will fail with ENOENT — we don't care, we just want to
		 * trigger the trap so the supervisor can run PIN_ARGS.
		 */
		openat(AT_FDCWD, shared_path, O_RDONLY);
		_exit(0);
	}

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	memset(&pinreq, 0, sizeof(pinreq));
	memset(readback, 0, sizeof(readback));
	pinreq.id           = req.id;
	pinreq.nr_args      = 1;
	pinreq.buf_size     = sizeof(readback);
	pinreq.buf          = (uintptr_t)readback;
	pinreq.args[0].arg_idx   = 1;
	pinreq.args[0].kind      = SECCOMP_PIN_CSTRING;
	pinreq.args[0].max_bytes = sizeof(readback);	/* deliberately small */

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_PIN_ARGS, &pinreq), 0);
	EXPECT_EQ(pinreq.args[0].truncated, SECCOMP_PIN_TRUNCATED_BYTES);
	EXPECT_EQ(pinreq.args[0].actual_size, sizeof(readback));
	/* Buffer is NUL-terminated even when truncated. */
	EXPECT_EQ(readback[sizeof(readback) - 1], '\0');

	/* Just continue normally so the child completes. */
	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));

	munmap(shared_path, PATH_MAX);
	close(listener);
}

/*
 * SECCOMP_PIN_CSTRING_ARRAY: pin argv at execve(), mutate the argv
 * pointer table (and the strings it points to) between PIN_ARGS and
 * SEND, and verify the kernel execs against the *pinned* argv.
 *
 * Reproduces the §1 attack from the design doc: the supervisor sees
 * a blessed argv, a shared peer rewrites argv between supervisor read
 * and kernel re-read, and without PIN_ARGS the kernel would exec
 * against the rewritten bytes.
 */
TEST(pin_args_execve_argv)
{
	char *shared;
	char *strA, *strB;
	char **argv_ptrs;
	struct seccomp_notif_pin_args pinreq;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[1024];
	static const char *const envp_a[] = {"CHK=A", NULL};
	int listener, status;
	pid_t pid;
	long ret;

	/*
	 * Set up the argv table and string storage in shared memory so
	 * the supervisor can mutate them between PIN_ARGS and SEND.
	 */
	shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, shared);

	/* Layout in the shared page:
	 *   [argv_ptrs: char*[3]] [strA: 32 bytes] [strB: 32 bytes]
	 */
	argv_ptrs = (char **)shared;
	strA = shared + sizeof(char *) * 3;
	strB = strA + 32;
	strcpy(strA, "/bin/true");
	strcpy(strB, "/bin/false");
	argv_ptrs[0] = strA;
	argv_ptrs[1] = NULL;	/* will mutate to strB before SEND */
	argv_ptrs[2] = NULL;

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

	listener = install_user_notif_filter(__NR_execve);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		/* execve("/bin/true", {strA, NULL}, {"CHK=A", NULL}).
		 * The supervisor will mutate argv to point at strB before
		 * CONTINUE_PINNED. With PIN_ARGS working, the kernel still
		 * execs /bin/true (filename is also pinned in this test),
		 * exit code 0. Without it, the kernel would re-read argv
		 * and exec /bin/false, exit code 1.
		 *
		 * We pin the *filename* (arg 0) too so the mutation can't
		 * change which binary runs by changing argv[0].
		 */
		execve(strA, argv_ptrs, (char *const *)envp_a);
		_exit(99);
	}

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_execve);

	/* Pin filename (CSTRING) and argv (CSTRING_ARRAY). */
	memset(&pinreq, 0, sizeof(pinreq));
	memset(readback, 0, sizeof(readback));
	pinreq.id           = req.id;
	pinreq.nr_args      = 2;
	pinreq.buf_size     = sizeof(readback);
	pinreq.buf          = (uintptr_t)readback;

	pinreq.args[0].arg_idx   = 0;
	pinreq.args[0].kind      = SECCOMP_PIN_CSTRING;
	pinreq.args[0].max_bytes = 64;

	pinreq.args[1].arg_idx     = 1;
	pinreq.args[1].kind        = SECCOMP_PIN_CSTRING_ARRAY;
	pinreq.args[1].max_bytes   = 512;
	pinreq.args[1].max_entries = 8;

	ret = ioctl(listener, SECCOMP_IOCTL_NOTIF_PIN_ARGS, &pinreq);
	ASSERT_EQ(ret, 0) {
		TH_LOG("PIN_ARGS failed: %s", strerror(errno));
	}
	EXPECT_EQ(pinreq.args[1].actual_entries, 1);

	/* Mutate the argv pointer table to swap in strB ("/bin/false"). */
	argv_ptrs[0] = strB;

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));
	/* /bin/true exits 0; /bin/false exits 1; execve failure exits 99. */
	EXPECT_EQ(0, WEXITSTATUS(status)) {
		TH_LOG("expected /bin/true (pinned) but got exit code %d",
		       WEXITSTATUS(status));
	}

	munmap(shared, 4096);
	close(listener);
}

/*
 * SECCOMP_PIN_FIXED applied to write(fd, buf, count): pin @buf via
 * PIN_ARGS, mutate the underlying shared bytes between PIN_ARGS and
 * SEND, and verify the bytes the kernel actually writes to disk are
 * the *pinned* ones, not the mutated ones.
 */
TEST(pin_args_write_buf)
{
	char *shared_buf;
	char file_path[64];
	struct seccomp_notif_pin_args pinreq;
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	const char *pinned_msg = "PINNED";
	const char *mutated_msg = "MUTATED";
	size_t msg_len = strlen(pinned_msg);
	char readback[16];
	char file_content[16];
	int listener, status, file_fd;
	pid_t pid;
	long ret;

	snprintf(file_path, sizeof(file_path),
		 "/tmp/seccomp-pin-write-%d", getpid());
	unlink(file_path);

	file_fd = open(file_path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	ASSERT_GE(file_fd, 0);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

	shared_buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, shared_buf);
	memcpy(shared_buf, pinned_msg, msg_len);

	listener = install_user_notif_filter(__NR_write);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		ssize_t n;

		n = write(file_fd, shared_buf, msg_len);
		_exit(n == (ssize_t)msg_len ? 0 : 10);
	}

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_write);

	memset(&pinreq, 0, sizeof(pinreq));
	memset(readback, 0, sizeof(readback));
	pinreq.id           = req.id;
	pinreq.nr_args      = 1;
	pinreq.buf_size     = sizeof(readback);
	pinreq.buf          = (uintptr_t)readback;
	pinreq.args[0].arg_idx   = 1;	/* write: buf */
	pinreq.args[0].kind      = SECCOMP_PIN_FIXED;
	pinreq.args[0].max_bytes = msg_len;

	ret = ioctl(listener, SECCOMP_IOCTL_NOTIF_PIN_ARGS, &pinreq);
	ASSERT_EQ(ret, 0) {
		TH_LOG("PIN_ARGS failed: %s", strerror(errno));
	}
	EXPECT_EQ(pinreq.args[0].actual_size, msg_len);
	EXPECT_EQ(0, memcmp(readback, pinned_msg, msg_len));

	/* Race: rewrite the buffer the child is about to write. */
	memcpy(shared_buf, mutated_msg, msg_len);

	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	close(file_fd);
	file_fd = open(file_path, O_RDONLY);
	ASSERT_GE(file_fd, 0);
	memset(file_content, 0, sizeof(file_content));
	EXPECT_EQ((ssize_t)msg_len,
		  read(file_fd, file_content, sizeof(file_content)));
	close(file_fd);

	/* The pinned bytes should be on disk. */
	EXPECT_EQ(0, memcmp(file_content, pinned_msg, msg_len)) {
		TH_LOG("file contained '%.*s'; expected '%s'",
		       (int)msg_len, file_content, pinned_msg);
	}

	unlink(file_path);
	munmap(shared_buf, 4096);
	close(listener);
}

/*
 * The pin is single-shot: after CONTINUE_PINNED, the subsequent
 * task_work-driven clear must run before the trapped task issues its
 * *next* filtered syscall, so a second PIN_ARGS for the new notif id
 * succeeds (no stale -EEXIST). Validates the post-syscall lifecycle.
 */
TEST(pin_args_one_shot)
{
	struct sockaddr_un *shared;
	char path_a[64], path_b[64];
	struct seccomp_notif req = {};
	struct seccomp_notif_resp resp = {};
	char readback[sizeof(struct sockaddr_un)];
	int listener, status;
	pid_t pid;

	snprintf(path_a, sizeof(path_a), "/tmp/seccomp-pin-1shot-%d-A", getpid());
	snprintf(path_b, sizeof(path_b), "/tmp/seccomp-pin-1shot-%d-B", getpid());
	unlink(path_a);
	unlink(path_b);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(MAP_FAILED, shared);

	listener = install_user_notif_filter(__NR_bind);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		int fd1, fd2;

		memset(shared, 0, sizeof(*shared));
		shared->sun_family = AF_UNIX;
		strcpy(shared->sun_path, path_a);
		fd1 = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (fd1 < 0)
			_exit(10);
		if (bind(fd1, (struct sockaddr *)shared,
			 sizeof(*shared)) < 0)
			_exit(11);

		strcpy(shared->sun_path, path_b);
		fd2 = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (fd2 < 0)
			_exit(12);
		if (bind(fd2, (struct sockaddr *)shared,
			 sizeof(*shared)) < 0)
			_exit(13);
		_exit(0);
	}

	/* First trap: bind(path_a). Pin and CONTINUE_PINNED. */
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(listener, req.id,
				     readback, sizeof(readback)));
	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	/* Second trap: bind(path_b). PIN_ARGS must succeed (no stale pin
	 * from the first trap leaking via -EEXIST).
	 */
	memset(&req, 0, sizeof(req));
	memset(&resp, 0, sizeof(resp));
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_bind);
	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(listener, req.id,
				     readback, sizeof(readback))) {
		TH_LOG("second PIN_ARGS failed (errno=%d %s); pin from prior trap may have leaked",
		       errno, strerror(errno));
	}
	resp.id    = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_CONTINUE_PINNED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));

	struct stat st;

	EXPECT_EQ(stat(path_a, &st), 0);
	EXPECT_EQ(stat(path_b, &st), 0);

	unlink(path_a);
	unlink(path_b);
	munmap(shared, sizeof(*shared));
	close(listener);
}

/* SIGKILL the trapped child while a pin is attached but not yet armed.
 * The kpa must be freed; supervisor's listener fd must remain healthy.
 */
TEST(pin_args_sigkill_child)
{
	struct bind_race r;
	struct seccomp_notif req = {};
	char readback[sizeof(struct sockaddr_un)];
	int status;

	ASSERT_EQ(0, bind_race_setup(&r));

	EXPECT_EQ(ioctl(r.listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	memset(readback, 0, sizeof(readback));
	ASSERT_EQ(0, do_pin_sockaddr(r.listener, req.id,
				     readback, sizeof(readback)));

	/* Pin attached, not armed. Kill the child mid-wait. */
	kill(r.child, SIGKILL);

	EXPECT_EQ(waitpid(r.child, &status, 0), r.child);
	r.child = -1;
	EXPECT_EQ(true, WIFSIGNALED(status));
	EXPECT_EQ(SIGKILL, WTERMSIG(status));

	/*
	 * Listener fd is still valid. F_GETFD returns the FD flags
	 * (FD_CLOEXEC is set on the listener by seccomp), so the
	 * health-check is "not -1", not "== 0".
	 */
	EXPECT_NE(-1, fcntl(r.listener, F_GETFD));

	bind_race_teardown(&r);
}

TEST_HARNESS_MAIN
