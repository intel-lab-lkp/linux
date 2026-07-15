// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 */

#include <linux/cpufeature.h>
#include <linux/memblock.h>
#include <linux/arm-rmi-cmds.h>
#include <linux/processor.h>
#include <linux/slab.h>

#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>

/* Currently only the first 2 registers are used by Linux */
#define RMI_FEAT_REG_COUNT	2
static __ro_after_init unsigned long rmi_feat_reg_cache[RMI_FEAT_REG_COUNT];

unsigned long rmi_feat_reg(unsigned long id)
{
	if (WARN_ON(id >= RMI_FEAT_REG_COUNT))
		return 0;

	return rmi_feat_reg_cache[id];
}
EXPORT_SYMBOL_GPL(rmi_feat_reg);

int rmi_delegate_range(phys_addr_t phys,
		       unsigned long size,
		       phys_addr_t *out_phys)
{
	unsigned long ret = 0;
	unsigned long top = phys + size;
	unsigned long out_top;

	while (phys < top) {
		ret = rmi_granule_range_delegate(phys, top, &out_top);
		if (ret == RMI_SUCCESS)
			phys = out_top;
		else if (ret == RMI_BUSY || ret == RMI_BLOCKED)
			cpu_relax();
		else
			break;
	}

	if (out_phys)
		*out_phys = phys;

	return ret;
}
EXPORT_SYMBOL_GPL(rmi_delegate_range);

int rmi_undelegate_range(phys_addr_t phys,
			 unsigned long size)
{
	unsigned long ret = 0;
	unsigned long top = phys + size;
	unsigned long out_top;

	while (phys < top) {
		ret = rmi_granule_range_undelegate(phys, top, &out_top);
		if (ret == RMI_SUCCESS)
			phys = out_top;
		else if (ret == RMI_BUSY || ret == RMI_BLOCKED)
			cpu_relax();
		else
			break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(rmi_undelegate_range);

static unsigned long donate_req_to_size(unsigned long donatereq)
{
	unsigned long unit_size = RMI_DONATE_SIZE(donatereq);

	return BIT(ARM64_HW_PGTABLE_LEVEL_SHIFT(3 - unit_size));
}

static void rmi_smccc_invoke(struct arm_smccc_1_2_regs *regs_in,
			     struct arm_smccc_1_2_regs *regs_out)
{
	struct arm_smccc_1_2_regs regs = *regs_in;
	unsigned long status;

	while (1) {
		arm_smccc_1_2_invoke(&regs, regs_out);
		status = RMI_RETURN_STATUS(regs_out->a0);
		if (status != RMI_BUSY && status != RMI_BLOCKED)
			break;
		cpu_relax();
	}
}

static void rmi_op_continue(unsigned long sro_handle, unsigned long flags,
			    struct arm_smccc_1_2_regs *out_regs)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_CONTINUE, sro_handle, flags
	};

	rmi_smccc_invoke(&regs, out_regs);
}

static void rmi_op_cancel(unsigned long sro_handle,
			  struct arm_smccc_1_2_regs *out_regs)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_CANCEL, sro_handle
	};

	rmi_smccc_invoke(&regs, out_regs);
}

static void rmi_op_mem_donate(unsigned long sro_handle, unsigned long list_addr,
			      unsigned long list_count, unsigned long flags,
			      struct arm_smccc_1_2_regs *out_regs)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_MEM_DONATE, sro_handle, list_addr, list_count, flags
	};

	rmi_smccc_invoke(&regs, out_regs);
}

static void rmi_op_mem_reclaim(unsigned long sro_handle,
			       unsigned long list_addr,
			       unsigned long list_count,
			       struct arm_smccc_1_2_regs *out_regs)
{
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_MEM_RECLAIM, sro_handle, list_addr, list_count
	};

	rmi_smccc_invoke(&regs, out_regs);
}

int free_delegated_page(phys_addr_t phys)
{
	if (WARN_ON_ONCE(rmi_undelegate_page(phys))) {
		/* Undelegate failed: leak the page */
		return -EBUSY;
	}

	free_page((unsigned long)phys_to_virt(phys));

	return 0;
}
EXPORT_SYMBOL_GPL(free_delegated_page);

static int rmi_sro_ensure_capacity(struct rmi_sro_state *sro,
				   unsigned long count)
{
	if (WARN_ON_ONCE(sro->addr_count > RMI_MAX_ADDR_LIST))
		return -EOVERFLOW;

	if (count > RMI_MAX_ADDR_LIST - sro->addr_count)
		return -ENOSPC;

	return 0;
}

static int rmi_sro_donate_contig(struct rmi_sro_state *sro,
				 unsigned long sro_handle,
				 unsigned long donatereq,
				 struct arm_smccc_1_2_regs *out_regs,
				 gfp_t gfp)
{
	unsigned long unit_size = RMI_DONATE_SIZE(donatereq);
	unsigned long unit_size_bytes = donate_req_to_size(donatereq);
	unsigned long count = RMI_DONATE_COUNT(donatereq);
	unsigned long state = RMI_DONATE_STATE(donatereq);
	unsigned long size = unit_size_bytes * count;
	unsigned long addr_range;
	int ret;
	void *virt;
	phys_addr_t phys;

	for (int i = 0; i < sro->addr_count; i++) {
		unsigned long entry = sro->addr_list[i];

		if (RMI_ADDR_RANGE_SIZE(entry) == unit_size &&
		    RMI_ADDR_RANGE_COUNT(entry) == count &&
		    RMI_ADDR_RANGE_STATE(entry) == state &&
		    IS_ALIGNED(RMI_ADDR_RANGE_ADDR(entry), size)) {
			sro->addr_count--;
			swap(sro->addr_list[sro->addr_count],
			     sro->addr_list[i]);

			goto out;
		}
	}

	ret = rmi_sro_ensure_capacity(sro, 1);
	if (ret)
		return ret;

	virt = alloc_pages_exact(size, gfp);
	if (!virt)
		return -ENOMEM;
	phys = virt_to_phys(virt);

	if (state == RMI_OP_MEM_DELEGATED) {
		phys_addr_t delegated_phys;

		if (rmi_delegate_range(phys, size, &delegated_phys)) {
			if (!rmi_undelegate_range(phys, delegated_phys - phys))
				free_pages_exact(virt, size);
			return -ENXIO;
		}
	}

	addr_range = phys & RMI_ADDR_RANGE_ADDR_MASK;
	FIELD_MODIFY(RMI_ADDR_RANGE_SIZE_MASK, &addr_range, unit_size);
	FIELD_MODIFY(RMI_ADDR_RANGE_COUNT_MASK, &addr_range, count);
	FIELD_MODIFY(RMI_ADDR_RANGE_STATE_MASK, &addr_range, state);

	sro->addr_list[sro->addr_count] = addr_range;

out:
	rmi_op_mem_donate(sro_handle,
			  virt_to_phys(&sro->addr_list[sro->addr_count]), 1,
			  0, out_regs);

	unsigned long donated_granules = out_regs->a1;
	unsigned long donated_size = donated_granules << PAGE_SHIFT;

	if (donated_granules == 0) {
		/* No pages used by the RMM */
		sro->addr_count++;
	} else if (donated_size < size) {
		phys = sro->addr_list[sro->addr_count] & RMI_ADDR_RANGE_ADDR_MASK;

		/* Not all granules used by the RMM, free the remaining pages */
		for (long i = donated_size; i < size; i += PAGE_SIZE) {
			if (state == RMI_OP_MEM_DELEGATED)
				free_delegated_page(phys + i);
			else
				__free_page(phys_to_page(phys + i));
		}
	}

	return 0;
}

static int rmi_sro_donate_noncontig(struct rmi_sro_state *sro,
				    unsigned long sro_handle,
				    unsigned long donatereq,
				    struct arm_smccc_1_2_regs *out_regs,
				    gfp_t gfp)
{
	unsigned long unit_size = RMI_DONATE_SIZE(donatereq);
	unsigned long unit_size_bytes = donate_req_to_size(donatereq);
	unsigned long count = RMI_DONATE_COUNT(donatereq);
	unsigned long state = RMI_DONATE_STATE(donatereq);
	unsigned long found = 0;
	unsigned long addr_list_start = sro->addr_count;
	int ret;

	for (int i = 0; i < addr_list_start && found < count; i++) {
		unsigned long entry = sro->addr_list[i];

		if (RMI_ADDR_RANGE_SIZE(entry) == unit_size &&
		    RMI_ADDR_RANGE_COUNT(entry) == 1 &&
		    RMI_ADDR_RANGE_STATE(entry) == state) {
			addr_list_start--;
			swap(sro->addr_list[addr_list_start],
			     sro->addr_list[i]);
			found++;
			i--;
		}
	}

	ret = rmi_sro_ensure_capacity(sro, count - found);
	if (ret)
		return ret;

	while (found < count) {
		unsigned long addr_range;
		void *virt = alloc_pages_exact(unit_size_bytes, gfp);
		phys_addr_t phys;

		if (!virt)
			return -ENOMEM;

		phys = virt_to_phys(virt);

		if (state == RMI_OP_MEM_DELEGATED) {
			phys_addr_t delegated_phys;

			if (rmi_delegate_range(phys, unit_size_bytes,
					       &delegated_phys)) {
				if (!rmi_undelegate_range(phys, delegated_phys - phys))
					free_pages_exact(virt, unit_size_bytes);
				return -ENXIO;
			}
		}

		addr_range = phys & RMI_ADDR_RANGE_ADDR_MASK;
		FIELD_MODIFY(RMI_ADDR_RANGE_SIZE_MASK, &addr_range, unit_size);
		FIELD_MODIFY(RMI_ADDR_RANGE_COUNT_MASK, &addr_range, 1);
		FIELD_MODIFY(RMI_ADDR_RANGE_STATE_MASK, &addr_range, state);

		sro->addr_list[sro->addr_count++] = addr_range;
		found++;
	}

	rmi_op_mem_donate(sro_handle,
			  virt_to_phys(&sro->addr_list[addr_list_start]),
			  found, 0, out_regs);

	unsigned long donated_granules = out_regs->a1;
	unsigned long granules_per_unit = unit_size_bytes >> PAGE_SHIFT;
	unsigned long consumed_units;

	/*
	 * The RMM shouldn't report more granules than we provided, but clamp
	 * just in case.
	 */
	if (WARN_ON_ONCE(donated_granules > found * granules_per_unit))
		donated_granules = found * granules_per_unit;

	/*
	 * The RMM reports the consumed memory in terms of granules, but we
	 * track in the address lists in unit-sized ranges. So divide to get
	 * the number of (complete) consumed units.
	 */
	consumed_units = donated_granules / granules_per_unit;
	if (donated_granules % granules_per_unit) {
		/*
		 * A unit has been partially consumed, the start is owned by
		 * the RMM, the tail is owned by the host
		 */
		unsigned long entry =
			sro->addr_list[addr_list_start + consumed_units];
		phys_addr_t phys = RMI_ADDR_RANGE_ADDR(entry);
		unsigned long donated_size =
			(donated_granules % granules_per_unit) << PAGE_SHIFT;

		/* Free the tail back */
		for (unsigned long i = donated_size; i < unit_size_bytes;
		     i += PAGE_SIZE) {
			if (state == RMI_OP_MEM_DELEGATED)
				free_delegated_page(phys + i);
			else
				__free_page(phys_to_page(phys + i));
		}

		/*
		 * This unit is now fully 'consumed' (either held by the RMM or
		 * freed)
		 */
		consumed_units++;
	}

	/* Keep just the units the RMM didn't use in addr_list */
	for (unsigned long i = consumed_units; i < found; i++)
		sro->addr_list[addr_list_start + i - consumed_units] =
			sro->addr_list[addr_list_start + i];

	sro->addr_count -= consumed_units;

	return 0;
}

static int rmi_sro_donate(struct rmi_sro_state *sro,
			  unsigned long sro_handle,
			  unsigned long donatereq,
			  struct arm_smccc_1_2_regs *regs,
			  gfp_t gfp)
{
	unsigned long count = RMI_DONATE_COUNT(donatereq);

	if (WARN_ON_ONCE(!count))
		return 0;

	if (RMI_DONATE_CONTIG(donatereq)) {
		return rmi_sro_donate_contig(sro, sro_handle, donatereq,
					     regs, gfp);
	} else {
		return rmi_sro_donate_noncontig(sro, sro_handle, donatereq,
						regs, gfp);
	}
}

static int rmi_sro_reclaim(struct rmi_sro_state *sro,
			   unsigned long sro_handle,
			   struct arm_smccc_1_2_regs *out_regs)
{
	unsigned long capacity;
	int ret;

	ret = rmi_sro_ensure_capacity(sro, 1);
	if (ret)
		rmi_sro_free(sro);

	capacity = RMI_MAX_ADDR_LIST - sro->addr_count;

	rmi_op_mem_reclaim(sro_handle,
			   virt_to_phys(&sro->addr_list[sro->addr_count]),
			   capacity, out_regs);

	if (WARN_ON_ONCE(out_regs->a1 > capacity))
		out_regs->a1 = capacity;

	sro->addr_count += out_regs->a1;

	return 0;
}

void rmi_sro_free(struct rmi_sro_state *sro)
{
	for (int i = 0; i < sro->addr_count; i++) {
		unsigned long entry = sro->addr_list[i];
		unsigned long addr = RMI_ADDR_RANGE_ADDR(entry);
		unsigned long unit_size = RMI_ADDR_RANGE_SIZE(entry);
		unsigned long count = RMI_ADDR_RANGE_COUNT(entry);
		unsigned long state = RMI_ADDR_RANGE_STATE(entry);
		unsigned long size = donate_req_to_size(unit_size) * count;

		if (state == RMI_OP_MEM_DELEGATED) {
			if (WARN_ON_ONCE(rmi_undelegate_range(addr, size))) {
				/* Leak the pages */
				continue;
			}
		}
		free_pages_exact(phys_to_virt(addr), size);
	}

	sro->addr_count = 0;
}
EXPORT_SYMBOL_GPL(rmi_sro_free);

unsigned long rmi_sro_memxfer_execute(struct rmi_sro_state *sro, gfp_t gfp)
{
	unsigned long sro_handle;
	struct arm_smccc_1_2_regs *regs = &sro->regs;

	rmi_smccc_invoke(regs, regs);

	sro_handle = regs->a1;

	while (RMI_RETURN_STATUS(regs->a0) == RMI_INCOMPLETE) {
		bool can_cancel = RMI_RETURN_CAN_CANCEL(regs->a0);
		int ret = 0;

		switch (RMI_RETURN_MEMREQ(regs->a0)) {
		case RMI_OP_MEM_REQ_NONE:
			rmi_op_continue(sro_handle, RMI_CONTINUE_KEEP_GOING,
					regs);
			break;
		case RMI_OP_MEM_REQ_DONATE:
			ret = rmi_sro_donate(sro, sro_handle, regs->a2, regs,
					     gfp);
			break;
		case RMI_OP_MEM_REQ_RECLAIM:
			ret = rmi_sro_reclaim(sro, sro_handle, regs);
			break;
		default:
			ret = WARN_ON_ONCE(1);
			break;
		}

		if (ret) {
			if (can_cancel)
				rmi_op_cancel(sro_handle, regs);

			if (WARN_ON_ONCE(RMI_RETURN_STATUS(regs->a0) != RMI_INCOMPLETE))
				return ret;
		}
	}

	return regs->a0;
}
EXPORT_SYMBOL_GPL(rmi_sro_memxfer_execute);

/* For RMI commands that are stateful but not memory-transferring */
unsigned long rmi_sro_execute(struct arm_smccc_1_2_regs *regs)
{
	unsigned long sro_handle;

	rmi_smccc_invoke(regs, regs);

	sro_handle = regs->a1;

	while (RMI_RETURN_STATUS(regs->a0) == RMI_INCOMPLETE) {
		bool can_cancel = RMI_RETURN_CAN_CANCEL(regs->a0);

		switch (RMI_RETURN_MEMREQ(regs->a0)) {
		case RMI_OP_MEM_REQ_NONE:
			rmi_op_continue(sro_handle, RMI_CONTINUE_KEEP_GOING,
					regs);
			break;
		default:
			WARN_ON_ONCE(1);
			if (!can_cancel)
				return regs->a0;

			rmi_op_cancel(sro_handle, regs);
		}
	}

	return regs->a0;
}
EXPORT_SYMBOL_GPL(rmi_sro_execute);

static int rmi_check_version(void)
{
	struct arm_smccc_res res;
	unsigned short version_major, version_minor;
	unsigned long host_version = RMI_ABI_VERSION(RMI_ABI_MAJOR_VERSION,
						     RMI_ABI_MINOR_VERSION);
	unsigned long aa64pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	/* If RME isn't supported, then RMI can't be */
	if (cpuid_feature_extract_unsigned_field(aa64pfr0, ID_AA64PFR0_EL1_RME_SHIFT) == 0)
		return -ENXIO;

	arm_smccc_1_1_invoke(SMC_RMI_VERSION, host_version, &res);

	if (res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return -ENXIO;

	version_major = RMI_ABI_VERSION_GET_MAJOR(res.a1);
	version_minor = RMI_ABI_VERSION_GET_MINOR(res.a1);

	if (res.a0 != RMI_SUCCESS) {
		unsigned short high_version_major, high_version_minor;

		high_version_major = RMI_ABI_VERSION_GET_MAJOR(res.a2);
		high_version_minor = RMI_ABI_VERSION_GET_MINOR(res.a2);

		pr_err("Unsupported RMI ABI (v%d.%d - v%d.%d) we want v%d.%d\n",
		       version_major, version_minor,
		       high_version_major, high_version_minor,
		       RMI_ABI_MAJOR_VERSION,
		       RMI_ABI_MINOR_VERSION);
		return -ENXIO;
	}

	pr_info("RMI ABI version %d.%d\n", version_major, version_minor);

	return 0;
}

static int rmi_read_features(void)
{
	/*
	 * Since we've negotiated a compatible version these feature registers
	 * should always be available
	 */
	for (int i = 0; i < RMI_FEAT_REG_COUNT; i++) {
		if (WARN_ON(rmi_features(i, &rmi_feat_reg_cache[i])))
			return -EINVAL;
	}

	return 0;
}

static int rmi_configure(void)
{
	unsigned long ret;
	struct rmm_config *config __free(free_page) = (struct rmm_config *)get_zeroed_page(GFP_KERNEL);
	struct rmi_sro_state *sro __free(kfree) = (struct rmi_sro_state *)kmalloc_obj(*sro);

	if (!config || !sro)
		return -ENOMEM;

	switch (PAGE_SIZE) {
	case SZ_4K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_4KB;
		break;
	case SZ_16K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_16KB;
		break;
	case SZ_64K:
		config->rmi_granule_size = RMI_GRANULE_SIZE_64KB;
		break;
	default:
		BUILD_BUG();
	}

	/*
	 * For now we set the tracking_region_size to 0 which is the only option
	 * for 4KB PAGE_SIZE (1GB for 4KB PAGE_SIZE, 32MB/512MB for 16KB/64KB).
	 * TODO: Support other tracking sizes via Kconfig option for other
	 * PAGE_SIZES
	 */
	config->tracking_region_size = 0;

	ret = rmi_rmm_config_set(virt_to_phys(config));
	if (ret) {
		pr_err("RMM config set failed\n");
		return -EINVAL;
	}

	ret = rmi_rmm_activate(sro);
	if (ret) {
		pr_err("RMM activate failed\n");
		return -ENXIO;
	}

	return 0;
}

static int __init arm64_init_rmi(void)
{
	int ret;

	/* Continue without realm support if we can't agree on a version */
	ret = rmi_check_version();
	if (ret)
		return ret;

	ret = rmi_read_features();
	if (ret)
		return ret;

	ret = rmi_configure();
	if (ret)
		return ret;

	return 0;
}

/*
 * Note arm64_init_rmi() must be called before kvm_init_rmi() otherwise KVM
 * will not support realm guests. subsys_initcall() is called before
 * module_init() (used for KVM) so this is OK.
 */
subsys_initcall(arm64_init_rmi);
