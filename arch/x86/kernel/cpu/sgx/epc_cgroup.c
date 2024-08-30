// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022-2024 Intel Corporation. */

#include<linux/slab.h>
#include "epc_cgroup.h"

/* The root SGX EPC cgroup */
static struct sgx_cgroup sgx_cg_root;

/**
 * sgx_cgroup_try_charge() - try to charge cgroup for a single EPC page
 *
 * @sgx_cg:	The EPC cgroup to be charged for the page.
 * Return:
 * * %0 - If successfully charged.
 * * -errno - for failures.
 */
int sgx_cgroup_try_charge(struct sgx_cgroup *sgx_cg)
{
	return misc_cg_try_charge(MISC_CG_RES_SGX_EPC, sgx_cg->cg, PAGE_SIZE);
}

/**
 * sgx_cgroup_uncharge() - uncharge a cgroup for an EPC page
 * @sgx_cg:	The charged sgx cgroup.
 */
void sgx_cgroup_uncharge(struct sgx_cgroup *sgx_cg)
{
	misc_cg_uncharge(MISC_CG_RES_SGX_EPC, sgx_cg->cg, PAGE_SIZE);
}

static void sgx_cgroup_free(struct misc_cg *cg)
{
	struct sgx_cgroup *sgx_cg;

	sgx_cg = sgx_cgroup_from_misc_cg(cg);
	if (!sgx_cg)
		return;

	kfree(sgx_cg);
}

static void sgx_cgroup_misc_init(struct misc_cg *cg, struct sgx_cgroup *sgx_cg)
{
	cg->res[MISC_CG_RES_SGX_EPC].priv = sgx_cg;
	sgx_cg->cg = cg;
}

static int sgx_cgroup_alloc(struct misc_cg *cg)
{
	struct sgx_cgroup *sgx_cg;

	sgx_cg = kzalloc(sizeof(*sgx_cg), GFP_KERNEL);
	if (!sgx_cg)
		return -ENOMEM;

	sgx_cgroup_misc_init(cg, sgx_cg);

	return 0;
}

const struct misc_res_ops sgx_cgroup_ops = {
	.alloc = sgx_cgroup_alloc,
	.free = sgx_cgroup_free,
};

/*
 * Register capacity and ops for SGX cgroup and init the root cgroup.
 * Only called at the end of sgx_init() when SGX is ready to handle the ops
 * callbacks. No failures allowed in this function.
 */
void __init sgx_cgroup_init(void)
{
	unsigned int nid = first_node(sgx_numa_mask);
	unsigned int first = nid;
	u64 capacity = 0;

	sgx_cgroup_misc_init(misc_cg_root(), &sgx_cg_root);
	misc_cg_set_ops(MISC_CG_RES_SGX_EPC, &sgx_cgroup_ops);

	/* sgx_numa_mask is not empty when this is called */
	do {
		capacity += sgx_numa_nodes[nid].size;
		nid = next_node_in(nid, sgx_numa_mask);
	} while (nid != first);
	misc_cg_set_capacity(MISC_CG_RES_SGX_EPC, capacity);
}
