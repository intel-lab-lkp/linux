// SPDX-License-Identifier: GPL-2.0
/*
 * BPF struct_ops support for Runtime Verification monitors
 *
 * Allows BPF programs to register as RV monitors via struct_ops.
 * BPF monitors appear in /sys/kernel/tracing/rv/ alongside kernel monitors.
 *
 * Copyright (C) 2026-2029 Red Hat, Inc. Gabriele Monaco <gmonaco@redhat.com>
 */

#include <linux/bpf.h>
#include <linux/bpf_verifier.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/filter.h>
#include <linux/rv.h>

static int bpf_rv_monitor_init(struct btf *btf)
{
	return 0;
}

static int bpf_rv_monitor_init_member(const struct btf_type *t,
				      const struct btf_member *member,
				      void *kdata, const void *udata)
{
	const struct rv_monitor *umon = udata;
	struct rv_monitor *kmon = kdata;
	u32 moff = __btf_member_bit_offset(t, member) / 8;
	int ret;

	switch (moff) {
	case offsetof(struct rv_monitor, name):
		ret = bpf_obj_name_cpy(kmon->name, umon->name,
				       sizeof(kmon->name));
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -EINVAL;
		return 1;
	case offsetof(struct rv_monitor, description):
		ret = strscpy(kmon->description, umon->description);
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -EINVAL;
		return 1;
	}

	return 0;
}

static int bpf_rv_monitor_reg(void *kdata, struct bpf_link *link)
{
	struct rv_monitor *mon = kdata;

	pr_info("rv: Registering BPF monitor %s\n", mon->name);
	return rv_register_monitor(mon, NULL);
}

static void bpf_rv_monitor_unreg(void *kdata, struct bpf_link *link)
{
	struct rv_monitor *mon = kdata;

	pr_info("rv: Unregistering BPF monitor %s\n", mon->name);
	rv_unregister_monitor(mon);
}

static int bpf_rv_monitor_validate(void *kdata)
{
	struct rv_monitor *mon = kdata;

	if (!mon->enable)
		return -EINVAL;

	return 0;
}

static const struct bpf_verifier_ops bpf_rv_monitor_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = NULL,
};

static int rv_ops__mon_enable(void)
{
	return 0;
}

static void rv_ops__mon_disable(void) { }

static void rv_ops__mon_reset(void) { }

static struct rv_monitor __bpf_ops_rv_monitor = {
	.name = "rv_monitor",
	.description = "stub BPF monitor.",
	.enable = rv_ops__mon_enable,
	.disable = rv_ops__mon_disable,
	.reset = rv_ops__mon_reset,
	.enabled = 0,
};

static struct bpf_struct_ops bpf_rv_monitor_ops = {
	.verifier_ops = &bpf_rv_monitor_verifier_ops,
	.init = bpf_rv_monitor_init,
	.init_member = bpf_rv_monitor_init_member,
	.reg = bpf_rv_monitor_reg,
	.unreg = bpf_rv_monitor_unreg,
	.validate = bpf_rv_monitor_validate,
	.name = "rv_monitor",
	.cfi_stubs = &__bpf_ops_rv_monitor,
	.owner = THIS_MODULE,
};

static int __init bpf_rv_monitor_init_ops(void)
{
	return register_bpf_struct_ops(&bpf_rv_monitor_ops, rv_monitor);
}
late_initcall(bpf_rv_monitor_init_ops);
