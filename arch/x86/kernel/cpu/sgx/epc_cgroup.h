/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SGX_EPC_CGROUP_H_
#define _SGX_EPC_CGROUP_H_

#include <asm/sgx.h>
#include <linux/cgroup.h>
#include <linux/misc_cgroup.h>

#include "sgx.h"

#ifndef CONFIG_CGROUP_MISC

#define MISC_CG_RES_SGX_EPC MISC_CG_RES_TYPES
struct sgx_cgroup;

static inline struct sgx_cgroup *sgx_get_current_cg(void)
{
	return NULL;
}

static inline void sgx_put_cg(struct sgx_cgroup *sgx_cg) { }

static inline int sgx_cgroup_try_charge(struct sgx_cgroup *sgx_cg, enum sgx_reclaim reclaim)
{
	return 0;
}

static inline void sgx_cgroup_uncharge(struct sgx_cgroup *sgx_cg) { }

static inline bool sgx_cgroup_lru_empty(struct misc_cg *root)
{
	return true;
}

static inline int __init sgx_cgroup_init(void)
{
	return 0;
}

static inline void __init sgx_cgroup_deinit(void) { }

static inline void __init sgx_cgroup_register(void) { }

static inline void sgx_cgroup_reclaim_pages_global(struct mm_struct *charge_mm) { }

static inline void sgx_cgroup_reclaim_direct(void) { }

#else /* CONFIG_CGROUP_MISC */

struct sgx_cgroup {
	struct misc_cg *cg;
	struct sgx_epc_lru_list lru;
	struct work_struct reclaim_work;
	/*
	 * Pointer to the next cgroup to scan when the per-cgroup reclamation
	 * is triggered next time. It does not hold a reference to prevent it
	 * from being freed in order to allow the misc cgroup subsystem to
	 * release and free the cgroup as needed, e.g., when admin wants to
	 * delete the cgroup. When the cgroup pointed to is being freed,
	 * sgx_cgroup_next_cg_skip(), will be invoked to update the pointer to
	 * next accessible cgroup in a preorder walk of the subtree of the same
	 * root.
	 */
	struct sgx_cgroup *next_cg;
	/* Lock to protect concurrent access to @next_cg */
	spinlock_t next_cg_lock;
};

static inline struct sgx_cgroup *sgx_cgroup_from_misc_cg(struct misc_cg *cg)
{
	return (struct sgx_cgroup *)(cg->res[MISC_CG_RES_SGX_EPC].priv);
}

/**
 * sgx_get_current_cg() - get the EPC cgroup of current process.
 *
 * Returned cgroup has its ref count increased by 1. Caller must call
 * sgx_put_cg() to return the reference.
 *
 * Return: EPC cgroup to which the current task belongs to.
 */
static inline struct sgx_cgroup *sgx_get_current_cg(void)
{
	/* get_current_misc_cg() never returns NULL when Kconfig enabled */
	return sgx_cgroup_from_misc_cg(get_current_misc_cg());
}

/**
 * sgx_put_cg() - Put the EPC cgroup and reduce its ref count.
 * @sgx_cg - EPC cgroup to put.
 */
static inline void sgx_put_cg(struct sgx_cgroup *sgx_cg)
{
	put_misc_cg(sgx_cg->cg);
}

int sgx_cgroup_try_charge(struct sgx_cgroup *sgx_cg, enum sgx_reclaim reclaim);
void sgx_cgroup_uncharge(struct sgx_cgroup *sgx_cg);
bool sgx_cgroup_lru_empty(struct misc_cg *root);
void sgx_cgroup_reclaim_pages_global(struct mm_struct *charge_mm);
void sgx_cgroup_reclaim_direct(void);
int __init sgx_cgroup_init(void);
void __init sgx_cgroup_register(void);
void __init sgx_cgroup_deinit(void);

#endif /* CONFIG_CGROUP_MISC */

#endif /* _SGX_EPC_CGROUP_H_ */
