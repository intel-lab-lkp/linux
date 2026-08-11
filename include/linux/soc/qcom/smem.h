/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_SMEM_H__
#define __QCOM_SMEM_H__

#define QCOM_SMEM_HOST_ANY -1

#if IS_ENABLED(CONFIG_QCOM_SMEM)

bool qcom_smem_is_available(void);
int qcom_smem_alloc(unsigned host, unsigned item, size_t size);
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);

int qcom_smem_get_free_space(unsigned host);

phys_addr_t qcom_smem_virt_to_phys(void *p);

int qcom_smem_get_soc_id(u32 *id);
int qcom_smem_get_feature_code(u32 *code);

int qcom_smem_bust_hwspin_lock_by_host(unsigned int host);

int qcom_smem_dram_get_hbb(void);

#else

static inline bool qcom_smem_is_available(void)
{
	return false;
}

static inline int qcom_smem_alloc(unsigned int host, unsigned int item, size_t size)
{
	return -ENODEV;
}

static inline void *qcom_smem_get(unsigned int host, unsigned int item, size_t *size)
{
	return ERR_PTR(-ENODEV);
}

static inline int qcom_smem_get_free_space(unsigned int host)
{
	return -ENODEV;
}

static inline phys_addr_t qcom_smem_virt_to_phys(void *p)
{
	return 0;
}

static inline int qcom_smem_get_soc_id(u32 *id)
{
	return -ENODEV;
}

static inline int qcom_smem_get_feature_code(u32 *code)
{
	return -ENODEV;
}

static inline int qcom_smem_bust_hwspin_lock_by_host(unsigned int host)
{
	return -ENODEV;
}

static inline int qcom_smem_dram_get_hbb(void)
{
	return -ENODATA;
}

#endif /* CONFIG_QCOM_SMEM */

#endif
