// SPDX-License-Identifier: GPL-2.0
/*
 * This test creates a new netdevsim device and then sends
 * an IPv6 multipath netlink message to it and the loopback
 * interface.
 *
 * This triggers an edge case where the routing table is
 * constructed with an entry where gw_family = AF_UNSPEC.
 * If not caught, this causes an unexpected nsiblings count
 * in netdevsim/fib.c: nsim_fib6_event_init(), raising a
 * warning.
 *
 * NOTE: The warning in question is raised by WARN_ON_ONCE.
 * Therefore, this test reports a false negative if the
 * warning has already been triggered.
 *
 */

#include <arpa/inet.h>
#include <bits/types/struct_iovec.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>
#include <net/if.h>

#define RTF_UP 0x0001 // route usable
#define RTF_HOST 0x0004 // host entry (net otherwise)

#define NSIM_PORTS 1
#define NETDEVSIM_DEV_DIR "/sys/bus/netdevsim/devices"
#define NSIM_DEV_DIR_BUFFER_SIZE 128
#define LO_DEV "lo"

#define BUFSIZE 4096
#define DST_PREFIX "2001:db8::"
#define GW1 "fe80::1"
#define GW2 "::1"

#define PID_LEN 16

int get_free_idx(void)
{
	int idx = 0;
	int tmp = 0;
	DIR *nsim_dir = opendir(NETDEVSIM_DEV_DIR);
	struct dirent *entry = NULL;

	if (nsim_dir == NULL) {
		fprintf(stderr, "Unable to open nsim directory\n");
		return -1;
	}

	do {
		entry = readdir(nsim_dir);
		if (entry != NULL &&
		    sscanf(entry->d_name, "netdevsim%d", &tmp) > 0) {
			if (tmp >= idx)
				idx = tmp + 1;
		}
	} while (entry != NULL);

	closedir(nsim_dir);
	return idx;
}

int create_netdevsim_device(int id, int num_ports)
{
	const char *path = "/sys/bus/netdevsim/new_device";
	char buffer[64];
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "Failed to open new_device\n");
		return -1;
	}

	snprintf(buffer, sizeof(buffer), "%d %d", id, num_ports);
	if (write(fd, buffer, strlen(buffer)) < 0) {
		fprintf(stderr, "Failed to write to new_device\n");
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

int ensure_nsim_dev_exists(void)
{
	int ret;
	int nsim_idx;

	nsim_idx = get_free_idx();
	ret = create_netdevsim_device(nsim_idx, NSIM_PORTS);
	if (ret != 0) {
		fprintf(stderr, "Failed to create nsim device\n");
		return -1;
	}

	return nsim_idx;
}

char *get_nsim_dev_link(int nsim_idx)
{
	char nsim_dev_dir_buffer[NSIM_DEV_DIR_BUFFER_SIZE];
	DIR *nsim_dev_dir;
	struct dirent *entry;

	sprintf(nsim_dev_dir_buffer, "%s/netdevsim%d/%s", NETDEVSIM_DEV_DIR,
		nsim_idx, "net");

	nsim_dev_dir = opendir(nsim_dev_dir_buffer);

	if (nsim_dev_dir == NULL) {
		fprintf(stderr, "Unable to open %s\n", nsim_dev_dir_buffer);
		return NULL;
	}

	do {
		entry = readdir(nsim_dev_dir);
		if (entry != NULL && entry->d_name[0] != '.')
			break;

	} while (entry != NULL);

	if (entry == NULL || entry->d_name[0] == '.') {
		fprintf(stderr, "Device has no ports\n");
		return NULL;
	}

	closedir(nsim_dev_dir);

	return entry->d_name;
}

int get_nsim_dev(char **nsim_link)
{
	int nsim_idx;
	char *nsim_dev_link;

	nsim_idx = ensure_nsim_dev_exists();
	if (nsim_idx < 0)
		return -1;

	nsim_dev_link = get_nsim_dev_link(nsim_idx);
	if (nsim_dev_link == NULL)
		return -1;

	*nsim_link = nsim_dev_link;
	return 0;
}

int prepare_socket(void)
{
	struct sockaddr_nl sa;
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

	if (fd < 0) {
		fprintf(stderr, "Failed to open socket\n");
		return -1;
	}

	sa.nl_family = AF_NETLINK;

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		fprintf(stderr, "Failed to bind socket\n");

	return fd;
}

struct nlmsghdr *construct_header(char **pos)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)(*pos);

	nlh->nlmsg_type = RTM_NEWROUTE;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE;

	*pos += NLMSG_HDRLEN;

	return nlh;
}

void construct_rtmsg(char **pos)
{
	struct rtmsg *rtm = (struct rtmsg *)(*pos);

	rtm->rtm_family = AF_INET6;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_STATIC;
	rtm->rtm_type = RTN_UNICAST;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_dst_len = 128;
	rtm->rtm_flags |= RTF_HOST | RTF_UP;

	*pos += NLMSG_ALIGN(sizeof(struct rtmsg));
}

void construct_dest(char **pos)
{
	struct rtattr *rta_dest = (struct rtattr *)(*pos);
	struct in6_addr dst6;

	rta_dest->rta_type = RTA_DST;
	rta_dest->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
	inet_pton(AF_INET6, DST_PREFIX, &dst6);
	memcpy(RTA_DATA(rta_dest), &dst6, sizeof(dst6));
	*pos += RTA_ALIGN(rta_dest->rta_len);
}

struct rtattr *construct_multipath_hdr(char **pos)
{
	struct rtattr *rta_mp = (struct rtattr *)(*pos);

	rta_mp->rta_type = RTA_MULTIPATH;
	*pos += sizeof(struct rtattr);

	return rta_mp;
}

void add_nexthop(char **pos, int ifindex, char *gw_addr)
{
	struct rtnexthop *rtnh = (struct rtnexthop *)(*pos);

	rtnh->rtnh_hops = 0;
	rtnh->rtnh_ifindex = ifindex;
	char *rtnh_pos = (char *)rtnh + RTNH_ALIGN(sizeof(struct rtnexthop));

	struct rtattr *attr = (struct rtattr *)rtnh_pos;

	attr->rta_type = RTA_GATEWAY;
	attr->rta_len = RTA_LENGTH(sizeof(struct in6_addr));

	struct in6_addr gw;

	inet_pton(AF_INET6, gw_addr, &gw);
	memcpy(RTA_DATA(attr), &gw, sizeof(gw));

	rtnh_pos += RTA_ALIGN(attr->rta_len);
	rtnh->rtnh_len = rtnh_pos - (char *)rtnh;

	*pos = rtnh_pos;
}

struct nlmsghdr *construct_message(char *buf, int nsim_ifindex, int lo_ifindex)
{
	char *pos = buf;
	struct nlmsghdr *nlh = construct_header(&pos);

	construct_rtmsg(&pos);
	construct_dest(&pos);

	struct rtattr *rta_mp = construct_multipath_hdr(&pos);

	add_nexthop(&pos, nsim_ifindex, GW1);
	add_nexthop(&pos, lo_ifindex, GW2);

	rta_mp->rta_len = pos - (char *)rta_mp;
	nlh->nlmsg_len = pos - buf;

	return nlh;
}

int send_nl_msg(struct nlmsghdr *nlh, int socket)
{
	struct iovec iov = { .iov_base = nlh, .iov_len = nlh->nlmsg_len };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};

	if (sendmsg(socket, (struct msghdr *)&msg, 0) < 0) {
		fprintf(stderr, "Failed to send message\n");
		return 1;
	}

	return 0;
}

int open_kmsg(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);

	if (fd < 0) {
		fprintf(stderr, "Failed to open kmsg\n");
		return -1;
	}

	return fd;
}

int move_cursor_to_end(int fd)
{
	if (lseek(fd, 0, SEEK_END) == -1) {
		fprintf(stderr, "Failed to lseek kmsg\n");
		return -1;
	}

	return 0;
}

int look_for_warn(int kmsg_fd)
{
	char buffer[1024];
	int bytes_read;
	int pid = getpid();
	char pid_str[PID_LEN];

	snprintf(pid_str, PID_LEN, "%d", pid);

	while ((bytes_read = read(kmsg_fd, buffer, sizeof(buffer) - 1)) > 0) {
		buffer[bytes_read] = '\0';
		if (strstr(buffer, "WARNING") && strstr(buffer, pid_str)) {
			printf("Kernel warning detected\n");
			return 1;
		}
	}

	return 0;
}

int main(void)
{
	char *nsim_dev;
	int if_lo, if_nsim;
	int fd;
	int kmsg_fd;
	struct nlmsghdr *nlh;
	char buf[BUFSIZE];

	if (get_nsim_dev(&nsim_dev) != 0)
		return EXIT_FAILURE;

	sleep(1); // Doesn't work without a delay

	if_lo = if_nametoindex(LO_DEV);
	if_nsim = if_nametoindex(nsim_dev);

	if (!if_lo || !if_nsim) {
		fprintf(stderr, "Failed to get interface index\n");
		return EXIT_FAILURE;
	}

	memset(buf, 0, sizeof(buf));
	nlh = construct_message(buf, if_nsim, if_lo);

	fd = prepare_socket();
	if (fd < 0) {
		fprintf(stderr, "Failed to open socket\n");
		close(fd);
		return EXIT_FAILURE;
	}

	kmsg_fd = open_kmsg();
	if (kmsg_fd < 0) {
		fprintf(stderr, "Failed to open kmsg\n");
		close(fd);
		return EXIT_FAILURE;
	}

	if (move_cursor_to_end(kmsg_fd) < 0) {
		fprintf(stderr, "Failed to open kmsg\n");
		close(fd);
		close(kmsg_fd);
		return EXIT_FAILURE;
	}

	if (send_nl_msg(nlh, fd) != 0) {
		close(fd);
		close(kmsg_fd);
		return EXIT_FAILURE;
	}

	if (look_for_warn(kmsg_fd) != 0) {
		close(fd);
		close(kmsg_fd);
		return EXIT_FAILURE;
	}

	close(kmsg_fd);
	close(fd);
	return EXIT_SUCCESS;
}
