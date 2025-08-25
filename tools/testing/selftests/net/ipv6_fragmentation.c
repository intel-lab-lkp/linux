// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Brett A C Sheffield <bacs@librecast.net>
 *
 * Kernel selftest for the IPv6 fragmentation regression which affected
 * stable kernels:
 *
 *   https://lore.kernel.org/stable/aElivdUXqd1OqgMY@karahi.gladserv.com
 *
 * Commit:
 *   a18dfa9925b9 ("ipv6: save dontfrag in cork")
 * was backported to stable without some prerequisite commits.
 *
 * This caused a regression when sending IPv6 UDP packets by preventing
 * fragmentation and instead returning -1 (EMSGSIZE).
 *
 * This selftest demonstrates the issue. sendmsg returns correctly (8192)
 * on a working kernel, and returns -1 (EMSGSIZE) when the regression is
 * present.
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

#define MTU 1500
#define LARGER_THAN_MTU 8192

/* ensure MTU is smaller than what we plan to send */
static int set_mtu(int ctl, char *ifname, struct ifreq *ifr)
{
	ifr->ifr_mtu = MTU;
	return ioctl(ctl, SIOCSIFMTU, ifr);
}

/* bring up interface */
static int interface_up(int ctl, char *ifname, struct ifreq *ifr)
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
static int create_interface(int ctl, char *ifname, struct ifreq *ifr)
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

/* we need to set MTU, so do this in a namespace to play nicely */
static int create_namespace(void)
{
	const char *netns_path = "/proc/self/ns/net";
	int fd;

	if (unshare(CLONE_NEWNET) != 0) {
		perror("unshare");
		return -1;
	}

	fd = open(netns_path, O_RDONLY);
	if (fd == -1) {
		perror("open");
		return -1;
	}

	if (setns(fd, CLONE_NEWNET)) {
		perror("setns");
		return -1;
	}

	return 0;
}

static int setup(void)
{
	struct ifreq ifr = {0};
	char ifname[IFNAMSIZ];
	int fd = -1;
	int ctl;

	if (create_namespace() == -1)
		return -1;

	ctl = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (ctl == -1)
		return -1;

	memset(ifname, 0, sizeof(ifname));
	fd = create_interface(ctl, ifname, &ifr);
	if (fd == -1)
		goto err_close_ctl;
	if (disable_dad(ifname) == -1)
		goto err_close_fd;
	if (interface_up(ctl, ifname, &ifr) == -1)
		goto err_close_fd;
	if (set_mtu(ctl, ifname, &ifr) == -1)
		goto err_close_fd;
	usleep(10000); /* give interface a moment to wake up */
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
	/* address doesn't matter, use an IPv6 multicast address for simplicity */
	struct in6_addr addr = {
		.s6_addr[0] = 0xff, /* multicast */
		.s6_addr[1] = 0x12, /* set flags (T, link-local) */
	};
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_addr = addr,
		.sin6_port = 4242
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
	int s;

	printf("Testing IPv6 fragmentation\n");
	ns_fd = setup();
	if (ns_fd == -1)
		return 1;
	s = socket(AF_INET6, SOCK_DGRAM, 0);
	msg.msg_name = (struct sockaddr *)&sa;
	msg.msg_namelen = sizeof(sa);
	rc = sendmsg(s, &msg, 0);
	if (rc == -1) {
		perror("send");
		return 1;
	} else if (rc != LARGER_THAN_MTU) {
		fprintf(stderr, "send() returned %zi\n", rc);
		return 1;
	}
	close(s);
	close(ns_fd);

	return 0;
}
