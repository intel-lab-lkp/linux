// SPDX-License-Identifier: GPL-2.0
/*
 * scx_ai_numa - AI NUMA-aware scheduler (userspace loader)
 *
 * Detects NUMA topology, configures the BPF scheduler, and prints
 * per-node dispatch statistics every second.
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <sys/stat.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_ai_numa.bpf.skel.h"

/* Must match BPF side */
struct node_stat {
	__u64 local_dsq;
	__u64 numa_dsq;
	__u64 steal;
};

#define MAX_NUMA_NODES 16

static volatile int exit_req;

static void sigint_handler(int sig)
{
	exit_req = 1;
}

/* Detect NUMA node count by scanning sysfs */
static __u32 detect_nr_nodes(void)
{
	struct stat st;
	char path[64];
	__u32 i, count = 0;

	for (i = 0; i < MAX_NUMA_NODES; i++) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/node/node%u", i);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			count = i + 1;
		else
			break;
	}
	return count ? count : 1;
}

static void print_stats(struct scx_ai_numa *skel, __u32 nr_nodes)
{
	int nr_cpus = libbpf_num_possible_cpus();
	int map_fd  = bpf_map__fd(skel->maps.node_stats);

	printf("\n%-6s %14s %14s %14s\n",
	       "Node", "Local-DSQ", "NUMA-DSQ", "Steals");
	printf("------+--------------+--------------+--------------\n");

	for (__u32 node = 0; node < nr_nodes; node++) {
		struct node_stat per_cpu[nr_cpus];
		struct node_stat total = {};
		__u32 key = node;
		int i;

		if (bpf_map_lookup_elem(map_fd, &key, per_cpu) < 0)
			continue;

		for (i = 0; i < nr_cpus; i++) {
			total.local_dsq += per_cpu[i].local_dsq;
			total.numa_dsq  += per_cpu[i].numa_dsq;
			total.steal     += per_cpu[i].steal;
		}

		printf("%-6u %14llu %14llu %14llu\n", node,
		       total.local_dsq, total.numa_dsq, total.steal);
	}
}

int main(int argc, char **argv)
{
	struct scx_ai_numa *skel;
	struct bpf_link *link;
	__u64 ecode;
	__u32 nr_nodes;

	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);

	nr_nodes = detect_nr_nodes();
	printf("scx_ai_numa: detected %u NUMA node(s)\n", nr_nodes);

restart:
	/*
	 * Avoid SCX_OPS_OPEN() which accesses sub_attach/sub_detach/
	 * sub_cgroup_id at compile time. These fields may not be available
	 * in all supported kernel versions.
	 */
	skel = scx_ai_numa__open();
	SCX_BUG_ON(!skel, "Could not open scx_ai_numa");
	skel->struct_ops.ai_numa_ops->hotplug_seq = scx_hotplug_seq();
	SCX_ENUM_INIT(skel);

	/* Pass NUMA topology to the BPF program via rodata */
	skel->rodata->nr_nodes = nr_nodes;

	SCX_OPS_LOAD(skel, ai_numa_ops, scx_ai_numa, uei);
	link = SCX_OPS_ATTACH(skel, ai_numa_ops, scx_ai_numa);

	printf("scx_ai_numa: running (Ctrl-C to stop)\n");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		print_stats(skel, nr_nodes);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_ai_numa__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
