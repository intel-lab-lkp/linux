// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

#define TVA_IN_PF (1 << 1)

int pf_alloc, pf_disallow, khugepaged_alloc, khugepaged_disallow;
struct mm_struct *target_mm;
int pmd_order, cgrp_id;

/* Detecting whether a task can successfully allocate THP is unreliable because
 * it may be influenced by system memory pressure. Instead of making the result
 * dependent on unpredictable factors, we should simply check
 * get_suggested_order()'s return value, which is deterministic.
 */
SEC("fexit/get_suggested_order")
int BPF_PROG(thp_run, struct mm_struct *mm, struct vm_area_struct *vma__nullable,
	     u64 vma_flags, u64 tva_flags, int orders, int retval)
{
	if (mm != target_mm)
		return 0;

	if (orders != (1 << pmd_order))
		return 0;

	if (tva_flags == TVA_PAGEFAULT) {
		if (retval == (1 << pmd_order))
			pf_alloc++;
		else if (!retval)
			pf_disallow++;
	} else if (tva_flags == TVA_KHUGEPAGED || tva_flags == -1) {
		if (retval == (1 << pmd_order))
			khugepaged_alloc++;
		else if (!retval)
			khugepaged_disallow++;
	}
	return 0;
}

SEC("struct_ops/get_suggested_order")
int BPF_PROG(bpf_suggested_order, struct mm_struct *mm, struct vm_area_struct *vma__nullable,
	     u64 vma_flags, enum tva_type tva_flags, int orders)
{
	struct mem_cgroup *memcg = bpf_mm_get_mem_cgroup(mm);
	int suggested_orders = 0;

	/* Only works when CONFIG_MEMCG is enabled. */
	if (!memcg)
		return suggested_orders;

	if (memcg->css.cgroup->kn->id == cgrp_id) {
		if (!target_mm)
			target_mm = mm;
		/* BPF THP allocation policy:
		 * - Allow PMD allocation in khugepagd only
		 */
		if ((tva_flags == TVA_KHUGEPAGED || tva_flags == -1) &&
		    orders == (1 << pmd_order)) {
			suggested_orders = orders;
			goto out;
		}
	}

out:
	bpf_put_mem_cgroup(memcg);
	return suggested_orders;
}

SEC(".struct_ops.link")
struct bpf_thp_ops thp = {
	.get_suggested_order = (void *)bpf_suggested_order,
};
