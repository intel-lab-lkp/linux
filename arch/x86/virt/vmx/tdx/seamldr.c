// SPDX-License-Identifier: GPL-2.0
/*
 * P-SEAMLDR support for TDX module management features like runtime updates
 *
 * Copyright (C) 2025 Intel Corporation
 */
#define pr_fmt(fmt)	"seamldr: " fmt

#include <linux/mm.h>
#include <linux/nmi.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stop_machine.h>

#include <asm/seamldr.h>

#include "seamcall_internal.h"

/* P-SEAMLDR SEAMCALL leaf function */
#define P_SEAMLDR_INFO			0x8000000000000000

#define SEAMLDR_MAX_NR_MODULE_PAGES	496
#define SEAMLDR_MAX_NR_SIG_PAGES	4

/*
 * The seamldr_params "scenario" field specifies the operation mode:
 * 0: Install TDX module from scratch (not used by kernel)
 * 1: Update existing TDX module to a compatible version
 */
#define SEAMLDR_SCENARIO_UPDATE		1

/*
 * This is called the "SEAMLDR_PARAMS" data structure and is defined
 * in "SEAM Loader (SEAMLDR) Interface Specification".
 *
 * It describes the TDX module that will be installed.
 */
struct seamldr_params {
	u32	version;
	u32	scenario;
	u64	sigstruct_pa[SEAMLDR_MAX_NR_SIG_PAGES];
	u8	reserved[80];
	u64	num_module_pages;
	u64	mod_pages_pa_list[SEAMLDR_MAX_NR_MODULE_PAGES];
} __packed;

static_assert(sizeof(struct seamldr_params) == 4096);

/*
 * Serialize P-SEAMLDR calls since the hardware only allows a single CPU to
 * interact with P-SEAMLDR simultaneously. Use raw version as the calls can
 * be made with interrupts disabled, where plain spinlocks are prohibited in
 * PREEMPT_RT kernels as they become sleeping locks.
 */
static DEFINE_RAW_SPINLOCK(seamldr_lock);

static int seamldr_call(u64 fn, struct tdx_module_args *args)
{
	guard(raw_spinlock)(&seamldr_lock);
	return seamcall_prerr(fn, args);
}

int seamldr_get_info(struct seamldr_info *seamldr_info)
{
	struct tdx_module_args args = {};

	/*
	 * Use slow_virt_to_phys() since @seamldr_info may be allocated on
	 * the stack.
	 */
	args.rcx = slow_virt_to_phys(seamldr_info);
	return seamldr_call(P_SEAMLDR_INFO, &args);
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_get_info, "tdx-host");

/*
 * Intel TDX module blob. Its format is defined at:
 * https://github.com/intel/tdx-module-binaries/blob/main/blob_structure.txt
 *
 * Note this structure differs from the reference above: the two variable-length
 * fields "@sigstruct" and "@module" are represented as a single "@data" field
 * here and split programmatically using the offset_of_module value.
 *
 * Note @offset_of_module is relative to the start of struct tdx_blob, not
 * @data, and @length is the total length of the blob, not the length of
 * @data.
 */
struct tdx_blob {
	u16	version;
	u16	checksum;
	u32	offset_of_module;
	u8	signature[8];
	u32	length;
	u32	reserved0;
	u64	reserved1[509];
	u8	data[];
} __packed;

/* Supported versions of the tdx_blob */
#define TDX_BLOB_VERSION_1	0x100

/*
 * Blob fields are processed by the kernel and the payloads
 * are passed to the TDX module. Do normal user input type
 * check for any fields that don't get passed to the TDX module.
 */
static const struct tdx_blob *get_and_check_blob(const u8 *data, u32 size)
{
	const struct tdx_blob *blob = (const void *)data;

	/*
	 * Ensure the size is valid otherwise reading any field from the
	 * blob may overflow.
	 */
	if (size <= sizeof(struct tdx_blob))
		return ERR_PTR(-EINVAL);

	/*
	 * Don't care about user passing the wrong file, but protect
	 * kernel ABI by preventing accepting garbage.
	 */
	if (memcmp(blob->signature, "TDX-BLOB", 8))
		return ERR_PTR(-EINVAL);

	/*
	 * Ensure the offset of the module is within valid bounds and
	 * page-aligned.
	 */
	if (blob->offset_of_module >= size || blob->offset_of_module <= sizeof(struct tdx_blob))
		return ERR_PTR(-EINVAL);
	if (!IS_ALIGNED(blob->offset_of_module, PAGE_SIZE))
		return ERR_PTR(-EINVAL);

	if (blob->version != TDX_BLOB_VERSION_1)
		return ERR_PTR(-EINVAL);

	if (blob->reserved0 || memchr_inv(blob->reserved1, 0, sizeof(blob->reserved1)))
		return ERR_PTR(-EINVAL);

	return blob;
}

static struct seamldr_params *alloc_seamldr_params(const struct tdx_blob *blob, unsigned int blob_size)
{
	struct seamldr_params *params;
	int module_pg_cnt, sig_pg_cnt;
	const u8 *sig, *module;
	int i;

	params = (struct seamldr_params *)get_zeroed_page(GFP_KERNEL);
	if (!params)
		return ERR_PTR(-ENOMEM);

	/*
	 * Split the blob into a sigstruct and a module. Assume all
	 * size/offsets are within bounds of blob_size due to prior checks.
	 */
	sig		= blob->data;
	sig_pg_cnt	= (blob->offset_of_module - sizeof(struct tdx_blob)) >> PAGE_SHIFT;
	module		= (const u8 *)blob + blob->offset_of_module;
	module_pg_cnt	= (blob_size - blob->offset_of_module) >> PAGE_SHIFT;

	/*
	 * Only use version 1 when required (sigstruct > 4KB) for backward
	 * compatibility with P-SEAMLDR that lacks version 1 support.
	 */
	params->version = sig_pg_cnt > 1;
	params->scenario = SEAMLDR_SCENARIO_UPDATE;

	for (i = 0; i < MIN(sig_pg_cnt, SEAMLDR_MAX_NR_SIG_PAGES); i++) {
		params->sigstruct_pa[i] = vmalloc_to_pfn(sig) << PAGE_SHIFT;
		sig += PAGE_SIZE;
	}

	params->num_module_pages = MIN(module_pg_cnt, SEAMLDR_MAX_NR_MODULE_PAGES);
	for (i = 0; i < params->num_module_pages; i++) {
		params->mod_pages_pa_list[i] = vmalloc_to_pfn(module) << PAGE_SHIFT;
		module += PAGE_SIZE;
	}

	return params;
}

static struct seamldr_params *init_seamldr_params(const u8 *data, u32 size)
{
	const struct tdx_blob *blob;

	blob = get_and_check_blob(data, size);
	if (IS_ERR(blob))
		return ERR_CAST(blob);

	return alloc_seamldr_params(blob, size);
}

/*
 * During a TDX module update, all CPUs start from MODULE_UPDATE_START and
 * progress to MODULE_UPDATE_DONE. Each state is associated with certain
 * work. For some states, just one CPU needs to perform the work, while
 * other CPUs just wait during those states.
 */
enum module_update_state {
	MODULE_UPDATE_START,
	MODULE_UPDATE_DONE,
};

static struct {
	enum module_update_state state;
	int thread_ack;
	/*
	 * Protect update_data. Raw spinlock as it will be acquired from
	 * interrupt-disabled contexts.
	 */
	raw_spinlock_t lock;
} update_data = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(update_data.lock)
};

static void set_target_state(enum module_update_state state)
{
	/* Reset ack counter. */
	update_data.thread_ack = num_online_cpus();
	update_data.state = state;
}

/* Last one to ack a state moves to the next state. */
static void ack_state(void)
{
	guard(raw_spinlock)(&update_data.lock);
	update_data.thread_ack--;
	if (!update_data.thread_ack)
		set_target_state(update_data.state + 1);
}

/*
 * See multi_cpu_stop() from where this multi-cpu state-machine was
 * adopted, and the rationale for touch_nmi_watchdog().
 */
static int do_seamldr_install_module(void *seamldr_params)
{
	enum module_update_state newstate, curstate = MODULE_UPDATE_START;
	int ret = 0;

	do {
		/* Chill out and re-read update_data. */
		cpu_relax();
		newstate = READ_ONCE(update_data.state);

		if (newstate != curstate) {
			curstate = newstate;
			switch (curstate) {
			/* TODO: add the update steps. */
			default:
				break;
			}

			ack_state();
		} else {
			touch_nmi_watchdog();
			rcu_momentary_eqs();
		}
	} while (curstate != MODULE_UPDATE_DONE);

	return ret;
}

DEFINE_FREE(free_seamldr_params, struct seamldr_params *,
	    if (!IS_ERR_OR_NULL(_T)) free_page((unsigned long)_T))

/**
 * seamldr_install_module - Install a new TDX module.
 * @data: Pointer to the TDX module update blob.
 * @size: Size of the TDX module update blob.
 *
 * Returns 0 on success, negative error code on failure.
 */
int seamldr_install_module(const u8 *data, u32 size)
{
	struct seamldr_params *params __free(free_seamldr_params) =
						init_seamldr_params(data, size);
	if (IS_ERR(params))
		return PTR_ERR(params);

	/* Ensure a stable set of online CPUs for the update process. */
	guard(cpus_read_lock)();
	set_target_state(MODULE_UPDATE_START + 1);
	return stop_machine_cpuslocked(do_seamldr_install_module, params, cpu_online_mask);
}
EXPORT_SYMBOL_FOR_MODULES(seamldr_install_module, "tdx-host");
