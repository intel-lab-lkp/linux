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
