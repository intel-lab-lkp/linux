// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/clockchips.h>
#include <linux/slab.h>
#include <linux/cpuhotplug.h>
#include <linux/minmax.h>
#include <linux/export.h>
#include <asm/mshyperv.h>

int hv_call_deposit_pages(int node, u64 partition_id, u32 num_pages)
{
	return -ENOTSUPP;
}
EXPORT_SYMBOL_GPL(hv_call_deposit_pages);

int hv_deposit_memory_node(int node, u64 partition_id, u64 hv_status)
{
	return -ENOTSUPP;
}
EXPORT_SYMBOL_GPL(hv_deposit_memory_node);

bool hv_result_needs_memory(u64 status)
{
	switch (hv_result(status)) {
	case HV_STATUS_INSUFFICIENT_MEMORY:
	case HV_STATUS_INSUFFICIENT_CONTIGUOUS_MEMORY:
	case HV_STATUS_INSUFFICIENT_ROOT_MEMORY:
	case HV_STATUS_INSUFFICIENT_CONTIGUOUS_ROOT_MEMORY:
		return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(hv_result_needs_memory);

int hv_call_add_logical_proc(int node, u32 lp_index, u32 apic_id)
{
	struct hv_input_add_logical_processor *input;
	struct hv_output_add_logical_processor *output;
	u64 status;
	unsigned long flags;
	int ret = 0;

	/*
	 * When adding a logical processor, the hypervisor may return
	 * HV_STATUS_INSUFFICIENT_MEMORY. When that happens, we deposit more
	 * pages and retry.
	 */
	do {
		local_irq_save(flags);

		input = *this_cpu_ptr(hyperv_pcpu_input_arg);
		/* We don't do anything with the output right now */
		output = *this_cpu_ptr(hyperv_pcpu_output_arg);

		input->lp_index = lp_index;
		input->apic_id = apic_id;
		input->proximity_domain_info = hv_numa_node_to_pxm_info(node);
		status = hv_do_hypercall(HVCALL_ADD_LOGICAL_PROCESSOR,
					 input, output);
		local_irq_restore(flags);

		if (!hv_result_needs_memory(status)) {
			if (!hv_result_success(status)) {
				hv_status_err(status, "cpu %u apic ID: %u\n",
					      lp_index, apic_id);
				ret = hv_result_to_errno(status);
			}
			break;
		}
		ret = hv_deposit_memory_node(node, hv_current_partition_id,
					     status);
	} while (!ret);

	return ret;
}

int hv_call_create_vp(int node, u64 partition_id, u32 vp_index, u32 flags)
{
	struct hv_create_vp *input;
	u64 status;
	unsigned long irq_flags;
	int ret = 0;

	/* Root VPs don't seem to need pages deposited */
	if (partition_id != hv_current_partition_id) {
		/* The value 90 is empirically determined. It may change. */
		ret = hv_call_deposit_pages(node, partition_id, 90);
		if (ret)
			return ret;
	}

	do {
		local_irq_save(irq_flags);

		input = *this_cpu_ptr(hyperv_pcpu_input_arg);

		input->partition_id = partition_id;
		input->vp_index = vp_index;
		input->flags = flags;
		input->subnode_type = HV_SUBNODE_ANY;
		input->proximity_domain_info = hv_numa_node_to_pxm_info(node);
		status = hv_do_hypercall(HVCALL_CREATE_VP, input, NULL);
		local_irq_restore(irq_flags);

		if (!hv_result_needs_memory(status)) {
			if (!hv_result_success(status)) {
				hv_status_err(status, "vcpu: %u, lp: %u\n",
					      vp_index, flags);
				ret = hv_result_to_errno(status);
			}
			break;
		}
		ret = hv_deposit_memory_node(node, partition_id, status);

	} while (!ret);

	return ret;
}
EXPORT_SYMBOL_GPL(hv_call_create_vp);

int hv_call_notify_all_processors_started(void)
{
	struct hv_input_notify_partition_event *input;
	u64 status;
	unsigned long irq_flags;
	int ret = 0;

	local_irq_save(irq_flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(input, 0, sizeof(*input));
	input->event = HV_PARTITION_ALL_LOGICAL_PROCESSORS_STARTED;
	status = hv_do_hypercall(HVCALL_NOTIFY_PARTITION_EVENT,
				 input, NULL);
	local_irq_restore(irq_flags);

	if (!hv_result_success(status)) {
		hv_status_err(status, "\n");
		ret = hv_result_to_errno(status);
	}
	return ret;
}

bool hv_lp_exists(u32 lp_index)
{
	struct hv_input_get_logical_processor_run_time *input;
	struct hv_output_get_logical_processor_run_time *output;
	unsigned long flags;
	u64 status;

	local_irq_save(flags);
	input = *this_cpu_ptr(hyperv_pcpu_input_arg);
	output = *this_cpu_ptr(hyperv_pcpu_output_arg);

	input->lp_index = lp_index;
	status = hv_do_hypercall(HVCALL_GET_LOGICAL_PROCESSOR_RUN_TIME,
				 input, output);
	local_irq_restore(flags);

	if (!hv_result_success(status) &&
	    hv_result(status) != HV_STATUS_INVALID_LP_INDEX) {
		hv_status_err(status, "\n");
		BUG();
	}

	return hv_result_success(status);
}
