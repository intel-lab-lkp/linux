// SPDX-License-Identifier: GPL-2.0
/*
 * Regression tests for NFSD_CMD_FILEHANDLE_GET: root filehandle lookup
 * over generic netlink.
 *
 * Each test runs in a private net + mount namespace. The pushed export
 * lives in the per-netns svc_export cache and dies with the namespace;
 * the client domain itself is created in sunrpc's global auth_domain
 * table, like any real client name would be.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nfsd/export.h>
#include <linux/nfsd_netlink.h>
#include <linux/sunrpc_netlink.h>

#include "../kselftest_harness.h"
#include "nfsd_netlink.h"

#define FH_DOMAIN	"nfsd-fh-selftest"
#define FH_MAXSIZE	64
#define FH_TTL		3600
#define FH_FSID		4242

static int sunrpc_family = -1;

/* Push one ip_map entry the way mountd's cache_nl_process_ip_map() does. */
static int ip_map_set(const char *domain)
{
	char attrs[256];
	struct nlattr *nest = (void *)attrs;
	uint64_t expiry = (uint64_t)time(NULL) + FH_TTL;
	static const char class[] = "nfsd";
	static const char addr[] = "127.0.0.2";
	int inner = NLA_HDRLEN;

	inner = put_attr(attrs, inner, SUNRPC_A_IP_MAP_CLASS,
			 class, sizeof(class));
	inner = put_attr(attrs, inner, SUNRPC_A_IP_MAP_ADDR,
			 addr, sizeof(addr));
	inner = put_attr(attrs, inner, SUNRPC_A_IP_MAP_DOMAIN,
			 domain, strlen(domain) + 1);
	inner = put_attr(attrs, inner, SUNRPC_A_IP_MAP_EXPIRY,
			 &expiry, sizeof(expiry));
	nest->nla_type = SUNRPC_A_IP_MAP_REQS_REQUESTS | NLA_F_NESTED;
	nest->nla_len = inner;

	return genl_request_to(sunrpc_family, SUNRPC_CMD_IP_MAP_SET_REQS,
			       attrs, NLA_ALIGN4(inner));
}

/*
 * Push one positive svc_export entry for @domain/@path. An explicit fsid
 * lets this work when @path is on tmpfs or another fs without a device.
 */
static int export_set(const char *domain, const char *path)
{
	char attrs[512];
	struct nlattr *nest = (void *)attrs;
	uint64_t expiry = (uint64_t)time(NULL) + FH_TTL;
	uint32_t anon = 65534, flags = NFSEXP_FSID;
	int32_t fsid = FH_FSID;
	int inner = NLA_HDRLEN;

	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_CLIENT,
			 domain, strlen(domain) + 1);
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_PATH,
			 path, strlen(path) + 1);
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_EXPIRY,
			 &expiry, sizeof(expiry));
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_ANON_UID,
			 &anon, sizeof(anon));
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_ANON_GID,
			 &anon, sizeof(anon));
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_FLAGS,
			 &flags, sizeof(flags));
	inner = put_attr(attrs, inner, NFSD_A_SVC_EXPORT_FSID,
			 &fsid, sizeof(fsid));
	nest->nla_type = NFSD_A_SVC_EXPORT_REQS_REQUESTS | NLA_F_NESTED;
	nest->nla_len = inner;

	return genl_request(NFSD_CMD_SVC_EXPORT_SET_REQS, attrs,
			    NLA_ALIGN4(inner));
}

/* Push the ip_map and svc_export entries a mountd MOUNT would. */
static int provision(const char *domain, const char *path)
{
	int ret = ip_map_set(domain);

	if (ret)
		return ret;
	return export_set(domain, path);
}

/* Fine-grained control for negative tests: any field can be omitted. */
struct raw_fh_req {
	const char *domain;	/* NULL -> omit NFSD_A_FILEHANDLE_DOMAIN */
	const char *path;	/* NULL -> omit NFSD_A_FILEHANDLE_PATH */
	int emit_maxsize;
	uint32_t maxsize;
};

/* FILEHANDLE_GET into @fh; returns handle length or -errno. */
static int filehandle_get_raw(const struct raw_fh_req *r, char *fh,
			      size_t fhsize)
{
	char attrs[256], rbuf[512];
	const struct nlattr *na;
	int off = 0, ret, len;

	if (r->domain)
		off = put_attr(attrs, off, NFSD_A_FILEHANDLE_DOMAIN,
			       r->domain, strlen(r->domain) + 1);
	if (r->path)
		off = put_attr(attrs, off, NFSD_A_FILEHANDLE_PATH,
			       r->path, strlen(r->path) + 1);
	if (r->emit_maxsize)
		off = put_attr(attrs, off, NFSD_A_FILEHANDLE_MAXSIZE,
			       &r->maxsize, sizeof(r->maxsize));

	ret = genl_request_reply_attrs(NFSD_CMD_FILEHANDLE_GET, attrs, off,
				       rbuf, sizeof(rbuf));
	if (ret <= 0)
		return ret ? ret : -EIO;

	na = genl_find_attr(rbuf, ret, NFSD_A_FILEHANDLE_HANDLE);
	if (!na)
		return -EIO;
	len = na->nla_len - NLA_HDRLEN;
	if (len <= 0 || (size_t)len > fhsize)
		return -EIO;
	memcpy(fh, (const char *)na + NLA_HDRLEN, len);
	return len;
}

static int filehandle_get(const char *domain, const char *path,
			  char *fh, size_t fhsize)
{
	struct raw_fh_req r = { .domain = domain, .path = path,
				.emit_maxsize = 1, .maxsize = FH_MAXSIZE };

	return filehandle_get_raw(&r, fh, fhsize);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Legacy transaction interface: write "domain path maxsize", read "\x<hex>". */
static int proc_filehandle(const char *mnt, const char *domain,
			   const char *path, char *fh, size_t fhsize)
{
	char file[128], req[256], rsp[512];
	int fd, i, n, len;

	snprintf(file, sizeof(file), "%s/filehandle", mnt);
	fd = open(file, O_RDWR);
	if (fd < 0)
		return -errno;
	n = snprintf(req, sizeof(req), "%s %s %u\n", domain, path, FH_MAXSIZE);
	if (write(fd, req, n) != n) {
		n = -errno;
		close(fd);
		return n;
	}
	n = read(fd, rsp, sizeof(rsp) - 1);
	close(fd);
	if (n <= 0)
		return n ? -errno : -EIO;
	rsp[n] = '\0';

	if (n < 2 || rsp[0] != '\\' || rsp[1] != 'x')
		return -EIO;

	len = 0;
	for (i = 2; i + 1 < n && rsp[i] != '\n'; i += 2) {
		int hi = hexval(rsp[i]), lo = hexval(rsp[i + 1]);

		if (hi < 0 || lo < 0)
			return -EIO;
		if ((size_t)len >= fhsize)
			return -EIO;
		fh[len++] = (hi << 4) | lo;
	}
	return len ? len : -EIO;
}

FIXTURE(nfsd_filehandle) {
	char tmpdir[64];
	char mnt[80];
	int mounted;
};

FIXTURE_SETUP(nfsd_filehandle)
{
	if (geteuid() != 0)
		SKIP(return, "must be run as root");
	if (unshare(CLONE_NEWNET | CLONE_NEWNS) < 0)
		SKIP(return, "unshare(NEWNET|NEWNS): %s", strerror(errno));
	if (mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		SKIP(return, "mount(/ private): %s", strerror(errno));

	nfsd_family = genl_resolve_nfsd();
	if (nfsd_family < 0)
		SKIP(return, "nfsd genl family not found (modprobe nfsd?)");
	sunrpc_family = genl_resolve(SUNRPC_FAMILY_NAME,
				     sizeof(SUNRPC_FAMILY_NAME));
	if (sunrpc_family < 0)
		SKIP(return, "sunrpc genl family not found");

	/*
	 * An empty request fails attribute validation when the command
	 * exists and draws -EOPNOTSUPP when the kernel lacks it.
	 */
	if (genl_request(NFSD_CMD_FILEHANDLE_GET, NULL, 0) == -EOPNOTSUPP)
		SKIP(return, "NFSD_CMD_FILEHANDLE_GET not supported");

	strcpy(self->tmpdir, "/tmp/nfsd-fh-XXXXXX");
	if (!mkdtemp(self->tmpdir))
		SKIP(return, "mkdtemp: %s", strerror(errno));
	self->mounted = 0;
}

FIXTURE_TEARDOWN(nfsd_filehandle)
{
	if (self->mounted) {
		umount(self->mnt);
		rmdir(self->mnt);
	}
	rmdir(self->tmpdir);
}

/* Mount this netns's nfsd filesystem for the legacy comparison. */
static int fh_proc_mount(FIXTURE_DATA(nfsd_filehandle) * self)
{
	snprintf(self->mnt, sizeof(self->mnt), "%s/mnt", self->tmpdir);
	if (mkdir(self->mnt, 0700) < 0)
		return -errno;
	if (mount("nfsd", self->mnt, "nfsd", 0, NULL) < 0) {
		int err = -errno;

		rmdir(self->mnt);
		return err;
	}
	self->mounted = 1;
	return 0;
}

TEST_F(nfsd_filehandle, val_missing_domain)
{
	struct raw_fh_req r = { .path = self->tmpdir,
				.emit_maxsize = 1, .maxsize = FH_MAXSIZE };
	char fh[FH_MAXSIZE];

	EXPECT_EQ(-EINVAL, filehandle_get_raw(&r, fh, sizeof(fh)));
}

TEST_F(nfsd_filehandle, val_missing_path)
{
	struct raw_fh_req r = { .domain = FH_DOMAIN,
				.emit_maxsize = 1, .maxsize = FH_MAXSIZE };
	char fh[FH_MAXSIZE];

	EXPECT_EQ(-EINVAL, filehandle_get_raw(&r, fh, sizeof(fh)));
}

TEST_F(nfsd_filehandle, val_missing_maxsize)
{
	struct raw_fh_req r = { .domain = FH_DOMAIN, .path = self->tmpdir };
	char fh[FH_MAXSIZE];

	EXPECT_EQ(-EINVAL, filehandle_get_raw(&r, fh, sizeof(fh)));
}

TEST_F(nfsd_filehandle, val_maxsize_small)
{
	struct raw_fh_req r = { .domain = FH_DOMAIN, .path = self->tmpdir,
				.emit_maxsize = 1, .maxsize = 31 };
	char fh[FH_MAXSIZE];

	EXPECT_EQ(-ERANGE, filehandle_get_raw(&r, fh, sizeof(fh)));
	r.maxsize = 0;
	EXPECT_EQ(-ERANGE, filehandle_get_raw(&r, fh, sizeof(fh)));
}

TEST_F(nfsd_filehandle, val_maxsize_large)
{
	struct raw_fh_req r = { .domain = FH_DOMAIN, .path = self->tmpdir,
				.emit_maxsize = 1, .maxsize = 65 };
	char fh[FH_MAXSIZE];

	EXPECT_EQ(-ERANGE, filehandle_get_raw(&r, fh, sizeof(fh)));
}

TEST_F(nfsd_filehandle, val_no_export)
{
	char fh[FH_MAXSIZE];

	/* Well-formed but nothing was ever pushed for this domain. */
	EXPECT_LT(filehandle_get("nfsd-fh-noexport", self->tmpdir,
				 fh, sizeof(fh)), 0);
}

TEST_F(nfsd_filehandle, func_roundtrip)
{
	char fh1[FH_MAXSIZE], fh2[FH_MAXSIZE];
	int len1, len2;

	ASSERT_EQ(0, provision(FH_DOMAIN, self->tmpdir));

	len1 = filehandle_get(FH_DOMAIN, self->tmpdir, fh1, sizeof(fh1));
	ASSERT_GT(len1, 0);
	EXPECT_LE(len1, FH_MAXSIZE);

	/* The same lookup must compose the same handle. */
	len2 = filehandle_get(FH_DOMAIN, self->tmpdir, fh2, sizeof(fh2));
	EXPECT_EQ(len1, len2);
	EXPECT_EQ(0, memcmp(fh1, fh2, len1));
}

TEST_F(nfsd_filehandle, func_matches_proc)
{
	char fh_nl[FH_MAXSIZE], fh_proc[FH_MAXSIZE];
	int len_nl, len_proc, ret;

	ASSERT_EQ(0, provision(FH_DOMAIN, self->tmpdir));

	ret = fh_proc_mount(self);
	if (ret)
		SKIP(return, "mount(nfsd): %s", strerror(-ret));

	len_proc = proc_filehandle(self->mnt, FH_DOMAIN, self->tmpdir,
				   fh_proc, sizeof(fh_proc));
	ASSERT_GT(len_proc, 0);

	len_nl = filehandle_get(FH_DOMAIN, self->tmpdir, fh_nl, sizeof(fh_nl));
	ASSERT_GT(len_nl, 0);

	EXPECT_EQ(len_proc, len_nl);
	EXPECT_EQ(0, memcmp(fh_proc, fh_nl, len_nl));
}

TEST_HARNESS_MAIN
