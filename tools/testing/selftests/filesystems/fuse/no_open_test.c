// SPDX-License-Identifier: GPL-2.0
/* Exercise the wire protocol without requiring new libfuse interfaces. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/fuse.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../kselftest_harness.h"

enum { FILE_ID = 2, DIR_ID, CONTROL_ID, CREATE_ID, NR_INODES };

struct server {
	int fd;
	bool negotiate;
	bool atomic_trunc;
	bool writeback;
	atomic_bool supported;
	atomic_uint attr_flags;
	atomic_int flock_error;
	atomic_bool open_enosys;
	unsigned int opens[NR_INODES];
	unsigned int releases[NR_INODES];
	unsigned int ioctls[NR_INODES];
	unsigned int zero_ioctls;
	unsigned int zero_reads;
	unsigned int zero_readdirs;
	unsigned int statx_count;
	unsigned int lookup_count;
	unsigned int plus_count;
	unsigned int truncates;
	unsigned int flocks;
	atomic_uint release_unlocks;
	uint64_t lock_owner;
	uint64_t flock_release_fh;
	atomic_bool flock_held;
	unsigned int size;
	bool created;
	int error;
};

static void reply(struct server *s, uint64_t unique, int error,
		  void *data, size_t len)
{
	struct fuse_out_header out = {
		.len = sizeof(out) + len,
		.error = error,
		.unique = unique,
	};
	struct iovec iov[] = { { &out, sizeof(out) }, { data, len } };
	ssize_t written;

	written = writev(s->fd, iov, len ? 2 : 1);
	if (written != out.len) {
		s->error = written < 0 ? errno : EIO;
		fprintf(stderr, "reply to request %llu failed: %s\n",
			(unsigned long long)unique, strerror(s->error));
	}
}

static void fill_attr(struct server *s, struct fuse_attr *attr, uint64_t id)
{
	bool isdir = id == FUSE_ROOT_ID || id == DIR_ID;

	attr->ino = id;
	attr->mode = (isdir ? S_IFDIR : S_IFREG) | 0755;
	attr->nlink = isdir ? 2 : 1;
	attr->size = isdir ? 0 : s->size;
	attr->blksize = 4096;
	if (id != FUSE_ROOT_ID && id != CONTROL_ID)
		attr->flags = atomic_load(&s->attr_flags);
}

static void fill_entry(struct server *s, struct fuse_entry_out *out, uint64_t id)
{
	out->nodeid = id;
	out->generation = 1;
	out->entry_valid = 3600;
	out->attr_valid = 3600;
	fill_attr(s, &out->attr, id);
}

static void *serve(void *arg)
{
	struct server *s = arg;
	union {
		struct fuse_in_header header;
		char bytes[16384];
	} buf;

	for (;;) {
		struct fuse_in_header *in = &buf.header;
		void *payload = in + 1;
		ssize_t len = read(s->fd, &buf, sizeof(buf));
		uint64_t id;

		if (len < 0 && errno == EINTR)
			continue;
		if (len < 0 && errno == ENODEV)
			return NULL;
		if (len < (ssize_t)sizeof(*in)) {
			s->error = EIO;
			return NULL;
		}
		id = in->nodeid;
		if (id >= NR_INODES) {
			s->error = EINVAL;
			reply(s, in->unique, -EINVAL, NULL, 0);
			continue;
		}
		switch (in->opcode) {
		case FUSE_INIT: {
			struct fuse_init_in *init = payload;
			struct fuse_init_out out = {
				.major = FUSE_KERNEL_VERSION,
				.minor = FUSE_KERNEL_MINOR_VERSION,
				.max_write = 4096,
				.flags = FUSE_INIT_EXT | FUSE_DO_READDIRPLUS |
					 FUSE_FLOCK_LOCKS | FUSE_HAS_IOCTL_DIR,
			};
			uint64_t flags = init->flags;

			if (init->flags & FUSE_INIT_EXT)
				flags |= (uint64_t)init->flags2 << 32;
			atomic_store(&s->supported, flags & FUSE_PER_INODE_NO_OPEN);
			if (out.minor > init->minor)
				out.minor = init->minor;
			if (s->negotiate)
				out.flags2 = (flags & FUSE_PER_INODE_NO_OPEN) >> 32;
			if (s->atomic_trunc)
				out.flags |= FUSE_ATOMIC_O_TRUNC;
			if (s->writeback)
				out.flags |= FUSE_WRITEBACK_CACHE;
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_LOOKUP: {
			struct fuse_entry_out out = {};
			const char *name = payload;

			s->lookup_count++;
			if (!strcmp(name, "file")) {
				id = FILE_ID;
			} else if (!strcmp(name, "dir")) {
				id = DIR_ID;
			} else if (!strcmp(name, "control")) {
				id = CONTROL_ID;
			} else if (!strcmp(name, "created") && s->created) {
				id = CREATE_ID;
			} else {
				reply(s, in->unique, -ENOENT, NULL, 0);
				break;
			}
			fill_entry(s, &out, id);
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_GETATTR:
		case FUSE_SETATTR: {
			struct fuse_attr_out out = { .attr_valid = 3600 };

			if (in->opcode == FUSE_SETATTR) {
				struct fuse_setattr_in *attr = payload;

				if (attr->valid & FATTR_SIZE) {
					s->size = attr->size;
					s->truncates++;
				}
			}
			fill_attr(s, &out.attr, id);
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_STATX: {
			struct fuse_statx_out out = { .attr_valid = 3600 };
			struct fuse_attr attr = {};

			s->statx_count++;
			fill_attr(s, &attr, id);
			out.stat.mask = STATX_BASIC_STATS | STATX_BTIME;
			out.stat.ino = attr.ino;
			out.stat.mode = attr.mode;
			out.stat.nlink = attr.nlink;
			out.stat.size = attr.size;
			out.stat.blksize = attr.blksize;
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_OPEN:
		case FUSE_OPENDIR: {
			struct fuse_open_in *open = payload;
			struct fuse_open_out out = {
				.fh = 100 + id,
				.open_flags = FOPEN_KEEP_CACHE,
			};

			s->opens[id]++;
			if (atomic_load(&s->open_enosys)) {
				reply(s, in->unique, -ENOSYS, NULL, 0);
				break;
			}
			/* Unknown wire bits must not act as private release flags. */
			out.open_flags |= 1U << 30;
			if (open->flags & O_TRUNC) {
				s->size = 0;
				s->truncates++;
			}
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_CREATE: {
			struct {
				struct fuse_entry_out entry;
				struct fuse_open_out open;
			} out = {};

			s->created = true;
			fill_entry(s, &out.entry, CREATE_ID);
			out.open.fh = 100 + CREATE_ID;
			out.open.open_flags = FOPEN_KEEP_CACHE;
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_READ: {
			struct fuse_read_in *read = payload;
			char data = 'x';

			if (!read->fh)
				s->zero_reads++;
			reply(s, in->unique, 0, &data,
			      read->offset < s->size && read->size ? 1 : 0);
			break;
		}
		case FUSE_READDIR:
		case FUSE_READDIRPLUS: {
			struct fuse_read_in *read = payload;
			union {
				struct fuse_direntplus entry;
				char data[FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET_DIRENTPLUS + 4)];
			} out = {};
			struct fuse_direntplus *entry = &out.entry;

			if (!read->fh)
				s->zero_readdirs++;
			if (id != FUSE_ROOT_ID || read->offset ||
			    in->opcode != FUSE_READDIRPLUS) {
				reply(s, in->unique, 0, NULL, 0);
				break;
			}
			s->plus_count++;
			fill_entry(s, &entry->entry_out, FILE_ID);
			entry->dirent.ino = FILE_ID;
			entry->dirent.off = 1;
			entry->dirent.namelen = 4;
			entry->dirent.type = DT_REG;
			memcpy(entry->dirent.name, "file", 4);
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_IOCTL: {
			struct fuse_ioctl_in *ioctl = payload;
			struct {
				struct fuse_ioctl_out header;
				unsigned int flags;
			} out = {};

			if (ioctl->cmd != FS_IOC_GETFLAGS || ioctl->in_size ||
			    ioctl->out_size != sizeof(out.flags)) {
				reply(s, in->unique, -ENOTTY, NULL, 0);
				break;
			}
			s->ioctls[id]++;
			if (!ioctl->fh)
				s->zero_ioctls++;
			reply(s, in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_SETLK:
		case FUSE_SETLKW: {
			struct fuse_lk_in *lk = payload;
			int error = atomic_load(&s->flock_error);

			if (id != FILE_ID || !(lk->lk_flags & FUSE_LK_FLOCK))
				s->error = EINVAL;
			if (error && lk->lk.type != F_UNLCK) {
				reply(s, in->unique, -error, NULL, 0);
				break;
			}
			if (lk->lk.type == F_UNLCK) {
				if (lk->owner != s->lock_owner)
					s->error = EINVAL;
				atomic_store(&s->flock_held, false);
			} else {
				s->flocks++;
				s->lock_owner = lk->owner;
				atomic_store(&s->flock_held, true);
			}
			reply(s, in->unique, 0, NULL, 0);
			break;
		}
		case FUSE_RELEASE:
		case FUSE_RELEASEDIR: {
			struct fuse_release_in *release = payload;

			s->releases[id]++;
			if (release->fh != 100 + id &&
			    !(s->negotiate && id == FILE_ID && !release->fh &&
			      (release->release_flags & FUSE_RELEASE_FLOCK_UNLOCK)))
				s->error = EINVAL;
			if (release->release_flags & FUSE_RELEASE_FLOCK_UNLOCK) {
				if (!s->flocks || release->lock_owner != s->lock_owner)
					s->error = EINVAL;
				atomic_fetch_add(&s->release_unlocks, 1);
				s->flock_release_fh = release->fh;
				atomic_store(&s->flock_held, false);
			}
			reply(s, in->unique, 0, NULL, 0);
			break;
		}
		case FUSE_FLUSH:
		case FUSE_FSYNC:
		case FUSE_ACCESS:
		case FUSE_DESTROY:
			reply(s, in->unique, 0, NULL, 0);
			break;
		case FUSE_FORGET:
		case FUSE_BATCH_FORGET:
			break;
		default:
			reply(s, in->unique, -ENOSYS, NULL, 0);
		}
	}
}

FIXTURE(no_open) {
	struct server server;
	pthread_t thread;
	bool running;
	bool mounted;
	char dir[64];
	int root;
	pid_t child;
};

FIXTURE_VARIANT(no_open) {
	bool negotiate;
	bool atomic_trunc;
	bool writeback;
};

FIXTURE_VARIANT_ADD(no_open, unnegotiated) {};

FIXTURE_VARIANT_ADD(no_open, negotiated) { .negotiate = true };

FIXTURE_VARIANT_ADD(no_open, atomic_trunc) {
	.negotiate = true, .atomic_trunc = true,
};

FIXTURE_VARIANT_ADD(no_open, writeback) {
	.negotiate = true, .atomic_trunc = true, .writeback = true,
};

static void cleanup(FIXTURE_DATA(no_open) * self)
{
	if (self->child > 0) {
		kill(self->child, SIGKILL);
		while (waitpid(self->child, NULL, 0) < 0 && errno == EINTR)
			;
		self->child = 0;
	}
	if (self->root >= 0)
		close(self->root);
	if (self->mounted)
		umount2(self->dir, MNT_DETACH);
	if (self->running) {
		pthread_cancel(self->thread);
		pthread_join(self->thread, NULL);
	}
	if (self->server.fd >= 0)
		close(self->server.fd);
	if (self->dir[0])
		rmdir(self->dir);
}

FIXTURE_SETUP(no_open)
{
	struct server *s = &self->server;
	struct stat st;
	char opts[128];
	int err;

	self->root = -1;
	s->fd = -1;
	if (unshare(CLONE_NEWNS))
		SKIP(return, "need a mount namespace: %s", strerror(errno));
	ASSERT_EQ(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), 0);
	s->fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (s->fd < 0)
		SKIP(return, "cannot open /dev/fuse: %s", strerror(errno));
	strcpy(self->dir, "/tmp/fuse_no_open_XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	snprintf(opts, sizeof(opts),
		 "fd=%d,rootmode=40000,user_id=%u,group_id=%u,default_permissions",
		 s->fd, getuid(), getgid());
	if (mount("fuse", self->dir, "fuse", MS_NOSUID | MS_NODEV, opts)) {
		err = errno;
		cleanup(self);
		SKIP(return, "cannot mount FUSE: %s", strerror(err));
	}
	self->mounted = true;
	s->negotiate = variant->negotiate;
	s->atomic_trunc = variant->atomic_trunc;
	s->writeback = variant->writeback;
	s->size = 1;
	atomic_init(&s->attr_flags, FUSE_ATTR_NO_OPEN);
	atomic_init(&s->supported, false);
	atomic_init(&s->flock_error, 0);
	atomic_init(&s->open_enosys, false);
	atomic_init(&s->release_unlocks, 0);
	atomic_init(&s->flock_held, false);
	err = pthread_create(&self->thread, NULL, serve, s);
	if (err) {
		cleanup(self);
		ASSERT_EQ(err, 0);
	}
	self->running = true;
	ASSERT_EQ(stat(self->dir, &st), 0);
	if (variant->negotiate && !atomic_load(&s->supported)) {
		cleanup(self);
		SKIP(return, "kernel lacks FUSE_PER_INODE_NO_OPEN");
	}
	self->root = open(self->dir, O_PATH | O_DIRECTORY);
	ASSERT_GE(self->root, 0);
}

FIXTURE_TEARDOWN(no_open)
{
	cleanup(self);
}

static void sync_requests(struct __test_metadata *_metadata,
			  FIXTURE_DATA(no_open) * self)
{
	struct statx st;

	/*
	 * This server handles requests one at a time. The first GETATTR waits
	 * for earlier I/O replies and their completion callbacks. The second
	 * drains RELEASEs that those callbacks may have queued. Unmount alone
	 * would abort pending requests and could race with the server's replies.
	 */
	ASSERT_EQ(statx(self->root, ".", AT_STATX_FORCE_SYNC,
			STATX_BASIC_STATS, &st), 0);
	ASSERT_EQ(statx(self->root, ".", AT_STATX_FORCE_SYNC,
			STATX_BASIC_STATS, &st), 0);
}

static void finish(struct __test_metadata *_metadata, FIXTURE_DATA(no_open) * self)
{
	sync_requests(_metadata, self);
	ASSERT_EQ(close(self->root), 0);
	self->root = -1;
	ASSERT_EQ(umount(self->dir), 0);
	self->mounted = false;
	ASSERT_EQ(pthread_join(self->thread, NULL), 0);
	self->running = false;
	ASSERT_EQ(self->server.error, 0);
}

static void refresh_attr_at(struct __test_metadata *_metadata,
			    FIXTURE_DATA(no_open) * self, const char *name,
			    unsigned int mask)
{
	struct statx st;

	ASSERT_EQ(statx(self->root, name, AT_STATX_FORCE_SYNC, mask, &st), 0);
}

static void refresh_attr(struct __test_metadata *_metadata,
			 FIXTURE_DATA(no_open) * self, unsigned int mask)
{
	refresh_attr_at(_metadata, self, "file", mask);
}

static void check_flock_release(struct __test_metadata *_metadata,
				FIXTURE_DATA(no_open) * self, bool no_open)
{
	EXPECT_EQ(self->server.opens[FILE_ID], !no_open);
	EXPECT_EQ(self->server.releases[FILE_ID], 1);
	EXPECT_EQ(self->server.flocks, 1);
	EXPECT_EQ(atomic_load(&self->server.release_unlocks), 1);
	EXPECT_EQ(self->server.flock_release_fh, no_open ? 0 : 100 + FILE_ID);
	EXPECT_FALSE(atomic_load(&self->server.flock_held));
}

TEST_F(no_open, files_and_directories)
{
	int fd = openat(self->root, "file", O_RDONLY);
	DIR *dir;
	char data;

	ASSERT_GE(fd, 0);
	ASSERT_EQ(read(fd, &data, 1), 1);
	EXPECT_EQ(data, 'x');
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(fd, 0);
	dir = fdopendir(fd);
	ASSERT_NE(dir, NULL);
	errno = 0;
	ASSERT_EQ(readdir(dir), NULL);
	ASSERT_EQ(errno, 0);
	ASSERT_EQ(closedir(dir), 0);
	fd = openat(self->root, "control", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.opens[DIR_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[DIR_ID], !variant->negotiate);
	EXPECT_EQ(self->server.opens[CONTROL_ID], 1);
	EXPECT_EQ(self->server.releases[CONTROL_ID], 1);
	EXPECT_EQ(self->server.zero_reads, variant->negotiate);
	EXPECT_EQ(self->server.zero_readdirs, variant->negotiate);
}

TEST_F(no_open, hint_changes_with_open_handles)
{
	int local, remote, local_again;

	local = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(local, 0);
	atomic_store(&self->server.attr_flags, 0);
	refresh_attr(_metadata, self, STATX_BASIC_STATS);
	remote = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(remote, 0);
	/* Clearing the hint must not cause a RELEASE for the first handle. */
	ASSERT_EQ(close(local), 0);
	atomic_store(&self->server.attr_flags, FUSE_ATTR_NO_OPEN);
	refresh_attr(_metadata, self, STATX_BASIC_STATS);
	local_again = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(local_again, 0);
	/* Setting the hint must not lose the server's existing handle. */
	ASSERT_EQ(close(remote), 0);
	ASSERT_EQ(close(local_again), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[FILE_ID], variant->negotiate ? 1 : 3);
	EXPECT_EQ(self->server.releases[FILE_ID], variant->negotiate ? 1 : 3);
}

TEST_F(no_open, statx_preserves_hint)
{
	int fd;

	refresh_attr(_metadata, self, STATX_BASIC_STATS | STATX_BTIME);
	fd = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.statx_count, 1);
	EXPECT_EQ(self->server.opens[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[FILE_ID], !variant->negotiate);
}

TEST_F(no_open, directory_hint_changes_with_open_handles)
{
	int local, remote, local_again;

	local = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(local, 0);
	atomic_store(&self->server.attr_flags, 0);
	refresh_attr_at(_metadata, self, "dir", STATX_BASIC_STATS);
	remote = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(remote, 0);
	ASSERT_EQ(close(local), 0);
	atomic_store(&self->server.attr_flags, FUSE_ATTR_NO_OPEN);
	refresh_attr_at(_metadata, self, "dir", STATX_BASIC_STATS);
	local_again = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(local_again, 0);
	ASSERT_EQ(close(remote), 0);
	ASSERT_EQ(close(local_again), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[DIR_ID], variant->negotiate ? 1 : 3);
	EXPECT_EQ(self->server.releases[DIR_ID], variant->negotiate ? 1 : 3);
}

TEST_F(no_open, statx_preserves_cleared_hint)
{
	int fd;

	atomic_store(&self->server.attr_flags, 0);
	refresh_attr(_metadata, self, STATX_BASIC_STATS);
	atomic_store(&self->server.attr_flags, FUSE_ATTR_NO_OPEN);
	refresh_attr(_metadata, self, STATX_BASIC_STATS | STATX_BTIME);
	fd = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.statx_count, 1);
	EXPECT_EQ(self->server.opens[FILE_ID], 1);
	EXPECT_EQ(self->server.releases[FILE_ID], 1);
}

TEST_F(no_open, setattr_updates_hint)
{
	int local, remote, local_again;

	local = openat(self->root, "file", O_RDWR);
	ASSERT_GE(local, 0);
	atomic_store(&self->server.attr_flags, 0);
	ASSERT_EQ(ftruncate(local, 0), 0);
	remote = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(remote, 0);
	atomic_store(&self->server.attr_flags, FUSE_ATTR_NO_OPEN);
	ASSERT_EQ(ftruncate(local, 1), 0);
	local_again = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(local_again, 0);
	ASSERT_EQ(close(local), 0);
	ASSERT_EQ(close(remote), 0);
	ASSERT_EQ(close(local_again), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.truncates, 2);
	EXPECT_EQ(self->server.opens[FILE_ID], variant->negotiate ? 1 : 3);
	EXPECT_EQ(self->server.releases[FILE_ID], variant->negotiate ? 1 : 3);
}

TEST_F(no_open, truncate)
{
	int fd = openat(self->root, "file", O_WRONLY | O_TRUNC);
	bool server_open = !variant->negotiate || variant->atomic_trunc;
	struct stat st;

	ASSERT_GE(fd, 0);
	ASSERT_EQ(fstat(fd, &st), 0);
	EXPECT_EQ(st.st_size, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.size, 0);
	EXPECT_EQ(self->server.truncates, 1);
	EXPECT_EQ(self->server.opens[FILE_ID], server_open);
	EXPECT_EQ(self->server.releases[FILE_ID], server_open);
}

TEST_F(no_open, create_releases_handle)
{
	int fd = openat(self->root, "created", O_CREAT | O_EXCL | O_RDWR, 0600);

	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_TRUE(self->server.created);
	EXPECT_EQ(self->server.opens[CREATE_ID], 0);
	EXPECT_EQ(self->server.releases[CREATE_ID], 1);
}

TEST_F(no_open, close_unlocks_flock)
{
	int fd = openat(self->root, "file", O_RDONLY);

	ASSERT_GE(fd, 0);
	ASSERT_EQ(flock(fd, LOCK_EX), 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	check_flock_release(_metadata, self, variant->negotiate);
}

TEST_F(no_open, reopen_created_inode)
{
	int fd = openat(self->root, "created", O_CREAT | O_EXCL | O_RDWR, 0600);

	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "created", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[CREATE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[CREATE_ID], variant->negotiate ? 1 : 2);
}

TEST_F(no_open, explicit_flock_unlock_still_releases)
{
	int fd = openat(self->root, "file", O_RDONLY);

	ASSERT_GE(fd, 0);
	ASSERT_EQ(flock(fd, LOCK_EX), 0);
	ASSERT_EQ(flock(fd, LOCK_UN), 0);
	EXPECT_FALSE(atomic_load(&self->server.flock_held));
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	check_flock_release(_metadata, self, variant->negotiate);
}

TEST_F(no_open, failed_flock_keeps_release_suppressed)
{
	int fd = openat(self->root, "file", O_RDONLY);

	ASSERT_GE(fd, 0);
	atomic_store(&self->server.flock_error, EAGAIN);
	ASSERT_EQ(flock(fd, LOCK_EX | LOCK_NB), -1);
	EXPECT_EQ(errno, EAGAIN);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.flocks, 0);
	EXPECT_EQ(atomic_load(&self->server.release_unlocks), 0);
}

TEST_F(no_open, duplicated_flock_releases_on_last_close)
{
	int fd = openat(self->root, "file", O_RDONLY);
	int duplicate;

	ASSERT_GE(fd, 0);
	ASSERT_EQ(flock(fd, LOCK_EX), 0);
	duplicate = dup(fd);
	ASSERT_GE(duplicate, 0);
	ASSERT_EQ(close(fd), 0);
	sync_requests(_metadata, self);
	EXPECT_EQ(atomic_load(&self->server.release_unlocks), 0);
	EXPECT_TRUE(atomic_load(&self->server.flock_held));
	ASSERT_EQ(close(duplicate), 0);
	finish(_metadata, self);
	check_flock_release(_metadata, self, variant->negotiate);
}

TEST_F(no_open, killed_process_releases_flock)
{
	int notify[2], status;
	char ready;
	pid_t pid;

	ASSERT_EQ(pipe2(notify, O_CLOEXEC), 0);
	pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		int fd;

		close(notify[0]);
		alarm(30);
		fd = openat(self->root, "file", O_RDONLY);
		if (fd < 0 || flock(fd, LOCK_EX))
			_exit(1);
		if (write(notify[1], "x", 1) != 1)
			_exit(1);
		for (;;)
			pause();
	}
	self->child = pid;
	ASSERT_EQ(close(notify[1]), 0);
	ASSERT_EQ(read(notify[0], &ready, 1), 1);
	ASSERT_EQ(close(notify[0]), 0);
	ASSERT_EQ(kill(pid, SIGKILL), 0);
	ASSERT_EQ(waitpid(pid, &status, 0), pid);
	self->child = 0;
	EXPECT_TRUE(WIFSIGNALED(status));
	EXPECT_EQ(WTERMSIG(status), SIGKILL);
	finish(_metadata, self);
	check_flock_release(_metadata, self, variant->negotiate);
}

TEST_F(no_open, connection_wide_enosys_takes_precedence)
{
	int fd;
	char data;

	atomic_store(&self->server.attr_flags, 0);
	atomic_store(&self->server.open_enosys, true);
	fd = openat(self->root, "control", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "control", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	atomic_store(&self->server.attr_flags, FUSE_ATTR_NO_OPEN);
	fd = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(read(fd, &data, 1), 1);
	EXPECT_EQ(data, 'x');
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.opens[CONTROL_ID], 1);
	EXPECT_EQ(self->server.opens[DIR_ID], 1);
	EXPECT_EQ(self->server.opens[FILE_ID], 0);
	EXPECT_EQ(self->server.releases[CONTROL_ID], 0);
	EXPECT_EQ(self->server.releases[DIR_ID], 0);
	EXPECT_EQ(self->server.releases[FILE_ID], 0);
	EXPECT_EQ(self->server.zero_reads, 1);
}

TEST_F(no_open, fileattr_ioctl_uses_hint)
{
	int flags, fd;

	fd = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ioctl(fd, FS_IOC_GETFLAGS, &flags), 0);
	EXPECT_EQ(flags, 0);
	ASSERT_EQ(close(fd), 0);
	fd = openat(self->root, "dir", O_RDONLY | O_DIRECTORY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(ioctl(fd, FS_IOC_GETFLAGS, &flags), 0);
	EXPECT_EQ(flags, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.ioctls[FILE_ID], 1);
	EXPECT_EQ(self->server.ioctls[DIR_ID], 1);
	EXPECT_EQ(self->server.zero_ioctls, variant->negotiate ? 2 : 0);
	EXPECT_EQ(self->server.opens[FILE_ID], variant->negotiate ? 0 : 2);
	EXPECT_EQ(self->server.releases[FILE_ID], variant->negotiate ? 0 : 2);
	EXPECT_EQ(self->server.opens[DIR_ID], variant->negotiate ? 0 : 2);
	EXPECT_EQ(self->server.releases[DIR_ID], variant->negotiate ? 0 : 2);
}

TEST_F(no_open, readdirplus_populates_hint)
{
	int fd = openat(self->root, ".", O_RDONLY | O_DIRECTORY);
	struct dirent *entry;
	DIR *dir;

	ASSERT_GE(fd, 0);
	dir = fdopendir(fd);
	ASSERT_NE(dir, NULL);
	entry = readdir(dir);
	ASSERT_NE(entry, NULL);
	ASSERT_STREQ(entry->d_name, "file");
	ASSERT_EQ(closedir(dir), 0);
	fd = openat(self->root, "file", O_RDONLY);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	finish(_metadata, self);
	EXPECT_EQ(self->server.plus_count, 1);
	EXPECT_EQ(self->server.lookup_count, 0);
	EXPECT_EQ(self->server.opens[FILE_ID], !variant->negotiate);
	EXPECT_EQ(self->server.releases[FILE_ID], !variant->negotiate);
}

TEST_HARNESS_MAIN
