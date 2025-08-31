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
 * This selftest demonstrates the issue by sending an IPv6 UDP packet from
 * the autoconfigured link-local address to an arbritrary multicast group.
 *
 * sendmsg(2) returns bytes sent correctly on a working kernel, and returns -1
 * (EMSGSIZE) when the regression is present.
 *
 * The regression was not present in the mainline kernel, but add this test to
 * catch similar breakage in future.
 */

#define _GNU_SOURCE

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

/* ensure MTU is smaller than what we plan to send */
static int set_mtu(int ctl, struct ifreq *ifr)
{
	ifr->ifr_mtu = MTU;
	return ioctl(ctl, SIOCSIFMTU, ifr);
}

/* bring up interface */
static int interface_up(int ctl, struct ifreq *ifr)
{
	if (ioctl(ctl, SIOCGIFFLAGS, ifr) == -1) {
		perror("ioctl SIOCGIFFLAGS");
		return -1;
	}
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
	if (fd == -1) {
		perror("open accept_dad");
		return -1;
	}
	if (write(fd, "0", 1) != 1) {
		perror("write");
		return -1;
	}
	return close(fd);
}

/* create TAP interface that will be deleted when this process exits */
static int create_interface(char *ifname, struct ifreq *ifr)
{
	int fd;

	fd = open("/dev/net/tun", O_RDWR);
	if (fd == -1) {
		perror("open tun");
		return -1;
	}

	ifr->ifr_flags = IFF_TAP | IFF_NO_PI;
	if (ioctl(fd, TUNSETIFF, (void *)ifr) == -1) {
		close(fd);
		perror("ioctl: TUNSETIFF");
		return -1;
	}
	strcpy(ifname, ifr->ifr_name);

	return fd;
}

static int setup(void)
{
	struct ifreq ifr = {0};
	char ifname[IFNAMSIZ];
	int fd = -1;
	int ctl;

	/* we need to set MTU, so do this in a namespace to play nicely */
	if (unshare(CLONE_NEWNET) == -1)
		return -1;

	ctl = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (ctl == -1)
		return -1;

	memset(ifname, 0, sizeof(ifname));
	fd = create_interface(ifname, &ifr);
	if (fd == -1)
		goto err_close_ctl;
	if (disable_dad(ifname) == -1)
		goto err_close_fd;
	if (interface_up(ctl, &ifr) == -1)
		goto err_close_fd;
	if (set_mtu(ctl, &ifr) == -1)
		goto err_close_fd;
	goto err_close_ctl;
err_close_fd:
	close(fd);
	fd = -1;
err_close_ctl:
	close(ctl);
	return fd;
}

int main(void)
{
	/* destination doesn't matter, use an IPv6 link-local multicast group */
	struct in6_addr addr = {
		.s6_addr[0] = 0xff, /* multicast */
		.s6_addr[1] = 0x12, /* set flags (T, link-local) */
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
	if (ns_fd == -1) {
		printf("[FAIL] test setup failed\n");
		return KSFT_FAIL;
	}
	s = socket(AF_INET6, SOCK_DGRAM, 0);
send_again:
	rc = sendmsg(s, &msg, 0);
	if (rc == -1) {
		/* if interface wasn't ready, try again */
		if (errno == EADDRNOTAVAIL)
			goto send_again;
		printf("[FAIL] sendmsg: %s\n", strerror(errno));
		goto err_close_socket;
	} else if (rc != LARGER_THAN_MTU) {
		printf("[FAIL] sendmsg() returned %zi\n", rc);
		goto err_close_socket;
	}
	printf("[PASS] sendmsg() returned %zi\n", rc);
	err = KSFT_PASS;

err_close_socket:
	close(s);
	close(ns_fd);
	return err;
}
