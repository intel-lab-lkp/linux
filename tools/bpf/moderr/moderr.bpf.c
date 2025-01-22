// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Samsung */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "moderr.h"

const volatile bool filter_modname = false;
const volatile char targ_modname[MODULE_NAME_LEN];
const volatile bool set_errinj = false;
const volatile int targ_errinj = 0;
const volatile bool filter_modfunc = false;
const volatile int targ_modfunc = 0;

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 2097152);
} rb SEC(".maps");

static __always_inline bool filter_module_name(struct module *mod)
{
	char modname[MODULE_NAME_LEN];

	bpf_probe_read_str(modname, sizeof(modname), mod->name);

	if (!filter_modname ||
	    filter_modname && bpf_strncmp(modname, MODULE_NAME_LEN,
					  (const char *)targ_modname) != 0)
		return false;

	return true;
}

static __always_inline bool filter_module_func(enum modfunc fc)
{
	if (!filter_modfunc || filter_modfunc && targ_modfunc != fc)
		return false;

	return true;
}

static __always_inline bool
generate_errinj_event(struct pt_regs *ctx, struct module *mod, enum modfunc fc)
{
	struct event *e;

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return false;

	e->err = 0;
	e->func = fc;
	bpf_probe_read_str(e->modname, sizeof(e->modname), mod->name);

	if (set_errinj) {
		bpf_override_return(ctx, targ_errinj);
		e->err = targ_errinj;
	}

	bpf_ringbuf_submit(e, 0);
	return true;
}

static __always_inline bool generate_debug_event(struct pt_regs *ctx,
						 struct module *mod,
						 enum modfunc fc,
						 const char *fmt)
{
	struct event *e;

	e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return false;

	e->dbg = BPF_SNPRINTF(e->msg, sizeof(e->msg), "[%s:%s]: %s", mod->name,
			      modfunc_to_string(fc), fmt);

	bpf_ringbuf_submit(e, 0);
	return true;
}

static __always_inline int
module_error_injection(struct pt_regs *ctx, struct module *mod, enum modfunc fc)
{
	if (!filter_module_name(mod)) {
		generate_debug_event(ctx, mod, fc,
				     "Target module does not match");
		return 0;
	}

	if (!filter_module_func(fc)) {
		generate_debug_event(ctx, mod, fc,
				     "Target function does not match");
		return 0;
	}

	if (!generate_errinj_event(ctx, mod, fc)) {
		generate_debug_event(
			ctx, mod, fc,
			"Error injection event cannot be generated");
		return 0;
	}

	return 0;
}

SEC("kprobe/complete_formation")
int BPF_KPROBE(complete_formation, struct module *mod, struct load_info *info)
{
	return module_error_injection(ctx, mod, COMPLETE_FORMATION);
}

SEC("kprobe/do_init_module")
int BPF_KPROBE(do_init_module, struct module *mod, struct load_info *info)
{
	return module_error_injection(ctx, mod, DO_INIT_MODULE);
}

SEC("kprobe/module_enable_rodata_ro_after_init")
int BPF_KPROBE(module_enable_rodata_ro_after_init, struct module *mod)
{
	return module_error_injection(ctx, mod,
				      MODULE_ENABLE_RODATA_AFTER_INIT);
}
