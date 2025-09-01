// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Brett A C Sheffield <bacs@librecast.net>
 *
 * Kernel selftest for the IPv6 fragmentation regression which affected stable
 * kernels:
 *
 *   https://lore.kernel.org/stable/aElivdUXqd1OqgMY@karahi.gladserv.com
 *
 * Commit: a18dfa9925b9 ("ipv6: save dontfrag in cork") was backported to stable
 * without some prerequisite commits.
 *
 * This caused a regression when sending IPv6 UDP packets by preventing
 * fragmentation and instead returning -1 (EMSGSIZE).
 *
 * This selftest demonstrates the issue by sending an IPv6 UDP packet to
 * localhost (::1) on the loopback interface from the autoconfigured link-local
 * address.
 *
 * sendmsg(2) returns bytes sent correctly on a working kernel, and returns -1
 * (EMSGSIZE) when the regression is present.
 *
 * The regression was not present in the mainline kernel, but add this test to
 * catch similar breakage in future.
 */

#define _GNU_SOURCE

#include <error.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../kselftest.h"

#define MTU 1500
#define LARGER_THAN_MTU 8192

/* bring up interface */
static int interface_up(int ctl, struct ifreq *ifr)
{
	if (ioctl(ctl, SIOCGIFFLAGS, ifr) == -1)
		error(KSFT_FAIL, errno, "ioctl SIOCGIFFLAGS");
	ifr->ifr_flags = ifr->ifr_flags | IFF_UP;
	return ioctl(ctl, SIOCSIFFLAGS, ifr);
}

/* no need to wait for DAD in our namespace */
static int disable_dad(char *ifname)
{
	char sysvar[] = "/proc/sys/net/ipv6/conf/%s/accept_dad";
	char fname[IFNAMSIZ + sizeof(sysvar)];
	int fd;

	snprintf(fname, sizeof(fname), sysvar, ifname);
	fd = open(fname, O_WRONLY);
	if (fd == -1)
		error(KSFT_FAIL, errno, "open accept_dad");
	if (write(fd, "0", 1) != 1)
		error(KSFT_FAIL, errno, "write accept_dad");

	return close(fd);
}

static int setup(void)
{
	struct ifreq ifr = {
		.ifr_name = "lo"
	};
	int fd = -1;
	int ctl;

	/* we need to set MTU, so do this in a namespace to play nicely */
	if (unshare(CLONE_NEWNET) == -1)
		error(KSFT_FAIL, errno, "unshare");

	ctl = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (ctl == -1)
		error(KSFT_FAIL, errno, "socket");

	/* ensure MTU is smaller than what we plan to send */
	ifr.ifr_mtu = MTU;
	if (ioctl(ctl, SIOCSIFMTU, &ifr) == -1)
		error(KSFT_FAIL, errno, "ioctl: set MTU");

	disable_dad("lo");
	interface_up(ctl, &ifr);

	close(ctl);
	return fd;
}

int main(void)
{
	struct in6_addr addr = {
		.s6_addr[15] = 0x01, /* ::1 */
	};
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_addr = addr,
		.sin6_port = 9      /* port 9/udp (DISCARD) */
	};
	char buf[LARGER_THAN_MTU] = {0};
	struct iovec iov = { .iov_base = buf, .iov_len = sizeof(buf)};
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_name = (struct sockaddr *)&sa,
		.msg_namelen = sizeof(sa),
	};
	ssize_t rc;
	int ns_fd;
	int err = KSFT_FAIL;
	int s;

	printf("Testing IPv6 fragmentation\n");
	ns_fd = setup();
	s = socket(AF_INET6, SOCK_DGRAM, 0);
send_again:
	rc = sendmsg(s, &msg, 0);
	if (rc == -1) {
		/* if interface wasn't ready, try again */
		if (errno == EADDRNOTAVAIL) {
			usleep(1000);
			goto send_again;
		}
		printf("[FAIL] sendmsg: %s\n", strerror(errno));
	} else if (rc != LARGER_THAN_MTU) {
		printf("[FAIL] sendmsg() returned %zi, expected %i\n", rc, LARGER_THAN_MTU);
	} else {
		printf("[PASS] sendmsg() returned %zi\n", rc);
		err = KSFT_PASS;
	}
	close(s);
	close(ns_fd);
	return err;
}
