// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Tao Cui */

/*
 * blk-iocost: BPF struct_ops plumbing for pluggable cost models.
 *
 * Registers the "iocost_model_ops" struct_ops type.  At most one model
 * can be registered at a time; devices opt in per-queue with
 * "echo $DEV ctrl=bpf > io.cost.model".  Devices without a registered
 * model keep using the builtin linear model.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/blk-iocost.h>

/* 1s of device time; upper bound on a single IO's chargeable cost */
#define IOCOST_BPF_MAX_COST	(1ULL << 37)

static struct bpf_struct_ops bpf_iocost_model_ops;

static DEFINE_MUTEX(iocost_bpf_reg_lock);
static struct iocost_model_ops __rcu *iocost_bpf_model;
static DEFINE_STATIC_KEY_FALSE(iocost_bpf_key);

bool iocost_bpf_model_registered(void)
{
	return static_branch_unlikely(&iocost_bpf_key);
}

bool iocost_bpf_calc_cost(u64 op, u64 nbytes, u64 sector, u64 cursor,
			  u64 iocg_id, u64 flags, u64 *costp)
{
	const struct iocost_model_ops *ops;
	u64 cost;

	if (!static_branch_unlikely(&iocost_bpf_key))
		return false;

	rcu_read_lock();
	ops = rcu_dereference(iocost_bpf_model);
	if (!ops) {
		rcu_read_unlock();
		return false;
	}
	cost = ops->calc_cost(op, nbytes, sector, cursor, iocg_id, flags);
	rcu_read_unlock();

	if (!cost)
		return false;

	*costp = min(cost, IOCOST_BPF_MAX_COST);
	return true;
}

static int bpf_iocost_model_init(struct btf *btf)
{
	s32 type_id;

	type_id = btf_find_by_name_kind(btf, "iocost_model_ops", BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;
	return 0;
}

static int bpf_iocost_init_member(const struct btf_type *t,
				  const struct btf_member *member,
				  void *kdata, const void *udata)
{
	return 0;
}

static bool bpf_iocost_is_valid_access(int off, int size,
				       enum bpf_access_type type,
				       const struct bpf_prog *prog,
				       struct bpf_insn_access_aux *info)
{
	return bpf_tracing_btf_ctx_access(off, size, type, prog, info);
}

static const struct bpf_func_proto *
bpf_iocost_get_func_proto(enum bpf_func_id func_id,
			  const struct bpf_prog *prog)
{
	return bpf_base_func_proto(func_id, prog);
}

static const struct bpf_verifier_ops bpf_iocost_verifier_ops = {
	.get_func_proto = bpf_iocost_get_func_proto,
	.is_valid_access = bpf_iocost_is_valid_access,
};

static int bpf_iocost_reg(void *kdata, struct bpf_link *link)
{
	struct iocost_model_ops *new_ops = kdata;

	if (!new_ops->calc_cost)
		return -EINVAL;

	int ret = 0;

	/* the caller only holds the per-map lock, so serialize here */
	mutex_lock(&iocost_bpf_reg_lock);
	if (rcu_access_pointer(iocost_bpf_model))
		ret = -EBUSY;
	else {
		static_branch_inc(&iocost_bpf_key);
		rcu_assign_pointer(iocost_bpf_model, new_ops);
	}
	mutex_unlock(&iocost_bpf_reg_lock);
	return ret;
}

static void bpf_iocost_unreg(void *kdata, struct bpf_link *link)
{
	mutex_lock(&iocost_bpf_reg_lock);
	if (rcu_access_pointer(iocost_bpf_model) == kdata) {
		rcu_assign_pointer(iocost_bpf_model, NULL);
		static_branch_dec(&iocost_bpf_key);
	}
	mutex_unlock(&iocost_bpf_reg_lock);
}

static u64 bpf_iocost_calc_cost_stub(u64 op, u64 nbytes, u64 sector,
				     u64 cursor, u64 iocg_id, u64 flags)
{
	return 0;
}

static struct iocost_model_ops __bpf_ops_iocost_model_ops = {
	.calc_cost = bpf_iocost_calc_cost_stub,
};

static struct bpf_struct_ops bpf_iocost_model_ops = {
	.verifier_ops = &bpf_iocost_verifier_ops,
	.init = bpf_iocost_model_init,
	.init_member = bpf_iocost_init_member,
	.reg = bpf_iocost_reg,
	.unreg = bpf_iocost_unreg,
	.name = "iocost_model_ops",
	.cfi_stubs = &__bpf_ops_iocost_model_ops,
	.owner = THIS_MODULE,
};

static int __init bpf_iocost_init(void)
{
	return register_bpf_struct_ops(&bpf_iocost_model_ops, iocost_model_ops);
}
late_initcall(bpf_iocost_init);
