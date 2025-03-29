// SPDX-License-Identifier: GPL-2.0
/* Copyright Amazon.com Inc. or its affiliates. */

#include <limits.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../kselftest_harness.h"

static int udp_max_pages;

static int udp_parse_pages(struct __test_metadata *_metadata,
			   char *line, int *pages)
{
	int ret, unused;

	if (strncmp(line, "UDP:", 4))
		return -1;

	ret = sscanf(line + 4, " inuse %d mem %d", &unused, pages);
	ASSERT_EQ(2, ret);

	return 0;
}

FIXTURE(so_rcvbuf)
{
	union {
		struct sockaddr addr;
		struct sockaddr_in addr4;
		struct sockaddr_in6 addr6;
	};
	socklen_t addrlen;
	int server;
	int client;
};

FIXTURE_VARIANT(so_rcvbuf)
{
	int family;
	int type;
	int protocol;
	int *max_pages;
	int (*parse_pages)(struct __test_metadata *_metadata,
			   char *line, int *pages);
};

FIXTURE_VARIANT_ADD(so_rcvbuf, udp_ipv4)
{
	.family = AF_INET,
	.type = SOCK_DGRAM,
	.protocol = 0,
	.max_pages = &udp_max_pages,
	.parse_pages = udp_parse_pages,
};

FIXTURE_VARIANT_ADD(so_rcvbuf, udp_ipv6)
{
	.family = AF_INET6,
	.type = SOCK_DGRAM,
	.protocol = 0,
	.max_pages = &udp_max_pages,
	.parse_pages = udp_parse_pages,
};

static int get_page_shift(void)
{
	int page_size = getpagesize();
	int page_shift = 0;

	while (page_size > 1) {
		page_size >>= 1;
		page_shift++;
	}

	return page_shift;
}

FIXTURE_SETUP(so_rcvbuf)
{
	self->addr.sa_family = variant->family;

	if (variant->family == AF_INET)
		self->addrlen = sizeof(struct sockaddr_in);
	else
		self->addrlen = sizeof(struct sockaddr_in6);

	udp_max_pages = (INT_MAX + 1L) >> get_page_shift();
}

FIXTURE_TEARDOWN(so_rcvbuf)
{
}

static void create_socketpair(struct __test_metadata *_metadata,
			      FIXTURE_DATA(so_rcvbuf) *self,
			      const FIXTURE_VARIANT(so_rcvbuf) *variant)
{
	int ret;

	self->server = socket(variant->family, variant->type, variant->protocol);
	ASSERT_NE(self->server, -1);

	self->client = socket(variant->family, variant->type, variant->protocol);
	ASSERT_NE(self->client, -1);

	ret = bind(self->server, &self->addr, self->addrlen);
	ASSERT_EQ(ret, 0);

	ret = getsockname(self->server, &self->addr, &self->addrlen);
	ASSERT_EQ(ret, 0);

	ret = connect(self->client, &self->addr, self->addrlen);
	ASSERT_EQ(ret, 0);
}

static int get_prot_pages(struct __test_metadata *_metadata,
			  const FIXTURE_VARIANT(so_rcvbuf) *variant)
{
	char *line = NULL;
	size_t unused;
	int pages = 0;
	FILE *f;

	f = fopen("/proc/net/sockstat", "r");
	ASSERT_NE(NULL, f);

	while (getline(&line, &unused, f) != -1)
		if (!variant->parse_pages(_metadata, line, &pages))
			break;

	free(line);
	fclose(f);

	return pages;
}

TEST_F(so_rcvbuf, rmem_max)
{
	int ret, i, pages;
	char buf[16] = {};

	create_socketpair(_metadata, self, variant);

	ret = setsockopt(self->server, SOL_SOCKET, SO_RCVBUFFORCE,
			 &(int){INT_MAX}, sizeof(int));
	ASSERT_EQ(ret, 0);

	pages = get_prot_pages(_metadata, variant);
	ASSERT_EQ(pages, 0);

	for (i = 1; ; i++) {
		ret = send(self->client, buf, sizeof(buf), 0);
		ASSERT_EQ(ret, sizeof(buf));

		/* Make sure we don't stop at pages == (INT_MAX >> PAGE_SHIFT)
		 * in case ASSERT_LE() should fail.
		 */
		if (i % 10000 == 0) {
			pages = get_prot_pages(_metadata, variant);

			/* sk_rmem_alloc wrapped around by >PAGE_SIZE ? */
			ASSERT_LE(pages, *variant->max_pages);

			if (pages == *variant->max_pages)
				break;
		}
	}

	TH_LOG("max_pages: %d", pages);

	close(self->client);
	close(self->server);

	/* Give RCU a chance to call udp_destruct_common() */
	for (i = 0; i < 30; i++) {
		sleep(1);

		pages = get_prot_pages(_metadata, variant);
		if (!pages)
			break;
	}

	ASSERT_EQ(pages, 0);
}

TEST_HARNESS_MAIN
