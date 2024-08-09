// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2010 Loongson Inc. & Lemote Inc. &
 *                    Institute of Computing Technology
 * Author:  Xiang Gao, gaoxiang@ict.ac.cn
 *          Huacai Chen, chenhc@lemote.com
 *          Xiaofu Meng, Shuangshuang Zhang
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/numa_memblks.h>
#include <boot_param.h>
#include <loongson.h>

static int __init compute_node_distance(int row, int col)
{
	int package_row = row * loongson_sysconf.cores_per_node /
				loongson_sysconf.cores_per_package;
	int package_col = col * loongson_sysconf.cores_per_node /
				loongson_sysconf.cores_per_package;

	if (col == row)
		return LOCAL_DISTANCE;
	else if (package_row == package_col)
		return 40;
	else
		return 100;
}

int __init arch_platform_numa_init(void)
{
	int nr_nodes, nid, row, col;

	nr_nodes = loongson_sysconf.nr_nodes;

	for (nid = 0; nid < nr_nodes; nid++) {
		node_set(nid, numa_nodes_parsed);
		numa_add_memblk(nid, nid_to_addrbase(nid), nid_to_addrbase(nid + 1));
	}

	for (row = 0; row < nr_nodes; row++) {
		for (col = 0; col < nr_nodes; col++)
			numa_set_distance(row, col, compute_node_distance(row, col));
	}

	return 0;
}
