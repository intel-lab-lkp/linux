/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_SMEM_H__
#define __QCOM_SMEM_H__

#define QCOM_SMEM_HOST_ANY -1

bool qcom_smem_is_available(void);
int qcom_smem_alloc(unsigned host, unsigned item, size_t size);
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);

int qcom_smem_get_free_space(unsigned host);

phys_addr_t qcom_smem_virt_to_phys(void *p);

int qcom_smem_get_soc_id(u32 *id);
int qcom_smem_get_feature_code(u32 *code);

int qcom_smem_bust_hwspin_lock_by_host(unsigned int host);

#ifdef CONFIG_QCOM_SMEM_PSTORE
int qcom_register_pstore_smem(struct device *dev);
void qcom_unregister_pstore_smem(void);

#define MINIDUMP_MAX_NAME_LENGTH	12

/**
 * struct qcom_minidump_region - Minidump region information
 *
 * @name:	Minidump region name
 * @virt_addr:  Virtual address of the entry.
 * @phys_addr:	Physical address of the entry to dump.
 * @size:	Number of bytes to dump from @address location,
 *		and it should be 4 byte aligned.
 */
struct qcom_minidump_region {
	char		name[MINIDUMP_MAX_NAME_LENGTH];
	void		*virt_addr;
	phys_addr_t	phys_addr;
	size_t		size;
};

int qcom_minidump_region_unregister(const struct qcom_minidump_region *region);
int qcom_minidump_region_register(const struct qcom_minidump_region *region);

int qcom_smem_md_init(struct device *dev);

#else

static inline int qcom_register_pstore_smem(struct device *dev)
{
	return 0;
}

static inline void qcom_unregister_pstore_smem(void)
{
}

static inline int qcom_smem_md_init(struct device *dev)
{
	return 0;
}
#endif
#endif
