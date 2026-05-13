// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/memblock.h>

#include <asm/rmi_cmds.h>

static bool arm64_rmi_is_available;

unsigned long rmm_feat_reg0;
unsigned long rmm_feat_reg1;

int rmi_delegate_range(phys_addr_t phys, unsigned long size)
{
	unsigned long ret = 0;
	unsigned long top = phys + size;
	unsigned long out_top;

	while (phys < top) {
		ret = rmi_granule_range_delegate(phys, top, &out_top);
		if (ret == RMI_SUCCESS)
			phys = out_top;
		else if (ret != RMI_BUSY && ret != RMI_BLOCKED)
			return ret;
	}

	return ret;
}

int rmi_undelegate_range(phys_addr_t phys, unsigned long size)
{
	unsigned long ret = 0;
	unsigned long top = phys + size;
	unsigned long out_top;

	WARN_ON(size == 0);

	while (phys < top) {
		ret = rmi_granule_range_undelegate(phys, top, &out_top);
		if (ret == RMI_SUCCESS)
			phys = out_top;
		else if (ret != RMI_BUSY && ret != RMI_BLOCKED)
			return ret;
	}

	return ret;
}

static unsigned long donate_req_to_size(unsigned long donatereq)
{
	unsigned long unit_size = RMI_DONATE_SIZE(donatereq);

	switch (unit_size) {
	case 0:
		return PAGE_SIZE;
	case 1:
		return PMD_SIZE;
	case 2:
		return PUD_SIZE;
	case 3:
		return P4D_SIZE;
	}
	unreachable();
}

static void rmi_smccc_invoke(struct arm_smccc_1_2_regs *regs_in,
			     struct arm_smccc_1_2_regs *regs_out)
{
	struct arm_smccc_1_2_regs regs = *regs_in;
	unsigned long status;

	do {
		arm_smccc_1_2_invoke(&regs, regs_out);
		status = RMI_RETURN_STATUS(regs_out->a0);
	} while (status == RMI_BUSY || status == RMI_BLOCKED);
}

int free_delegated_page(phys_addr_t phys)
{
	if (WARN_ON(rmi_undelegate_page(phys))) {
		/* Undelegate failed: leak the page */
		return -EBUSY;
	}

	free_page((unsigned long)phys_to_virt(phys));

	return 0;
}

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
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_MEM_DONATE,
		sro_handle
	};

	for (int i = 0; i < sro->addr_count; i++) {
		unsigned long entry = sro->addr_list[i];

		if (RMI_ADDR_RANGE_SIZE(entry) == unit_size &&
		    RMI_ADDR_RANGE_COUNT(entry) == count &&
		    RMI_ADDR_RANGE_STATE(entry) == state) {
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
		if (rmi_delegate_range(phys, size)) {
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
	regs.a2 = virt_to_phys(&sro->addr_list[sro->addr_count]);
	regs.a3 = 1;
	rmi_smccc_invoke(&regs, out_regs);

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
	struct arm_smccc_1_2_regs regs = {
		SMC_RMI_OP_MEM_DONATE,
		sro_handle
	};

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
			if (rmi_delegate_range(phys, unit_size_bytes)) {
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

	regs.a2 = virt_to_phys(&sro->addr_list[addr_list_start]);
	regs.a3 = found;
	rmi_smccc_invoke(&regs, out_regs);

	unsigned long donated_granules = out_regs->a1;

	if (WARN_ON(donated_granules & ((unit_size_bytes >> PAGE_SHIFT) - 1))) {
		/*
		 * FIXME: RMM has only consumed part of a huge page, this leaks
		 * the rest of the huge page
		 */
		donated_granules = ALIGN(donated_granules,
					 (unit_size_bytes >> PAGE_SHIFT));
	}
	unsigned long donated_blocks = donated_granules / (unit_size_bytes >> PAGE_SHIFT);

	if (WARN_ON(donated_blocks > found))
		donated_blocks = found;

	unsigned long undonated_blocks = found - donated_blocks;

	while (donated_blocks && undonated_blocks) {
		sro->addr_count--;
		swap(sro->addr_list[addr_list_start],
		     sro->addr_list[sro->addr_count]);
		addr_list_start++;

		donated_blocks--;
		undonated_blocks--;
	}
	sro->addr_count -= donated_blocks;

	return 0;
}

static int rmi_sro_donate(struct rmi_sro_state *sro,
			  unsigned long sro_handle,
			  unsigned long donatereq,
			  struct arm_smccc_1_2_regs *regs,
			  gfp_t gfp)
{
	unsigned long count = RMI_DONATE_COUNT(donatereq);

	if (WARN_ON(!count))
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
	struct arm_smccc_1_2_regs regs;
	int ret;

	ret = rmi_sro_ensure_capacity(sro, 1);
	if (ret)
		rmi_sro_free(sro);

	capacity = RMI_MAX_ADDR_LIST - sro->addr_count;

	regs = (struct arm_smccc_1_2_regs){
		SMC_RMI_OP_MEM_RECLAIM,
		sro_handle,
		virt_to_phys(&sro->addr_list[sro->addr_count]),
		capacity
	};
	rmi_smccc_invoke(&regs, out_regs);

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
			if (WARN_ON(rmi_undelegate_range(addr, size))) {
				/* Leak the pages */
				continue;
			}
		}
		free_pages_exact(phys_to_virt(addr), size);
	}

	sro->addr_count = 0;
}

unsigned long rmi_sro_execute(struct rmi_sro_state *sro, gfp_t gfp)
{
	unsigned long sro_handle;
	struct arm_smccc_1_2_regs regs;
	struct arm_smccc_1_2_regs *regs_in = &sro->regs;

	rmi_smccc_invoke(regs_in, &regs);

	sro_handle = regs.a1;

	while (RMI_RETURN_STATUS(regs.a0) == RMI_INCOMPLETE) {
		bool can_cancel = RMI_RETURN_CAN_CANCEL(regs.a0);
		int ret;

		switch (RMI_RETURN_MEMREQ(regs.a0)) {
		case RMI_OP_MEM_REQ_NONE:
			regs = (struct arm_smccc_1_2_regs){
				SMC_RMI_OP_CONTINUE, sro_handle, 0
			};
			rmi_smccc_invoke(&regs, &regs);
			break;
		case RMI_OP_MEM_REQ_DONATE:
			ret = rmi_sro_donate(sro, sro_handle, regs.a2, &regs,
					     gfp);
			break;
		case RMI_OP_MEM_REQ_RECLAIM:
			ret = rmi_sro_reclaim(sro, sro_handle, &regs);
			break;
		default:
			ret = WARN_ON(1);
			break;
		}

		if (ret) {
			if (can_cancel) {
				/*
				 * FIXME: Handle cancelling properly!
				 *
				 * If the operation has failed due to memory
				 * allocation failure then the information on
				 * the memory allocation should be saved, so
				 * that the allocation can be repeated outside
				 * of any context which prevented the
				 * allocation.
				 */
			}
			if (WARN_ON(ret))
				return ret;
		}
	}

	return regs.a0;
}

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

static int rmi_configure(void)
{
	struct rmm_config *config __free(free_page) = NULL;
	unsigned long ret;

	config = (struct rmm_config *)get_zeroed_page(GFP_KERNEL);
	if (!config)
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
		pr_err("Unsupported PAGE_SIZE for RMM\n");
		return -EINVAL;
	}

	ret = rmi_rmm_config_set(virt_to_phys(config));
	if (ret) {
		pr_err("RMM config set failed\n");
		return -EINVAL;
	}

	ret = rmi_rmm_activate();
	if (ret) {
		pr_err("RMM activate failed\n");
		return -ENXIO;
	}

	return 0;
}

/*
 * For now we set the tracking_region_size to 0 for RMI_RMM_CONFIG_SET().
 * TODO: Support other tracking sizes (via Kconfig option).
 */
#ifdef CONFIG_PAGE_SIZE_4KB
#define RMM_GRANULE_TRACKING_SIZE	SZ_1G
#elif defined(CONFIG_PAGE_SIZE_16KB)
#define RMM_GRANULE_TRACKING_SIZE	SZ_32M
#elif defined(CONFIG_PAGE_SIZE_64KB)
#define RMM_GRANULE_TRACKING_SIZE	SZ_512M
#endif

/*
 * Make sure the area is tracked by RMM at FINE granularity.
 * We do not support changing the tracking yet.
 */
static int rmi_verify_memory_tracking(phys_addr_t start, phys_addr_t end)
{
	while (start < end) {
		unsigned long ret, category, state, next;

		ret = rmi_granule_tracking_get(start, end, &category, &state, &next);
		if (ret != RMI_SUCCESS ||
		    state != RMI_TRACKING_FINE ||
		    category != RMI_MEM_CATEGORY_CONVENTIONAL) {
			/* TODO: Set granule tracking in this case */
			pr_err("Granule tracking for region isn't fine/conventional: %llx",
			       start);
			return -ENODEV;
		}
		start = next;
	}

	return 0;
}

static unsigned long rmi_l0gpt_size(void)
{
	return 1UL << (30 + FIELD_GET(RMI_FEATURE_REGISTER_1_L0GPTSZ,
				      rmm_feat_reg1));
}

static int rmi_create_gpts(phys_addr_t start, phys_addr_t end)
{
	unsigned long l0gpt_sz = rmi_l0gpt_size();

	start = ALIGN_DOWN(start, l0gpt_sz);
	end = ALIGN(end, l0gpt_sz);

	while (start < end) {
		int ret = rmi_gpt_l1_create(start);

		/*
		 * Make sure the L1 GPT tables are created for the region.
		 * RMI_ERROR_GPT indicates the L1 table already exists.
		 */
		if (ret && ret != RMI_ERROR_GPT) {
			/*
			 * FIXME: Handle SRO so that memory can be donated for
			 * the tables.
			 */
			pr_err("GPT Level1 table missing for %llx\n", start);
			return -ENOMEM;
		}
		start += l0gpt_sz;
	}

	return 0;
}

static int rmi_init_metadata(void)
{
	phys_addr_t start, end;
	const struct memblock_region *r;

	for_each_mem_region(r) {
		int ret;

		start = memblock_region_memory_base_pfn(r) << PAGE_SHIFT;
		end = memblock_region_memory_end_pfn(r) << PAGE_SHIFT;
		ret = rmi_verify_memory_tracking(start, end);
		if (ret)
			return ret;
		ret = rmi_create_gpts(start, end);
		if (ret)
			return ret;
	}

	return 0;
}

bool rmi_is_available(void)
{
	return arm64_rmi_is_available;
}

static int __init arm64_init_rmi(void)
{
	/* Continue without realm support if we can't agree on a version */
	if (rmi_check_version())
		return 0;

	if (WARN_ON(rmi_features(0, &rmm_feat_reg0)))
		return 0;
	if (WARN_ON(rmi_features(1, &rmm_feat_reg1)))
		return 0;

	if (rmi_configure())
		return 0;
	if (rmi_init_metadata())
		return 0;

	arm64_rmi_is_available = true;
	pr_info("RMI configured");

	return 0;
}
subsys_initcall(arm64_init_rmi);
