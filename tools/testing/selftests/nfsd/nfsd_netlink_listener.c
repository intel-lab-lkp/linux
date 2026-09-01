// SPDX-License-Identifier: GPL-2.0
/*
 * Regression tests for the NFSD generic-netlink listener interface
 * (NFSD_CMD_LISTENER_SET / NFSD_CMD_LISTENER_GET).
 *
 * These cover the request validation that nfsd_nl_validate_listeners() does
 * before nfsd_mutex is taken: bad or absent transport name, missing address,
 * truncated or unsupported sockaddr, oversized list. None of them reach
 * nfsd_create_serv(), so nothing here creates a serv or talks to rpcbind.
 *
 * Each test runs in its own private net + mount namespace (unshare in
 * FIXTURE_SETUP). /run is masked there: a pathname AF_LOCAL connect is not
 * scoped by the network namespace, since unix_find_bsd() resolves by inode
 * and takes no struct net, so the kernel's rpcbind client would otherwise be
 * able to reach the rpcbind running on the host.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nfsd_netlink.h>

#include "../kselftest_harness.h"

#define NLA_ALIGN4(len)			(((len) + 3) & ~3)
#define TEST_PORT			20049
#define MAX_LISTENERS			8
#define RECV_TIMEO_SEC			30

static int nfsd_family;			/* set per-test in FIXTURE_SETUP */

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

/* ------------------- minimal generic-netlink plumbing ------------------- */

static int genl_open(void)
{
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	struct timeval tv = { .tv_sec = RECV_TIMEO_SEC };
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);

	if (fd < 0)
		die("socket(NETLINK_GENERIC)");
	if (bind(fd, (void *)&sa, sizeof(sa)) < 0)
		die("bind(netlink)");
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

/* Append an attribute at @off; return the new (aligned) offset. */
static int put_attr(char *buf, int off, uint16_t type,
		    const void *data, int len)
{
	struct nlattr *na = (void *)(buf + off);

	na->nla_type = type;
	na->nla_len = NLA_HDRLEN + len;
	if (len)
		memcpy(buf + off + NLA_HDRLEN, data, len);
	return off + NLA_ALIGN4(NLA_HDRLEN + len);
}

/* Build a genl message header into @buf; return the offset past it. */
static int genl_hdr(char *buf, uint16_t type, uint16_t flags, uint8_t cmd)
{
	struct nlmsghdr *nlh = (void *)buf;
	struct genlmsghdr *gnl = (void *)(buf + NLMSG_HDRLEN);

	memset(buf, 0, NLMSG_HDRLEN + GENL_HDRLEN);
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_seq = 1;
	gnl->cmd = cmd;
	gnl->version = 1;
	return NLMSG_HDRLEN + GENL_HDRLEN;
}

/* Send an nfsd command with an ACK; return the ACK errno (<= 0). */
static int genl_request(uint8_t cmd, const char *attrs, int attrs_len)
{
	char buf[1 << 20], rbuf[4096];
	struct nlmsghdr *nlh = (void *)buf;
	int fd = genl_open();
	int off, n, ret;

	off = genl_hdr(buf, nfsd_family, NLM_F_REQUEST | NLM_F_ACK, cmd);
	if (attrs_len) {
		memcpy(buf + off, attrs, attrs_len);
		off += attrs_len;
	}
	nlh->nlmsg_len = off;

	if (send(fd, buf, off, 0) < 0)
		die("send(genl)");

	n = recv(fd, rbuf, sizeof(rbuf), 0);
	if (n < 0)
		ret = (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
	else if (((struct nlmsghdr *)rbuf)->nlmsg_type == NLMSG_ERROR)
		ret = ((struct nlmsgerr *)NLMSG_DATA(rbuf))->error;
	else
		ret = 0;
	close(fd);
	return ret;
}

/* Send a command and return the full reply message; -errno on failure. */
static int genl_request_reply(uint8_t cmd, char *rbuf, size_t rlen)
{
	char buf[256];
	struct nlmsghdr *nlh = (void *)buf;
	int fd = genl_open();
	int off, n, ret;

	off = genl_hdr(buf, nfsd_family, NLM_F_REQUEST, cmd);
	nlh->nlmsg_len = off;

	if (send(fd, buf, off, 0) < 0)
		die("send(genl reply)");

	n = recv(fd, rbuf, rlen, 0);
	if (n < 0)
		ret = (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT : -errno;
	else if (((struct nlmsghdr *)rbuf)->nlmsg_type == NLMSG_ERROR)
		ret = ((struct nlmsgerr *)NLMSG_DATA(rbuf))->error;
	else
		ret = n;
	close(fd);
	return ret;
}

/* Resolve the "nfsd" genl family id; -1 if not registered. */
static int genl_resolve_nfsd(void)
{
	char buf[1024], rbuf[4096];
	struct nlmsghdr *nlh = (void *)buf;
	struct nlmsghdr *rh = (void *)rbuf;
	struct nlattr *na;
	int fd, off, left, id = -1;

	fd = genl_open();
	off = genl_hdr(buf, GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY);
	off = put_attr(buf, off, CTRL_ATTR_FAMILY_NAME,
		       NFSD_FAMILY_NAME, sizeof(NFSD_FAMILY_NAME));
	nlh->nlmsg_len = off;

	if (send(fd, buf, off, 0) < 0)
		die("send(GETFAMILY)");
	if (recv(fd, rbuf, sizeof(rbuf), 0) < 0)
		die("recv(GETFAMILY)");
	close(fd);

	if (rh->nlmsg_type == NLMSG_ERROR)
		return -1;

	na = (void *)((char *)NLMSG_DATA(rh) + GENL_HDRLEN);
	left = rh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
	while (left >= (int)NLA_HDRLEN) {
		if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
			id = *(uint16_t *)((char *)na + NLA_HDRLEN);
			break;
		}
		left -= NLA_ALIGN4(na->nla_len);
		na = (void *)((char *)na + NLA_ALIGN4(na->nla_len));
	}
	return id;
}

/* ------------------- listener request builders ------------------- */

/* Fine-grained control for negative tests: any field can be omitted/malformed. */
struct raw_listener {
	const char *xprt;	/* NULL -> omit NFSD_A_SOCK_TRANSPORT_NAME */
	int emit_addr;		/* 0 -> omit NFSD_A_SOCK_ADDR */
	const void *addr;
	int addr_len;		/* bytes to emit for NFSD_A_SOCK_ADDR */
};

static int put_raw_listener(char *buf, int off, const struct raw_listener *r)
{
	struct nlattr *nest = (void *)(buf + off);
	int inner = off + NLA_HDRLEN;

	if (r->emit_addr)
		inner = put_attr(buf, inner, NFSD_A_SOCK_ADDR, r->addr, r->addr_len);
	if (r->xprt)
		inner = put_attr(buf, inner, NFSD_A_SOCK_TRANSPORT_NAME,
				 r->xprt, strlen(r->xprt) + 1);
	nest->nla_type = NFSD_A_SERVER_SOCK_ADDR | NLA_F_NESTED;
	nest->nla_len = inner - off;
	return off + NLA_ALIGN4(nest->nla_len);
}

/* Well-formed loopback listener for @family (AF_INET or AF_INET6). */
static int put_listener_af(char *buf, int off, const char *xprt, int family,
			   uint16_t port)
{
	struct sockaddr_storage ss = {0};
	struct raw_listener r = { .xprt = xprt, .emit_addr = 1, .addr = &ss };

	if (family == AF_INET6) {
		struct sockaddr_in6 *s6 = (void *)&ss;

		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		s6->sin6_addr = in6addr_loopback;
		r.addr_len = sizeof(*s6);
	} else {
		struct sockaddr_in *s4 = (void *)&ss;

		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		s4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		r.addr_len = sizeof(*s4);
	}
	return put_raw_listener(buf, off, &r);
}

static int put_listener(char *buf, int off, const char *xprt, uint16_t port)
{
	return put_listener_af(buf, off, xprt, AF_INET, port);
}

/* ------------------- LISTENER_GET parsing ------------------- */

struct listener_ent {
	char xprt[16];
	int family;
	uint16_t port;
	struct in_addr a4;
	struct in6_addr a6;
};

static int parse_listener_get(const char *rbuf, int len,
			      struct listener_ent *out, int max)
{
	const struct nlmsghdr *nlh = (const void *)rbuf;
	const struct nlattr *na;
	int left, count = 0;

	(void)len;
	na = (const void *)(rbuf + NLMSG_HDRLEN + GENL_HDRLEN);
	left = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

	while (left >= (int)NLA_HDRLEN) {
		int alen = na->nla_len;

		if ((na->nla_type & NLA_TYPE_MASK) == NFSD_A_SERVER_SOCK_ADDR &&
		    count < max) {
			const struct nlattr *in = (const void *)((char *)na + NLA_HDRLEN);
			int ileft = alen - NLA_HDRLEN;
			struct listener_ent *e = &out[count];

			memset(e, 0, sizeof(*e));
			while (ileft >= (int)NLA_HDRLEN) {
				const void *d = (const char *)in + NLA_HDRLEN;
				int t = in->nla_type & NLA_TYPE_MASK;

				if (t == NFSD_A_SOCK_TRANSPORT_NAME) {
					strncpy(e->xprt, d, sizeof(e->xprt) - 1);
				} else if (t == NFSD_A_SOCK_ADDR) {
					const struct sockaddr_storage *ss = d;

					e->family = ss->ss_family;
					if (ss->ss_family == AF_INET) {
						const struct sockaddr_in *s = d;

						e->a4 = s->sin_addr;
						e->port = ntohs(s->sin_port);
					} else if (ss->ss_family == AF_INET6) {
						const struct sockaddr_in6 *s = d;

						e->a6 = s->sin6_addr;
						e->port = ntohs(s->sin6_port);
					}
				}
				ileft -= NLA_ALIGN4(in->nla_len);
				in = (const void *)((char *)in + NLA_ALIGN4(in->nla_len));
			}
			count++;
		}
		left -= NLA_ALIGN4(alen);
		na = (const void *)((char *)na + NLA_ALIGN4(alen));
	}
	return count;
}

/* ------------------- convenience wrappers ------------------- */

static int listener_set(const char *attrs, int len)
{
	return genl_request(NFSD_CMD_LISTENER_SET, attrs, len);
}

/* Fetch the current listeners; returns count (>=0) or -errno. */
static int listener_get(struct listener_ent *out, int max)
{
	char rbuf[8192];
	int n = genl_request_reply(NFSD_CMD_LISTENER_GET, rbuf, sizeof(rbuf));

	if (n < 0)
		return n;
	return parse_listener_get(rbuf, n, out, max);
}

/* --------------------------- fixture --------------------------- */

FIXTURE(nfsd_listener) {
	int placeholder;
};

FIXTURE_SETUP(nfsd_listener)
{
	struct ifreq ifr = {0};
	struct stat st;
	int s;

	if (geteuid() != 0)
		SKIP(return, "must be run as root");
	if (unshare(CLONE_NEWNET | CLONE_NEWNS) < 0)
		SKIP(return, "unshare(NEWNET|NEWNS): %s", strerror(errno));
	if (mount("", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
		SKIP(return, "mount(/ private): %s", strerror(errno));

	/*
	 * Keep the kernel's rpcbind client inside this namespace. The
	 * abstract socket it tries first is per-netns, but the
	 * "/var/run/rpcbind.sock" fallback is not, so hide the path.
	 */
	if (mount("tmpfs", "/run", "tmpfs", 0, NULL) < 0)
		SKIP(return, "mount(tmpfs on /run): %s", strerror(errno));
	if (lstat("/var/run", &st) == 0 && S_ISDIR(st.st_mode) &&
	    mount("tmpfs", "/var/run", "tmpfs", 0, NULL) < 0)
		SKIP(return, "mount(tmpfs on /var/run): %s", strerror(errno));

	/*
	 * Bring loopback up so listener binds (127.0.0.1 / ::1) work. Root
	 * without CAP_NET_ADMIN in this netns gets -EPERM here, so skip.
	 */
	s = socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(s, 0);
	strcpy(ifr.ifr_name, "lo");
	if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
		close(s);
		SKIP(return, "SIOCGIFFLAGS(lo): %s", strerror(errno));
	}
	ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
	if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) {
		close(s);
		SKIP(return, "SIOCSIFFLAGS(lo): %s", strerror(errno));
	}
	close(s);

	nfsd_family = genl_resolve_nfsd();
	if (nfsd_family < 0)
		SKIP(return, "nfsd genl family not found (modprobe nfsd?)");
}

FIXTURE_TEARDOWN(nfsd_listener)
{
}

/* ===================== validation / negative ===================== */

TEST_F(nfsd_listener, val_too_many)
{
	static char attrs[1 << 20];
	int i, off = 0;

	for (i = 0; i < 1025; i++)		/* > NFSD_NL_LISTENER_MAX (1024) */
		off = put_listener(attrs, off, "udp", TEST_PORT);
	EXPECT_EQ(-E2BIG, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_missing_addr)
{
	char attrs[64];
	struct raw_listener r = { .xprt = "tcp", .emit_addr = 0 };
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EINVAL, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_missing_transport)
{
	struct sockaddr_in s4 = { .sin_family = AF_INET, .sin_port = htons(TEST_PORT) };
	struct raw_listener r = { .xprt = NULL, .emit_addr = 1,
				  .addr = &s4, .addr_len = sizeof(s4) };
	char attrs[64];
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EINVAL, listener_set(attrs, off));
}

/*
 * A name matching no transport class must be refused before nfsd_mutex is
 * taken, so it never reaches svc_xprt_create_from_sa() and its
 * request_module("svc%s", name) upcall.
 */
TEST_F(nfsd_listener, val_bad_transport)
{
	char attrs[64];
	int off = put_listener(attrs, 0, "bogus_xprt", TEST_PORT);

	EXPECT_EQ(-EPROTONOSUPPORT, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_addr_too_short)
{
	unsigned char tiny = 0;
	struct raw_listener r = { .xprt = "tcp", .emit_addr = 1,
				  .addr = &tiny, .addr_len = 1 };
	char attrs[64];
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EINVAL, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_inet_short)
{
	struct sockaddr_in s4 = { .sin_family = AF_INET, .sin_port = htons(TEST_PORT) };
	struct raw_listener r = { .xprt = "tcp", .emit_addr = 1, .addr = &s4,
				  .addr_len = sizeof(sa_family_t) + 2 };
	char attrs[64];
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EINVAL, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_inet6_short)
{
	struct sockaddr_in6 s6 = { .sin6_family = AF_INET6, .sin6_port = htons(TEST_PORT) };
	struct raw_listener r = { .xprt = "tcp", .emit_addr = 1, .addr = &s6,
				  .addr_len = sizeof(struct sockaddr_in) };
	char attrs[64];
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EINVAL, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_bad_family)
{
	struct sockaddr_storage ss = { .ss_family = AF_UNIX };
	struct raw_listener r = { .xprt = "tcp", .emit_addr = 1, .addr = &ss,
				  .addr_len = sizeof(struct sockaddr_in) };
	char attrs[64];
	int off = put_raw_listener(attrs, 0, &r);

	EXPECT_EQ(-EAFNOSUPPORT, listener_set(attrs, off));
}

TEST_F(nfsd_listener, val_second_entry_bad)
{
	struct sockaddr_storage ss = { .ss_family = AF_UNIX };
	struct raw_listener bad = { .xprt = "tcp", .emit_addr = 1, .addr = &ss,
				    .addr_len = sizeof(struct sockaddr_in) };
	char attrs[128];
	int off = put_listener(attrs, 0, "tcp", TEST_PORT);

	off = put_raw_listener(attrs, off, &bad);
	/* The whole request is rejected during validation; nothing applied. */
	EXPECT_EQ(-EAFNOSUPPORT, listener_set(attrs, off));
}

/* LISTENER_GET with no serv in this netns returns an empty list. */
TEST_F(nfsd_listener, func_get_empty)
{
	struct listener_ent got[MAX_LISTENERS];

	EXPECT_EQ(0, listener_get(got, MAX_LISTENERS));
}

TEST_HARNESS_MAIN
