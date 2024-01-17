#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#include <libmnl/libmnl.h>
#include <linux/pkt_sched.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

#include "tc_sch_fq.skel.h"

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	return vprintf(format, args);
}

#define TCA_BUF_MAX (64 * 1024)
#define FILTER_NAMESZ 16

bool cleanup;
unsigned int ifindex;
unsigned int handle = 0x8000000;
unsigned int parent = TC_H_ROOT;
struct mnl_socket *nl;

static void usage(const char *cmd)
{
	printf("Attach an fq eBPF qdisc and optionally an EDT rate limiter.\n");
	printf("Usage: %s [...]\n", cmd);
	printf("	-d <device>	Device\n");
	printf("	-h <handle>	Qdisc handle\n");
	printf("	-p <parent>	Parent Qdisc handle\n");
	printf("	-s		Share packet drop info with the clsact EDT rate limiter\n");
	printf("	-c		Delete the qdisc before quit\n");
	printf("	-v		Verbose\n");
}

static int get_tc_classid(__u32 *h, const char *str)
{
	unsigned long maj, min;
	char *p;

	maj = TC_H_ROOT;
	if (strcmp(str, "root") == 0)
		goto ok;
	maj = TC_H_UNSPEC;
	if (strcmp(str, "none") == 0)
		goto ok;
	maj = strtoul(str, &p, 16);
	if (p == str) {
		maj = 0;
		if (*p != ':')
			return -1;
	}
	if (*p == ':') {
		if (maj >= (1<<16))
			return -1;
		maj <<= 16;
		str = p+1;
		min = strtoul(str, &p, 16);
		if (*p != 0)
			return -1;
		if (min >= (1<<16))
			return -1;
		maj |= min;
	} else if (*p != 0)
		return -1;

ok:
	*h = maj;
	return 0;
}

static int get_qdisc_handle(__u32 *h, const char *str)
{
	__u32 maj;
	char *p;

	maj = TC_H_UNSPEC;
	if (strcmp(str, "none") == 0)
		goto ok;
	maj = strtoul(str, &p, 16);
	if (p == str || maj >= (1 << 16))
		return -1;
	maj <<= 16;
	if (*p != ':' && *p != 0)
		return -1;
ok:
	*h = maj;
	return 0;
}

static void sigdown(int signo)
{
	struct {
		struct nlmsghdr n;
		struct tcmsg t;
		char buf[TCA_BUF_MAX];
	} req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
		.n.nlmsg_flags = NLM_F_REQUEST,
		.n.nlmsg_type = RTM_DELQDISC,
		.t.tcm_family = AF_UNSPEC,
	};

	if (!cleanup)
		exit(0);

	req.n.nlmsg_seq = time(NULL);
	req.t.tcm_ifindex = ifindex;
	req.t.tcm_parent = TC_H_ROOT;
	req.t.tcm_handle = handle;

	if (mnl_socket_sendto(nl, &req.n, req.n.nlmsg_len) < 0)
		exit(1);

	exit(0);
}

static int qdisc_add_tc_sch_fq(struct tc_sch_fq *skel)
{
	char qdisc_type[FILTER_NAMESZ] = "bpf";
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct rtattr *option_attr;
	const char *qdisc_name;
	char prog_name[256];
	int ret;
	unsigned int seq, portid;
	struct {
		struct nlmsghdr n;
		struct tcmsg t;
		char buf[TCA_BUF_MAX];
	} req = {
		.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct tcmsg)),
		.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_EXCL | NLM_F_CREATE,
		.n.nlmsg_type = RTM_NEWQDISC,
		.t.tcm_family = AF_UNSPEC,
	};

	seq = time(NULL);
	portid = mnl_socket_get_portid(nl);

	qdisc_name = bpf_object__name(skel->obj);

	req.t.tcm_ifindex = ifindex;
	req.t.tcm_parent = parent;
	req.t.tcm_handle = handle;
	mnl_attr_put_str(&req.n, TCA_KIND, qdisc_type);

	// eBPF Qdisc specific attributes
	option_attr = (struct rtattr *)mnl_nlmsg_get_payload_tail(&req.n);
	mnl_attr_put(&req.n, TCA_OPTIONS, 0, NULL);
	mnl_attr_put_u32(&req.n, TCA_SCH_BPF_ENQUEUE_PROG_FD,
			 bpf_program__fd(skel->progs.enqueue_prog));
	snprintf(prog_name, sizeof(prog_name), "%s_enqueue", qdisc_name);
	mnl_attr_put(&req.n, TCA_SCH_BPF_ENQUEUE_PROG_NAME, strlen(prog_name) + 1, prog_name);

	mnl_attr_put_u32(&req.n, TCA_SCH_BPF_DEQUEUE_PROG_FD,
			 bpf_program__fd(skel->progs.dequeue_prog));
	snprintf(prog_name, sizeof(prog_name), "%s_dequeue", qdisc_name);
	mnl_attr_put(&req.n, TCA_SCH_BPF_DEQUEUE_PROG_NAME, strlen(prog_name) + 1, prog_name);

	mnl_attr_put_u32(&req.n, TCA_SCH_BPF_RESET_PROG_FD,
			 bpf_program__fd(skel->progs.reset_prog));
	snprintf(prog_name, sizeof(prog_name), "%s_reset", qdisc_name);
	mnl_attr_put(&req.n, TCA_SCH_BPF_RESET_PROG_NAME, strlen(prog_name) + 1, prog_name);

	option_attr->rta_len = (void *)mnl_nlmsg_get_payload_tail(&req.n) -
			       (void *)option_attr;

	if (mnl_socket_sendto(nl, &req.n, req.n.nlmsg_len) < 0) {
		perror("mnl_socket_sendto");
		return -1;
	}

	for (;;) {
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret == -1) {
			if (errno == ENOBUFS || errno == EINTR)
				continue;

			if (errno == EAGAIN) {
				errno = 0;
				ret = 0;
				break;
			}

			perror("mnl_socket_recvfrom");
			return -1;
		}

		ret = mnl_cb_run(buf, ret, seq, portid, NULL, NULL);
		if (ret < 0) {
			perror("mnl_cb_run");
			return -1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	LIBBPF_OPTS(bpf_object_open_opts, opts, .kernel_log_level = 2);
	bool verbose = false, share = false;
	struct tc_sch_fq *skel = NULL;
	struct stat stat_buf = {};
	char d[IFNAMSIZ] = "lo";
	int opt, ret = 1;
	struct sigaction sa = {
		.sa_handler = sigdown,
	};

	while ((opt = getopt(argc, argv, "d:h:p:csv")) != -1) {
		switch (opt) {
		/* General args */
		case 'd':
			strncpy(d, optarg, sizeof(d)-1);
			break;
		case 'h':
			ret = get_qdisc_handle(&handle, optarg);
			if (ret) {
				printf("Invalid qdisc handle\n");
				return 1;
			}
			break;
		case 'p':
			ret = get_tc_classid(&parent, optarg);
			if (ret) {
				printf("Invalid parent qdisc handle\n");
				return 1;
			}
			break;
		case 'c':
			cleanup = true;
			break;
		case 's':
			share = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (!nl) {
		perror("mnl_socket_open");
		return 1;
	}

	ret = mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID);
	if (ret < 0) {
		perror("mnl_socket_bind");
		ret = 1;
		goto out;
	}

	ifindex = if_nametoindex(d);
	if (errno == ENODEV) {
		fprintf(stderr, "No such device: %s\n", d);
		goto out;
	}

	if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL))
		goto out;

	if (verbose)
		libbpf_set_print(libbpf_print_fn);

	skel = tc_sch_fq__open_opts(&opts);
	if (!skel) {
		perror("Failed to open tc_sch_fq");
		goto out;
	}

	if (share) {
		if (stat("/sys/fs/bpf/tc", &stat_buf) == -1)
			mkdir("/sys/fs/bpf/tc", 0700);

		mkdir("/sys/fs/bpf/tc/globals", 0700);

		bpf_map__set_pin_path(skel->maps.rate_map, "/sys/fs/bpf/tc/globals/rate_map");
		bpf_map__set_pin_path(skel->maps.comp_map, "/sys/fs/bpf/tc/globals/comp_map");

		skel->bss->q_compensate_tstamp = true;
		skel->bss->q_random_drop = true;
	}

	ret = tc_sch_fq__load(skel);
	if (ret) {
		perror("Failed to load tc_sch_fq");
		ret = 1;
		goto out_destroy;
	}

	ret = qdisc_add_tc_sch_fq(skel);
	if (ret < 0) {
		perror("Failed to create qdisc");
		ret = 1;
		goto out_destroy;
	}

	for (;;)
		pause();

out_destroy:
	tc_sch_fq__destroy(skel);
out:
	mnl_socket_close(nl);
	return ret;
}
