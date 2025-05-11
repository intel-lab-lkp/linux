// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2009, Microsoft Corporation.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/hyperv.h>
#include <linux/random.h>
#include <linux/clockchips.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <clocksource/hyperv_timer.h>
#include <asm/mshyperv.h>
#include <linux/set_memory.h>
#include "hyperv_vmbus.h"

/* The one and only */
struct hv_context hv_context;

/*
 * hv_init - Main initialization routine.
 *
 * This routine must be called before any other routines in here are called
 */
int hv_init(void)
{
	hv_context.cpu_context = alloc_percpu(struct hv_per_cpu_context);
	if (!hv_context.cpu_context)
		return -ENOMEM;
	return 0;
}

/*
 * hv_post_message - Post a message using the hypervisor message IPC.
 *
 * This involves a hypercall.
 */
int hv_post_message(union hv_connection_id connection_id,
			enum hv_message_type message_type,
			void *payload, size_t payload_size)
{
	struct hv_input_post_message *aligned_msg;
	unsigned long flags;
	u64 status;

	if (payload_size > HV_MESSAGE_PAYLOAD_BYTE_COUNT)
		return -EMSGSIZE;

	local_irq_save(flags);

	/*
	 * A TDX VM with the paravisor must use the decrypted post_msg_page: see
	 * the comment in struct hv_per_cpu_context. A SNP VM with the paravisor
	 * can use the encrypted hyperv_pcpu_input_arg because it copies the
	 * input into the GHCB page, which has been decrypted by the paravisor.
	 */
	if (hv_isolation_type_tdx() && ms_hyperv.paravisor_present)
		aligned_msg = this_cpu_ptr(hv_context.cpu_context)->post_msg_page;
	else
		aligned_msg = *this_cpu_ptr(hyperv_pcpu_input_arg);

	aligned_msg->connectionid = connection_id;
	aligned_msg->reserved = 0;
	aligned_msg->message_type = message_type;
	aligned_msg->payload_size = payload_size;
	memcpy((void *)aligned_msg->payload, payload, payload_size);

	if (ms_hyperv.paravisor_present && !vmbus_is_confidential()) {
		if (hv_isolation_type_tdx())
			status = hv_tdx_hypercall(HVCALL_POST_MESSAGE,
						  virt_to_phys(aligned_msg), 0);
		else if (hv_isolation_type_snp())
			status = hv_ghcb_hypercall(HVCALL_POST_MESSAGE,
						   aligned_msg, NULL,
						   sizeof(*aligned_msg));
		else
			status = HV_STATUS_INVALID_PARAMETER;
	} else {
		status = hv_do_hypercall(HVCALL_POST_MESSAGE,
					 aligned_msg, NULL);
	}

	local_irq_restore(flags);

	return hv_result(status);
}

enum hv_page_encryption_action {
	HV_PAGE_ENC_DEAFULT,
	HV_PAGE_ENC_ENCRYPT,
	HV_PAGE_ENC_DECRYPT
};

static int hv_alloc_page(unsigned int cpu, void **page, enum hv_page_encryption_action enc_action,
	const char *note)
{
	int ret = 0;

	pr_debug("allocating %s\n", note);

	/*
	 * After the page changes its encryption status, its contents will
	 * appear scrambled. Thus `get_zeroed_page` would zero the page out
	 * in vain, we do that ourselves exactly one time.
	 *
	 * The function might be called from contexts where sleeping is very
	 * bad (like hotplug callbacks) or not possible (interrupt handling),
	 * Thus requesting `GFP_ATOMIC`.
	 *
	 * The page order is 0 as we need 1 page and log_2 (1) = 0.
	 */
	*page = (void *)__get_free_pages(GFP_ATOMIC, 0);
	if (!*page)
		return -ENOMEM;

	pr_debug("allocated %s\n", note);

	switch (enc_action) {
	case HV_PAGE_ENC_ENCRYPT:
		ret = set_memory_encrypted((unsigned long)*page, 1);
		break;
	case HV_PAGE_ENC_DECRYPT:
		ret = set_memory_decrypted((unsigned long)*page, 1);
		break;
	case HV_PAGE_ENC_DEAFULT:
		break;
	default:
		pr_warn("unknown page encryption action %d for %s\n", enc_action, note);
		break;
	}

	if (ret)
		goto failed;

	memset(*page, 0, PAGE_SIZE);
	return 0;

failed:

	pr_err("page encryption action %d failed for %s, error %d when allocating the page\n",
		enc_action, note, ret);
	free_page((unsigned long)*page);
	*page = NULL;
	return ret;
}

static int hv_free_page(void **page, enum hv_page_encryption_action enc_action,
	const char *note)
{
	int ret = 0;

	pr_debug("freeing %s\n", note);

	if (!page)
		return 0;
	if (!*page)
		return 0;

	switch (enc_action) {
	case HV_PAGE_ENC_ENCRYPT:
		ret = set_memory_encrypted((unsigned long)*page, 1);
		break;
	case HV_PAGE_ENC_DECRYPT:
		ret = set_memory_decrypted((unsigned long)*page, 1);
		break;
	case HV_PAGE_ENC_DEAFULT:
		break;
	default:
		pr_warn("unknown page encryption action %d for %s page\n",
			enc_action, note);
		break;
	}

	/*
	 * In the case of the action failure, the page is leaked.
	 * Something is wrong, prefer to lose the page and stay afloat.
	 */
	if (ret) {
		pr_err("page encryption action %d failed for %s, error %d when freeing\n",
			enc_action, note, ret);
	} else {
		pr_debug("freed %s\n", note);
		free_page((unsigned long)*page);
	}

	*page = NULL;

	return ret;
}

static bool hv_should_allocate_post_msg_page(void)
{
	return ms_hyperv.paravisor_present && hv_isolation_type_tdx();
}

static bool hv_should_allocate_synic_pages(void)
{
	return !ms_hyperv.paravisor_present && !hv_root_partition();
}

static bool hv_should_allocate_pv_synic_pages(void)
{
	return vmbus_is_confidential();
}

int hv_synic_alloc(void)
{
	int cpu, ret = -ENOMEM;
	struct hv_per_cpu_context *hv_cpu;

	const bool allocate_post_msg_page = hv_should_allocate_post_msg_page();
	const bool allocate_synic_pages = hv_should_allocate_synic_pages();
	const bool allocate_pv_synic_pages = hv_should_allocate_pv_synic_pages();
	const enum hv_page_encryption_action enc_action =
		(!vmbus_is_confidential()) ? HV_PAGE_ENC_DECRYPT : HV_PAGE_ENC_DEAFULT;

	/*
	 * First, zero all per-cpu memory areas so hv_synic_free() can
	 * detect what memory has been allocated and cleanup properly
	 * after any failures.
	 */
	for_each_present_cpu(cpu) {
		hv_cpu = per_cpu_ptr(hv_context.cpu_context, cpu);
		memset(hv_cpu, 0, sizeof(*hv_cpu));
	}

	hv_context.hv_numa_map = kcalloc(nr_node_ids, sizeof(struct cpumask),
					 GFP_KERNEL);
	if (!hv_context.hv_numa_map) {
		pr_err("Unable to allocate NUMA map\n");
		goto err;
	}

	for_each_present_cpu(cpu) {
		hv_cpu = per_cpu_ptr(hv_context.cpu_context, cpu);

		tasklet_init(&hv_cpu->msg_dpc,
			     vmbus_on_msg_dpc, (unsigned long)hv_cpu);

		if (allocate_post_msg_page) {
			ret = hv_alloc_page(cpu, &hv_cpu->post_msg_page,
				enc_action, "post msg page");
			if (ret)
				goto err;
		}

		/*
		 * If these SynIC pages are not allocated, SIEF and SIM pages
		 * are configured using what the root partition or the paravisor
		 * provides upon reading the SIEFP and SIMP registers.
		 */
		if (allocate_synic_pages) {
			ret = hv_alloc_page(cpu, &hv_cpu->hv_synic_message_page,
				enc_action, "SynIC msg page");
			if (ret)
				goto err;
			ret = hv_alloc_page(cpu, &hv_cpu->hv_synic_event_page,
				enc_action, "SynIC event page");
			if (ret)
				goto err;
		}

		if (allocate_pv_synic_pages) {
			ret = hv_alloc_page(cpu, &hv_cpu->pv_synic_message_page,
				HV_PAGE_ENC_DEAFULT, "pv SynIC msg page");
			if (ret)
				goto err;
			ret = hv_alloc_page(cpu, &hv_cpu->pv_synic_event_page,
				HV_PAGE_ENC_DEAFULT, "pv SynIC event page");
			if (ret)
				goto err;
		}
	}

	return 0;

err:
	/*
	 * Any memory allocations that succeeded will be freed when
	 * the caller cleans up by calling hv_synic_free()
	 */
	return ret;
}

void hv_synic_free(void)
{
	int cpu;

	const bool free_post_msg_page = hv_should_allocate_post_msg_page();
	const bool free_synic_pages = hv_should_allocate_synic_pages();
	const bool free_pv_synic_pages = hv_should_allocate_pv_synic_pages();

	for_each_present_cpu(cpu) {
		struct hv_per_cpu_context *hv_cpu =
			per_cpu_ptr(hv_context.cpu_context, cpu);

		if (free_post_msg_page)
			hv_free_page(&hv_cpu->post_msg_page,
				HV_PAGE_ENC_ENCRYPT, "post msg page");
		if (free_synic_pages) {
			hv_free_page(&hv_cpu->hv_synic_event_page,
				HV_PAGE_ENC_ENCRYPT, "SynIC event page");
			hv_free_page(&hv_cpu->hv_synic_message_page,
				HV_PAGE_ENC_ENCRYPT, "SynIC msg page");
		}
		if (free_pv_synic_pages) {
			hv_free_page(&hv_cpu->pv_synic_event_page,
				HV_PAGE_ENC_DEAFULT, "pv SynIC event page");
			hv_free_page(&hv_cpu->pv_synic_message_page,
				HV_PAGE_ENC_DEAFULT, "pv SynIC msg page");
		}
	}

	kfree(hv_context.hv_numa_map);
}

/*
 * hv_synic_enable_regs - Initialize the Synthetic Interrupt Controller.
 *
 * If it is already initialized by another entity (ie x2v shim), we need to
 * retrieve the initialized message and event pages.  Otherwise, we create and
 * initialize the message and event pages.
 */
void hv_synic_enable_regs(unsigned int cpu)
{
	struct hv_per_cpu_context *hv_cpu =
		per_cpu_ptr(hv_context.cpu_context, cpu);
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;
	union hv_synic_sint shared_sint;

	/* Setup the Synic's message page */
	simp.as_uint64 = hv_get_msr(HV_MSR_SIMP);
	simp.simp_enabled = 1;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		/* Mask out vTOM bit. ioremap_cache() maps decrypted */
		u64 base = (simp.base_simp_gpa << HV_HYP_PAGE_SHIFT) &
				~ms_hyperv.shared_gpa_boundary;
		hv_cpu->hv_synic_message_page
			= (void *)ioremap_cache(base, HV_HYP_PAGE_SIZE);
		if (!hv_cpu->hv_synic_message_page)
			pr_err("Fail to map synic message page.\n");
	} else {
		simp.base_simp_gpa = virt_to_phys(hv_cpu->hv_synic_message_page)
			>> HV_HYP_PAGE_SHIFT;
	}

	hv_set_msr(HV_MSR_SIMP, simp.as_uint64);

	/* Setup the Synic's event page with the hypervisor. */
	siefp.as_uint64 = hv_get_msr(HV_MSR_SIEFP);
	siefp.siefp_enabled = 1;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		/* Mask out vTOM bit. ioremap_cache() maps decrypted */
		u64 base = (siefp.base_siefp_gpa << HV_HYP_PAGE_SHIFT) &
				~ms_hyperv.shared_gpa_boundary;
		hv_cpu->hv_synic_event_page
			= (void *)ioremap_cache(base, HV_HYP_PAGE_SIZE);
		if (!hv_cpu->hv_synic_event_page)
			pr_err("Fail to map synic event page.\n");
	} else {
		siefp.base_siefp_gpa = virt_to_phys(hv_cpu->hv_synic_event_page)
			>> HV_HYP_PAGE_SHIFT;
	}

	hv_set_msr(HV_MSR_SIEFP, siefp.as_uint64);

	/* Setup the shared SINT. */
	if (vmbus_irq != -1)
		enable_percpu_irq(vmbus_irq, 0);
	shared_sint.as_uint64 = hv_get_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT);

	shared_sint.vector = vmbus_interrupt;
	shared_sint.masked = false;

	/*
	 * On architectures where Hyper-V doesn't support AEOI (e.g., ARM64),
	 * it doesn't provide a recommendation flag and AEOI must be disabled.
	 */
#ifdef HV_DEPRECATING_AEOI_RECOMMENDED
	shared_sint.auto_eoi =
			!(ms_hyperv.hints & HV_DEPRECATING_AEOI_RECOMMENDED);
#else
	shared_sint.auto_eoi = 0;
#endif
	hv_set_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT,
				shared_sint.as_uint64);
}

static void hv_synic_enable_interrupts(void)
{
	union hv_synic_scontrol sctrl;

	/* Enable the global synic bit */
	sctrl.as_uint64 = hv_get_msr(HV_MSR_SCONTROL);
	sctrl.enable = 1;

	hv_set_msr(HV_MSR_SCONTROL, sctrl.as_uint64);
}

/*
 * The paravisor might not support proxying SynIC, and this
 * function may fail.
 */
static int hv_pv_synic_enable_regs(unsigned int cpu)
{
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;

	int err;
	struct hv_per_cpu_context *hv_cpu
		= per_cpu_ptr(hv_context.cpu_context, cpu);

	/* Setup the Synic's message page with the paravisor. */
	simp.as_uint64 = hv_pv_get_synic_register(HV_MSR_SIMP, &err);
	if (err)
		return err;
	simp.simp_enabled = 1;
	simp.base_simp_gpa = virt_to_phys(hv_cpu->pv_synic_message_page)
			>> HV_HYP_PAGE_SHIFT;
	err = hv_pv_set_synic_register(HV_MSR_SIMP, simp.as_uint64);
	if (err)
		return err;

	/* Setup the Synic's event page with the paravisor. */
	siefp.as_uint64 = hv_pv_get_synic_register(HV_MSR_SIEFP, &err);
	if (err)
		return err;
	siefp.siefp_enabled = 1;
	siefp.base_siefp_gpa = virt_to_phys(hv_cpu->pv_synic_event_page)
			>> HV_HYP_PAGE_SHIFT;
	return hv_pv_set_synic_register(HV_MSR_SIEFP, siefp.as_uint64);
}

static int hv_pv_synic_enable_interrupts(void)
{
	union hv_synic_scontrol sctrl;
	int err;

	/* Enable the global synic bit */
	sctrl.as_uint64 = hv_pv_get_synic_register(HV_MSR_SCONTROL, &err);
	if (err)
		return err;
	sctrl.enable = 1;

	return hv_pv_set_synic_register(HV_MSR_SCONTROL, sctrl.as_uint64);
}

int hv_synic_init(unsigned int cpu)
{
	int err = 0;

	/*
	 * The paravisor may not support the confidential VMBus,
	 * check on that first.
	 */
	if (vmbus_is_confidential())
		err = hv_pv_synic_enable_regs(cpu);
	if (err)
		return err;

	hv_synic_enable_regs(cpu);
	if (!vmbus_is_confidential())
		hv_synic_enable_interrupts();
	else
		err = hv_pv_synic_enable_interrupts();
	if (err)
		return err;

	hv_stimer_legacy_init(cpu, VMBUS_MESSAGE_SINT);

	return err;
}

void hv_synic_disable_regs(unsigned int cpu)
{
	struct hv_per_cpu_context *hv_cpu =
		per_cpu_ptr(hv_context.cpu_context, cpu);
	union hv_synic_sint shared_sint;
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;

	shared_sint.as_uint64 = hv_get_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT);

	shared_sint.masked = 1;

	/* Need to correctly cleanup in the case of SMP!!! */
	/* Disable the interrupt */
	hv_set_msr(HV_MSR_SINT0 + VMBUS_MESSAGE_SINT, shared_sint.as_uint64);

	simp.as_uint64 = hv_get_msr(HV_MSR_SIMP);
	/*
	 * In Isolation VM, sim and sief pages are allocated by
	 * paravisor. These pages also will be used by kdump
	 * kernel. So just reset enable bit here and keep page
	 * addresses.
	 */
	simp.simp_enabled = 0;
	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		memunmap(hv_cpu->hv_synic_message_page);
		hv_cpu->hv_synic_message_page = NULL;
	} else {
		simp.base_simp_gpa = 0;
	}

	hv_set_msr(HV_MSR_SIMP, simp.as_uint64);

	siefp.as_uint64 = hv_get_msr(HV_MSR_SIEFP);
	siefp.siefp_enabled = 0;

	if (ms_hyperv.paravisor_present || hv_root_partition()) {
		memunmap(hv_cpu->hv_synic_event_page);
		hv_cpu->hv_synic_event_page = NULL;
	} else {
		siefp.base_siefp_gpa = 0;
	}

	hv_set_msr(HV_MSR_SIEFP, siefp.as_uint64);
}

static void hv_synic_disable_interrupts(void)
{
	union hv_synic_scontrol sctrl;

	/* Disable the global synic bit */
	sctrl.as_uint64 = hv_get_msr(HV_MSR_SCONTROL);
	sctrl.enable = 0;
	hv_set_msr(HV_MSR_SCONTROL, sctrl.as_uint64);
}

static void hv_vmbus_disable_percpu_interrupts(void)
{
	if (vmbus_irq != -1)
		disable_percpu_irq(vmbus_irq);
}

static void hv_pv_synic_disable_regs(unsigned int cpu)
{
	/*
	 * The get/set register errors are deliberatley ignored in
	 * the cleanup path as they are non-consequential here.
	 */
	int err;
	union hv_synic_simp simp;
	union hv_synic_siefp siefp;

	struct hv_per_cpu_context *hv_cpu
		= per_cpu_ptr(hv_context.cpu_context, cpu);

	/* Disable SynIC's message page in the paravisor. */
	simp.as_uint64 = hv_pv_get_synic_register(HV_MSR_SIMP, &err);
	if (err)
		return;
	simp.simp_enabled = 0;

	memunmap(hv_cpu->pv_synic_message_page);
	hv_cpu->pv_synic_message_page = NULL;

	err = hv_pv_set_synic_register(HV_MSR_SIMP, simp.as_uint64);
	if (err)
		return;

	/* Disable SynIC's event page in the paravisor. */
	siefp.as_uint64 = hv_pv_get_synic_register(HV_MSR_SIEFP, &err);
	if (err)
		return;
	siefp.siefp_enabled = 0;

	memunmap(hv_cpu->pv_synic_event_page);
	hv_cpu->pv_synic_event_page = NULL;

	hv_pv_set_synic_register(HV_MSR_SIEFP, siefp.as_uint64);
}

static void hv_pv_synic_disable_interrupts(void)
{
	union hv_synic_scontrol sctrl;
	int err;

	/* Disable the global synic bit */
	sctrl.as_uint64 = hv_pv_get_synic_register(HV_MSR_SCONTROL, &err);
	if (err)
		return;
	sctrl.enable = 0;
	hv_pv_set_synic_register(HV_MSR_SCONTROL, sctrl.as_uint64);
}

#define HV_MAX_TRIES 3

static bool hv_synic_event_pending_for(union hv_synic_event_flags *event, int sint)
{
	unsigned long *recv_int_page;
	bool pending;
	u32 relid;
	int tries;

	if (!event)
		return false;

	tries = 0;
	event += sint;
	recv_int_page = event->flags; /* assumes VMBus version >= VERSION_WIN8 */
retry:
	pending = false;
	for_each_set_bit(relid, recv_int_page, HV_EVENT_FLAGS_COUNT) {
		/* Special case - VMBus channel protocol messages */
		if (relid == 0)
			continue;
		pending = true;
		break;
	}
	if (pending && tries++ < HV_MAX_TRIES) {
		usleep_range(10000, 20000);
		goto retry;
	}
	return pending;
}

static int hv_pick_new_cpu(struct vmbus_channel *channel)
{
	int ret = -EBUSY;
	int start;
	int cpu;

	lockdep_assert_cpus_held();
	lockdep_assert_held(&vmbus_connection.channel_mutex);

	/*
	 * We can't assume that the relevant interrupts will be sent before
	 * the cpu is offlined on older versions of hyperv.
	 */
	if (vmbus_proto_version < VERSION_WIN10_V5_3)
		return -EBUSY;

	start = get_random_u32_below(nr_cpu_ids);

	for_each_cpu_wrap(cpu, cpu_online_mask, start) {
		if (channel->target_cpu == cpu ||
		    channel->target_cpu == VMBUS_CONNECT_CPU)
			continue;

		ret = vmbus_channel_set_cpu(channel, cpu);
		if (!ret)
			break;
	}

	if (ret)
		ret = vmbus_channel_set_cpu(channel, VMBUS_CONNECT_CPU);

	return ret;
}

/*
 * hv_synic_cleanup - Cleanup routine for hv_synic_init().
 */
/*
 * Scan the event flags page of 'this' CPU looking for any bit that is set.  If we find one
 * bit set, then wait for a few milliseconds.  Repeat these steps for a maximum of 3 times.
 * Return 'true', if there is still any set bit after this operation; 'false', otherwise.
 *
 * If a bit is set, that means there is a pending channel interrupt.  The expectation is
 * that the normal interrupt handling mechanism will find and process the channel interrupt
 * "very soon", and in the process clear the bit.
 */
static bool hv_synic_event_pending(void)
{
	struct hv_per_cpu_context *hv_cpu = this_cpu_ptr(hv_context.cpu_context);
	union hv_synic_event_flags *hv_synic_event_page = hv_cpu->hv_synic_event_page;
	union hv_synic_event_flags *pv_synic_event_page = hv_cpu->pv_synic_event_page;

	return
		hv_synic_event_pending_for(hv_synic_event_page, VMBUS_MESSAGE_SINT) ||
		hv_synic_event_pending_for(pv_synic_event_page, VMBUS_MESSAGE_SINT);
}

int hv_synic_cleanup(unsigned int cpu)
{
	struct vmbus_channel *channel, *sc;
	int ret = 0;

	if (vmbus_connection.conn_state != CONNECTED)
		goto always_cleanup;

	/*
	 * Hyper-V does not provide a way to change the connect CPU once
	 * it is set; we must prevent the connect CPU from going offline
	 * while the VM is running normally. But in the panic or kexec()
	 * path where the vmbus is already disconnected, the CPU must be
	 * allowed to shut down.
	 */
	if (cpu == VMBUS_CONNECT_CPU)
		return -EBUSY;

	/*
	 * Search for channels which are bound to the CPU we're about to
	 * cleanup.
	 */
	mutex_lock(&vmbus_connection.channel_mutex);
	list_for_each_entry(channel, &vmbus_connection.chn_list, listentry) {
		if (channel->target_cpu == cpu) {
			ret = hv_pick_new_cpu(channel);
			if (ret) {
				mutex_unlock(&vmbus_connection.channel_mutex);
				return ret;
			}
		}
		list_for_each_entry(sc, &channel->sc_list, sc_list) {
			if (sc->target_cpu == cpu) {
				ret = hv_pick_new_cpu(sc);
				if (ret) {
					mutex_unlock(&vmbus_connection.channel_mutex);
					return ret;
				}
			}
		}
	}
	mutex_unlock(&vmbus_connection.channel_mutex);

	/*
	 * Scan the event flags page looking for bits that are set and waiting
	 * with a timeout for vmbus_chan_sched() to process such bits. If bits
	 * are still set after this operation and VMBus is connected, fail the
	 * CPU offlining operation.
	 */
	if (vmbus_proto_version >= VERSION_WIN10_V4_1 && hv_synic_event_pending())
		return -EBUSY;

always_cleanup:
	hv_stimer_legacy_cleanup(cpu);

	hv_synic_disable_regs(cpu);
	if (vmbus_is_confidential())
		hv_pv_synic_disable_regs(cpu);
	if (!vmbus_is_confidential())
		hv_synic_disable_interrupts();
	else
		hv_pv_synic_disable_interrupts();
	hv_vmbus_disable_percpu_interrupts();

	return ret;
}
