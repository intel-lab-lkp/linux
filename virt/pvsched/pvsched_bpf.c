// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Google  */

#include <linux/types.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/filter.h>
#include <linux/pvsched.h>


/* "extern" is to avoid sparse warning.  It is only used in bpf_struct_ops.c. */
extern struct bpf_struct_ops bpf_pvsched_vcpu_ops;

static int bpf_pvsched_vcpu_init(struct btf *btf)
{
	return 0;
}

static bool bpf_pvsched_vcpu_is_valid_access(int off, int size,
				       enum bpf_access_type type,
				       const struct bpf_prog *prog,
				       struct bpf_insn_access_aux *info)
{
	if (off < 0 || off >= sizeof(__u64) * MAX_BPF_FUNC_ARGS)
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;

	if (!btf_ctx_access(off, size, type, prog, info))
		return false;

	return true;
}

static int bpf_pvsched_vcpu_btf_struct_access(struct bpf_verifier_log *log,
					const struct bpf_reg_state *reg,
					int off, int size)
{
	/*
	 * TODO: Enable write access to Guest shared mem.
	 */
	return -EACCES;
}

static const struct bpf_func_proto *
bpf_pvsched_vcpu_get_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	return bpf_base_func_proto(func_id);
}

static const struct bpf_verifier_ops bpf_pvsched_vcpu_verifier_ops = {
	.get_func_proto		= bpf_pvsched_vcpu_get_func_proto,
	.is_valid_access	= bpf_pvsched_vcpu_is_valid_access,
	.btf_struct_access	= bpf_pvsched_vcpu_btf_struct_access,
};

static int bpf_pvsched_vcpu_init_member(const struct btf_type *t,
				  const struct btf_member *member,
				  void *kdata, const void *udata)
{
	const struct pvsched_vcpu_ops *uvm_ops;
	struct pvsched_vcpu_ops *vm_ops;
	u32 moff;

	uvm_ops = (const struct pvsched_vcpu_ops *)udata;
	vm_ops = (struct pvsched_vcpu_ops *)kdata;

	moff = __btf_member_bit_offset(t, member) / 8;
	switch (moff) {
	case offsetof(struct pvsched_vcpu_ops, events):
		vm_ops->events = *(u32 *)(udata + moff);
		return 1;
	case offsetof(struct pvsched_vcpu_ops, name):
		if (bpf_obj_name_cpy(vm_ops->name, uvm_ops->name,
					sizeof(vm_ops->name)) <= 0)
			return -EINVAL;
		return 1;
	}

	return 0;
}

static int bpf_pvsched_vcpu_check_member(const struct btf_type *t,
				   const struct btf_member *member,
				   const struct bpf_prog *prog)
{
	return 0;
}

static int bpf_pvsched_vcpu_reg(void *kdata)
{
	return pvsched_register_vcpu_ops((struct pvsched_vcpu_ops *)kdata);
}

static void bpf_pvsched_vcpu_unreg(void *kdata)
{
	pvsched_unregister_vcpu_ops((struct pvsched_vcpu_ops *)kdata);
}

static int bpf_pvsched_vcpu_validate(void *kdata)
{
	return pvsched_validate_vcpu_ops((struct pvsched_vcpu_ops *)kdata);
}

static int bpf_pvsched_vcpu_update(void *kdata, void *old_kdata)
{
	return -EOPNOTSUPP;
}

static int __pvsched_vcpu_register(struct pid *pid)
{
	return 0;
}
static void __pvsched_vcpu_unregister(struct pid *pid)
{
}
static void __pvsched_notify_event(void *addr, struct pid *pid, u32 event)
{
}

static struct pvsched_vcpu_ops __bpf_ops_pvsched_vcpu_ops = {
	.pvsched_vcpu_register = __pvsched_vcpu_register,
	.pvsched_vcpu_unregister = __pvsched_vcpu_unregister,
	.pvsched_vcpu_notify_event = __pvsched_notify_event,
};

struct bpf_struct_ops bpf_pvsched_vcpu_ops = {
	.init = &bpf_pvsched_vcpu_init,
	.validate = bpf_pvsched_vcpu_validate,
	.update = bpf_pvsched_vcpu_update,
	.verifier_ops = &bpf_pvsched_vcpu_verifier_ops,
	.reg = bpf_pvsched_vcpu_reg,
	.unreg = bpf_pvsched_vcpu_unreg,
	.check_member = bpf_pvsched_vcpu_check_member,
	.init_member = bpf_pvsched_vcpu_init_member,
	.name = "pvsched_vcpu_ops",
	.cfi_stubs = &__bpf_ops_pvsched_vcpu_ops,
};
