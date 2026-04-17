// SPDX-License-Identifier: GPL-2.0
/*
 * Verify that rollback from cpu_cgroup_can_attach() failure doesn't perturb DL
 * bandwidth accounting when cpuset_can_attach() didn't allocate DL bandwidth in
 * the first place.
 *
 * The test uses a sched_ext scheduler whose cgroup_prep_move() rejects
 * SCHED_DEADLINE task migration. That makes the cpu controller fail after the
 * cpuset controller has already accepted the move, which triggers the cgroup
 * rollback path without any kernel fault injection.
 */
#define _GNU_SOURCE

#include <bpf/bpf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sched/types.h>
#include <scx/common.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cpuset_dl_rollback.bpf.skel.h"
#include "scx_test.h"

#ifndef SYS_sched_setattr
#if defined(__x86_64__)
#define SYS_sched_setattr 314
#elif defined(__i386__)
#define SYS_sched_setattr 351
#elif defined(__aarch64__)
#define SYS_sched_setattr 274
#else
#error "Unknown architecture: please define SYS_sched_setattr"
#endif
#endif

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

#define CGROUP2_ROOT "/sys/fs/cgroup"
#define SCHED_DEBUG "/sys/kernel/debug/sched/debug"

struct cpuset_dl_rollback_ctx {
	struct cpuset_dl_rollback *skel;
	struct bpf_link *link;
	pid_t child;
	/* The only CPU in dst, and the rollback accounting observation point. */
	int target_cpu;
	bool restore_parent_subtree;
	char parent[PATH_MAX];
	char root[PATH_MAX];
	char src[PATH_MAX];
	char dst[PATH_MAX];
	char src_rel[PATH_MAX];
	char parent_subtree[256];
	char cpu_list[1024];
	char mem_list[256];
	char dst_cpu[32];
};

static void cleanup(void *arg);

static int sched_setattr(pid_t pid, const struct sched_attr *attr,
			 unsigned int flags)
{
	return syscall(SYS_sched_setattr, pid, attr, flags);
}

static void trim_trailing_ws(char *buf)
{
	size_t len = strlen(buf);

	while (len > 0) {
		char c = buf[len - 1];

		if (c != '\n' && c != ' ' && c != '\t')
			break;
		buf[--len] = '\0';
	}
}

static int read_text(const char *path, char *buf, size_t size)
{
	ssize_t len;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	len = read(fd, buf, size - 1);
	close(fd);
	if (len < 0)
		return -errno;

	buf[len] = '\0';
	trim_trailing_ws(buf);
	return 0;
}

static int write_text(const char *path, const char *buf)
{
	size_t len = strlen(buf);
	ssize_t ret;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	ret = write(fd, buf, len);
	close(fd);
	if (ret < 0)
		return -errno;
	if ((size_t)ret != len)
		return -EIO;

	return 0;
}

static int build_path(char *buf, size_t size, const char *dir, const char *file)
{
	int ret;

	ret = snprintf(buf, size, "%s/%s", dir, file);
	if (ret < 0 || (size_t)ret >= size)
		return -ENAMETOOLONG;

	return 0;
}

static int build_cgroup_dir(const char *rel, char *buf, size_t size)
{
	int ret;

	if (!strcmp(rel, "/"))
		ret = snprintf(buf, size, "%s", CGROUP2_ROOT);
	else
		ret = snprintf(buf, size, "%s%s", CGROUP2_ROOT, rel);

	if (ret < 0 || (size_t)ret >= size)
		return -ENAMETOOLONG;

	return 0;
}

static int read_cgroup_relpath(const char *path, char *buf, size_t size)
{
	char line[PATH_MAX];
	FILE *fp;
	int ret;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;

	while (fgets(line, sizeof(line), fp)) {
		char *first, *second, *rel;

		trim_trailing_ws(line);

		first = strchr(line, ':');
		if (!first) {
			fclose(fp);
			return -EINVAL;
		}

		second = strchr(first + 1, ':');
		if (!second) {
			fclose(fp);
			return -EINVAL;
		}

		*first = '\0';
		*second = '\0';

		/* Match the cgroup v2 entry, which is formatted as 0::/path. */
		if (strcmp(line, "0") || first[1] != '\0')
			continue;

		rel = second + 1;
		if (rel[0] != '/') {
			fclose(fp);
			return -EINVAL;
		}

		ret = snprintf(buf, size, "%s", rel);
		fclose(fp);
		if (ret < 0 || (size_t)ret >= size)
			return -ENAMETOOLONG;

		return 0;
	}

	if (ferror(fp)) {
		fclose(fp);
		return -EIO;
	}

	fclose(fp);
	return -EOPNOTSUPP;
}

static bool has_token(const char *list, const char *token)
{
	size_t len = strlen(token);
	const char *pos = list;

	while ((pos = strstr(pos, token))) {
		bool left_ok = pos == list || pos[-1] == ' ';
		bool right_ok = pos[len] == '\0' || pos[len] == ' ';

		if (left_ok && right_ok)
			return true;
		pos += len;
	}

	return false;
}

static int enable_controllers(const char *dir, char *orig, size_t orig_sz,
			      bool *changed)
{
	char ctrl_path[PATH_MAX];
	char subtree_path[PATH_MAX];
	char controllers[256];
	char subtree[256];
	char enable[64];
	size_t len = 0;
	int ret;

	ret = build_path(ctrl_path, sizeof(ctrl_path), dir, "cgroup.controllers");
	if (ret)
		return ret;
	ret = build_path(subtree_path, sizeof(subtree_path), dir,
			 "cgroup.subtree_control");
	if (ret)
		return ret;

	ret = read_text(ctrl_path, controllers, sizeof(controllers));
	if (ret == -ENOENT)
		return -EOPNOTSUPP;
	if (ret)
		return ret;
	if (!has_token(controllers, "cpu") || !has_token(controllers, "cpuset"))
		return -EOPNOTSUPP;

	ret = read_text(subtree_path, subtree, sizeof(subtree));
	if (ret == -ENOENT)
		return -EOPNOTSUPP;
	if (ret)
		return ret;

	enable[0] = '\0';
	if (!has_token(subtree, "cpu"))
		len += snprintf(enable + len, sizeof(enable) - len, "+cpu ");
	if (!has_token(subtree, "cpuset"))
		len += snprintf(enable + len, sizeof(enable) - len, "+cpuset ");
	if (len >= sizeof(enable))
		return -EOVERFLOW;

	if (!enable[0]) {
		if (orig && orig_sz) {
			ret = snprintf(orig, orig_sz, "%s", subtree);
			if (ret < 0 || (size_t)ret >= orig_sz)
				return -ENAMETOOLONG;
		}
		if (changed)
			*changed = false;
		return 0;
	}

	if (orig && orig_sz) {
		ret = snprintf(orig, orig_sz, "%s", subtree);
		if (ret < 0 || (size_t)ret >= orig_sz)
			return -ENAMETOOLONG;
	}

	trim_trailing_ws(enable);
	ret = write_text(subtree_path, enable);
	if (!ret && changed)
		*changed = true;
	return ret;
}

static int restore_controllers(const char *dir, const char *orig)
{
	char subtree_path[PATH_MAX];
	char subtree[256];
	char disable[64];
	size_t len = 0;
	int ret;

	ret = build_path(subtree_path, sizeof(subtree_path), dir,
			 "cgroup.subtree_control");
	if (ret)
		return ret;

	ret = read_text(subtree_path, subtree, sizeof(subtree));
	if (ret)
		return ret;

	/*
	 * Only undo controllers that this test turned on. If "cpu" or "cpuset"
	 * was already present in the original subtree_control state, leave it
	 * alone.
	 */
	disable[0] = '\0';
	if (has_token(subtree, "cpu") && !has_token(orig, "cpu"))
		len += snprintf(disable + len, sizeof(disable) - len, "-cpu ");
	if (has_token(subtree, "cpuset") && !has_token(orig, "cpuset"))
		len += snprintf(disable + len, sizeof(disable) - len,
				"-cpuset ");
	if (len >= sizeof(disable))
		return -EOVERFLOW;

	if (!disable[0])
		return 0;

	trim_trailing_ws(disable);
	return write_text(subtree_path, disable);
}

static int mkdir_one(const char *path)
{
	if (mkdir(path, 0755) && errno != EEXIST)
		return -errno;
	return 0;
}

static int write_pid(const char *path, pid_t pid)
{
	char buf[32];
	int ret;

	ret = snprintf(buf, sizeof(buf), "%d", pid);
	if (ret < 0 || (size_t)ret >= sizeof(buf))
		return -EOVERFLOW;

	return write_text(path, buf);
}

/* Parse the first CPU from a cpulist-style string such as "0-3,8". */
static int first_list_item(const char *list, char *buf, size_t size, int *valp)
{
	char *end;
	long val;
	int ret;

	errno = 0;
	val = strtol(list, &end, 10);
	if (errno || end == list || val < 0)
		return -EINVAL;

	if (valp)
		*valp = val;

	ret = snprintf(buf, size, "%ld", val);
	if (ret < 0 || (size_t)ret >= size)
		return -EOVERFLOW;

	return 0;
}

/*
 * sched/debug reports dl_bw->total_bw inside each CPU section.
 *
 * This test constrains dst to a single CPU and stores that CPU number in
 * ctx->target_cpu. cpuset_cancel_attach() rolls rollback accounting against a
 * CPU selected from the destination effective mask, so with a single-CPU dst
 * that exact CPU becomes the rollback site and the matching observation point.
 *
 * Reading only the target CPU's dl_bw->total_bw avoids assuming that every CPU
 * in the system shares one root domain. Unlike sched_ext/total_bw.c, this test
 * has to identify one specific CPU section, so it also relies on the current
 * sched/debug "cpu#<n>" section header format.
 */
static int read_cpu_total_bw(int target_cpu, long long *bw)
{
	char line[256];
	FILE *fp;
	bool in_target = false;

	fp = fopen(SCHED_DEBUG, "r");
	if (!fp)
		return -errno;

	while (fgets(line, sizeof(line), fp)) {
		int header_cpu;
		char *val;

		if (sscanf(line, "cpu#%d", &header_cpu) == 1) {
			if (in_target)
				break;

			in_target = header_cpu == target_cpu;
			continue;
		}
		if (!in_target)
			continue;

		val = strstr(line, "dl_bw->total_bw");
		if (!val)
			continue;

		val = strchr(val, ':');
		if (!val) {
			fclose(fp);
			return -EINVAL;
		}

		*bw = strtoll(val + 1, NULL, 10);
		fclose(fp);
		return 0;
	}

	fclose(fp);
	return -ENOENT;
}

static int set_deadline_policy(void)
{
	struct sched_attr attr = {
		.size = sizeof(attr),
		.sched_policy = SCHED_DEADLINE,
		.sched_runtime = 10 * 1000 * 1000ULL,
		.sched_deadline = 30 * 1000 * 1000ULL,
		.sched_period = 30 * 1000 * 1000ULL,
	};

	return sched_setattr(0, &attr, 0);
}

static int spawn_dl_child(struct cpuset_dl_rollback_ctx *ctx)
{
	char procs_path[PATH_MAX];
	int pipefd[2];
	pid_t pid;
	int child_ret;
	int ret;

	ret = build_path(procs_path, sizeof(procs_path), ctx->src, "cgroup.procs");
	if (ret)
		return ret;

	if (pipe(pipefd))
		return -errno;

	pid = fork();
	if (pid < 0) {
		ret = -errno;
		close(pipefd[0]);
		close(pipefd[1]);
		return ret;
	}

	if (!pid) {
		int err = 0;

		close(pipefd[0]);

		err = write_pid(procs_path, getpid());
		if (!err && set_deadline_policy())
			err = -errno;

		if (write(pipefd[1], &err, sizeof(err)) != sizeof(err))
			_exit(1);

		if (err)
			_exit(1);

		for (;;)
			pause();
	}

	close(pipefd[1]);
	ret = read(pipefd[0], &child_ret, sizeof(child_ret));
	close(pipefd[0]);
	if (ret != sizeof(child_ret)) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return -EIO;
	}

	if (child_ret) {
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return child_ret;
	}

	ctx->child = pid;
	return 0;
}

static int create_cgroups(struct cpuset_dl_rollback_ctx *ctx)
{
	char parent_rel[PATH_MAX];
	char path[PATH_MAX];
	char tmpl[PATH_MAX];
	int ret;

	ret = read_cgroup_relpath("/proc/self/cgroup", parent_rel,
				  sizeof(parent_rel));
	if (ret)
		return ret;

	ret = build_cgroup_dir(parent_rel, ctx->parent, sizeof(ctx->parent));
	if (ret)
		return ret;

	ret = enable_controllers(ctx->parent, ctx->parent_subtree,
				 sizeof(ctx->parent_subtree),
				 &ctx->restore_parent_subtree);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->parent, "cpuset.cpus.effective");
	if (ret)
		return ret;
	ret = read_text(path, ctx->cpu_list, sizeof(ctx->cpu_list));
	if (ret == -ENOENT)
		return -EOPNOTSUPP;
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->parent, "cpuset.mems.effective");
	if (ret)
		return ret;
	ret = read_text(path, ctx->mem_list, sizeof(ctx->mem_list));
	if (ret == -ENOENT)
		return -EOPNOTSUPP;
	if (ret)
		return ret;
	if (!ctx->cpu_list[0] || !ctx->mem_list[0])
		return -ENOSPC;

	/*
	 * Keep dst on a single CPU so the rollback accounting target is
	 * deterministic. That same CPU is later sampled from sched/debug.
	 */
	ret = first_list_item(ctx->cpu_list, ctx->dst_cpu, sizeof(ctx->dst_cpu),
			      &ctx->target_cpu);
	if (ret)
		return ret;

	ret = snprintf(tmpl, sizeof(tmpl), "%s/scx-cpuset-dl-rollback-XXXXXX",
		       ctx->parent);
	if (ret < 0 || (size_t)ret >= sizeof(tmpl))
		return -ENAMETOOLONG;

	if (!mkdtemp(tmpl))
		return -errno;

	ret = snprintf(ctx->root, sizeof(ctx->root), "%s", tmpl);
	if (ret < 0 || (size_t)ret >= sizeof(ctx->root))
		return -EOVERFLOW;

	ret = snprintf(ctx->src, sizeof(ctx->src), "%s/src", ctx->root);
	if (ret < 0 || (size_t)ret >= sizeof(ctx->src))
		return -EOVERFLOW;
	ret = snprintf(ctx->dst, sizeof(ctx->dst), "%s/ovl", ctx->root);
	if (ret < 0 || (size_t)ret >= sizeof(ctx->dst))
		return -EOVERFLOW;
	ret = snprintf(ctx->src_rel, sizeof(ctx->src_rel), "%s/src",
		       ctx->root + strlen(CGROUP2_ROOT));
	if (ret < 0 || (size_t)ret >= sizeof(ctx->src_rel))
		return -EOVERFLOW;

	ret = build_path(path, sizeof(path), ctx->root, "cpuset.cpus");
	if (ret)
		return ret;
	ret = write_text(path, ctx->cpu_list);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->root, "cpuset.mems");
	if (ret)
		return ret;
	ret = write_text(path, ctx->mem_list);
	if (ret)
		return ret;

	ret = enable_controllers(ctx->root, NULL, 0, NULL);
	if (ret)
		return ret;

	ret = mkdir_one(ctx->src);
	if (ret)
		return ret;
	ret = mkdir_one(ctx->dst);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->src, "cpuset.cpus");
	if (ret)
		return ret;
	ret = write_text(path, ctx->cpu_list);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->src, "cpuset.mems");
	if (ret)
		return ret;
	ret = write_text(path, ctx->mem_list);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->dst, "cpuset.cpus");
	if (ret)
		return ret;
	ret = write_text(path, ctx->dst_cpu);
	if (ret)
		return ret;

	ret = build_path(path, sizeof(path), ctx->dst, "cpuset.mems");
	if (ret)
		return ret;
	return write_text(path, ctx->mem_list);
}

static bool child_in_src(const struct cpuset_dl_rollback_ctx *ctx)
{
	char path[PATH_MAX];
	char cgroup[PATH_MAX];
	int ret;

	ret = snprintf(path, sizeof(path), "/proc/%d/cgroup", ctx->child);
	if (ret < 0 || (size_t)ret >= sizeof(path))
		return false;

	if (read_cgroup_relpath(path, cgroup, sizeof(cgroup)))
		return false;

	return strcmp(cgroup, ctx->src_rel) == 0;
}

static enum scx_test_status setup(void **out_ctx)
{
	struct cpuset_dl_rollback_ctx *ctx;
	int ret;

	if (geteuid()) {
		fprintf(stderr, "Skipping test: root privileges required\n");
		return SCX_TEST_SKIP;
	}

	if (access(SCHED_DEBUG, R_OK)) {
		fprintf(stderr, "Skipping test: %s not accessible\n", SCHED_DEBUG);
		return SCX_TEST_SKIP;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return SCX_TEST_FAIL;

	ret = create_cgroups(ctx);
	switch (ret) {
	case -EOPNOTSUPP:
		fprintf(stderr,
			"Skipping test: cgroup v2 cpu/cpuset controllers unavailable in current cgroup tree\n");
		cleanup(ctx);
		return SCX_TEST_SKIP;
	case -EPERM:
	case -EACCES:
	case -EROFS:
		fprintf(stderr,
			"Skipping test: current cgroup tree does not allow cpu/cpuset writes\n");
		cleanup(ctx);
		return SCX_TEST_SKIP;
	case -EBUSY:
		fprintf(stderr,
			"Skipping test: current cgroup tree does not allow enabling cpu/cpuset controllers here\n");
		cleanup(ctx);
		return SCX_TEST_SKIP;
	case -ENOSPC:
		fprintf(stderr,
			"Skipping test: current cgroup does not expose enough effective cpuset resources\n");
		cleanup(ctx);
		return SCX_TEST_SKIP;
	}
	if (ret) {
		SCX_ERR("Failed to create cgroups (%d)", ret);
		cleanup(ctx);
		return SCX_TEST_FAIL;
	}

	ctx->skel = cpuset_dl_rollback__open();
	if (!ctx->skel) {
		SCX_ERR("Failed to open skel");
		cleanup(ctx);
		return SCX_TEST_FAIL;
	}
	SCX_ENUM_INIT(ctx->skel);
	if (cpuset_dl_rollback__load(ctx->skel)) {
		SCX_ERR("Failed to load skel");
		cleanup(ctx);
		return SCX_TEST_FAIL;
	}

	*out_ctx = ctx;
	return SCX_TEST_PASS;
}

static enum scx_test_status run(void *arg)
{
	struct cpuset_dl_rollback_ctx *ctx = arg;
	char procs_path[PATH_MAX];
	long long before_bw, after_bw;
	int ret;

	ret = read_cpu_total_bw(ctx->target_cpu, &before_bw);
	SCX_FAIL_IF(ret, "Failed to read baseline total_bw (%d)", ret);

	ctx->link = bpf_map__attach_struct_ops(ctx->skel->maps.cpuset_dl_rollback_ops);
	SCX_FAIL_IF(!ctx->link, "Failed to attach scheduler");

	ret = spawn_dl_child(ctx);
	switch (ret) {
	case -EACCES:
	case -EPERM:
		fprintf(stderr,
			"Skipping test: unable to place child in the source cgroup or enable SCHED_DEADLINE due to permissions (%d)\n",
			ret);
		return SCX_TEST_SKIP;
	case -EBUSY:
		fprintf(stderr,
			"Skipping test: SCHED_DEADLINE admission control rejected the child (%d)\n",
			ret);
		return SCX_TEST_SKIP;
	case -EINVAL:
		fprintf(stderr,
			"Skipping test: unable to enable SCHED_DEADLINE for the child in this environment (%d)\n",
			ret);
		return SCX_TEST_SKIP;
	}
	SCX_FAIL_IF(ret, "Failed to start SCHED_DEADLINE child (%d)", ret);

	ret = read_cpu_total_bw(ctx->target_cpu, &before_bw);
	SCX_FAIL_IF(ret, "Failed to read pre-move total_bw (%d)", ret);

	ret = build_path(procs_path, sizeof(procs_path), ctx->dst, "cgroup.procs");
	SCX_FAIL_IF(ret, "Failed to build cgroup.procs path (%d)", ret);

	ret = write_pid(procs_path, ctx->child);
	SCX_FAIL_IF(ret != -EAGAIN,
		    "Expected cgroup move failure with -EAGAIN, got %d", ret);
	SCX_FAIL_IF(!child_in_src(ctx), "Child left source cgroup after rollback");

	ret = read_cpu_total_bw(ctx->target_cpu, &after_bw);
	SCX_FAIL_IF(ret, "Failed to read post-move total_bw (%d)", ret);
	SCX_FAIL_IF(after_bw != before_bw,
		    "Expected total_bw for CPU%d to remain unchanged (%lld != %lld)",
		    ctx->target_cpu, after_bw, before_bw);

	return SCX_TEST_PASS;
}

static void cleanup(void *arg)
{
	struct cpuset_dl_rollback_ctx *ctx = arg;
	int ret;

	if (!ctx)
		return;

	if (ctx->child > 0) {
		kill(ctx->child, SIGKILL);
		waitpid(ctx->child, NULL, 0);
	}

	if (ctx->link)
		bpf_link__destroy(ctx->link);
	if (ctx->skel)
		cpuset_dl_rollback__destroy(ctx->skel);

	if (ctx->dst[0])
		rmdir(ctx->dst);
	if (ctx->src[0])
		rmdir(ctx->src);
	if (ctx->root[0])
		rmdir(ctx->root);

	if (ctx->restore_parent_subtree) {
		ret = restore_controllers(ctx->parent, ctx->parent_subtree);
		if (ret)
			fprintf(stderr,
				"%s: failed to restore %s/cgroup.subtree_control (%d)\n",
				__func__, ctx->parent, ret);
	}

	free(ctx);
}

struct scx_test cpuset_dl_rollback = {
	.name = "cpuset_dl_rollback",
	.description = "Verify attach rollback after cpuset preserves DL bandwidth accounting",
	.setup = setup,
	.run = run,
	.cleanup = cleanup,
};
REGISTER_SCX_TEST(&cpuset_dl_rollback)
