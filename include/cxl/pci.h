/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CXL_CXL_PCI_H__
#define __CXL_CXL_PCI_H__

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/types.h>

/* Register Block Identifier (RBI) */
enum cxl_regloc_type {
	CXL_REGLOC_RBI_EMPTY = 0,
	CXL_REGLOC_RBI_COMPONENT,
	CXL_REGLOC_RBI_VIRT,
	CXL_REGLOC_RBI_MEMDEV,
	CXL_REGLOC_RBI_PMU,
	CXL_REGLOC_RBI_TYPES
};

/* CXL 2.0 8.2.4 CXL Component Register Layout and Definition */
#define CXL_COMPONENT_REG_BLOCK_SIZE SZ_64K

/* CXL 2.0 8.2.5 CXL.cache and CXL.mem Registers */
#define CXL_CM_OFFSET 0x1000
#define CXL_CM_CAP_HDR_OFFSET 0x0
#define   CXL_CM_CAP_HDR_ID_MASK GENMASK(15, 0)
#define     CM_CAP_HDR_CAP_ID 1
#define   CXL_CM_CAP_HDR_VERSION_MASK GENMASK(19, 16)
#define     CM_CAP_HDR_CAP_VERSION 1
#define   CXL_CM_CAP_HDR_CACHE_MEM_VERSION_MASK GENMASK(23, 20)
#define     CM_CAP_HDR_CACHE_MEM_VERSION 1
#define   CXL_CM_CAP_HDR_ARRAY_SIZE_MASK GENMASK(31, 24)
#define CXL_CM_CAP_PTR_MASK GENMASK(31, 20)

#define   CXL_CM_CAP_CAP_ID_RAS 0x2
#define   CXL_CM_CAP_CAP_ID_HDM 0x5
#define   CXL_CM_CAP_CAP_HDM_VERSION 1

/* HDM decoders CXL 2.0 8.2.5.12 CXL HDM Decoder Capability Structure */
#define CXL_HDM_DECODER_CAP_OFFSET 0x0
#define   CXL_HDM_DECODER_COUNT_MASK GENMASK(3, 0)
#define   CXL_HDM_DECODER_TARGET_COUNT_MASK GENMASK(7, 4)
#define   CXL_HDM_DECODER_INTERLEAVE_11_8 BIT(8)
#define   CXL_HDM_DECODER_INTERLEAVE_14_12 BIT(9)
#define   CXL_HDM_DECODER_INTERLEAVE_3_6_12_WAY BIT(11)
#define   CXL_HDM_DECODER_INTERLEAVE_16_WAY BIT(12)
#define CXL_HDM_DECODER_CTRL_OFFSET 0x4
#define   CXL_HDM_DECODER_ENABLE BIT(1)
#define CXL_HDM_DECODER0_BASE_LOW_OFFSET(i) (0x20 * (i) + 0x10)
#define CXL_HDM_DECODER0_BASE_HIGH_OFFSET(i) (0x20 * (i) + 0x14)
#define CXL_HDM_DECODER0_SIZE_LOW_OFFSET(i) (0x20 * (i) + 0x18)
#define CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(i) (0x20 * (i) + 0x1c)
#define CXL_HDM_DECODER0_CTRL_OFFSET(i) (0x20 * (i) + 0x20)
#define   CXL_HDM_DECODER0_CTRL_IG_MASK GENMASK(3, 0)
#define   CXL_HDM_DECODER0_CTRL_IW_MASK GENMASK(7, 4)
#define   CXL_HDM_DECODER0_CTRL_LOCK BIT(8)
#define   CXL_HDM_DECODER0_CTRL_COMMIT BIT(9)
#define   CXL_HDM_DECODER0_CTRL_COMMITTED BIT(10)
#define   CXL_HDM_DECODER0_CTRL_COMMIT_ERROR BIT(11)
#define   CXL_HDM_DECODER0_CTRL_HOSTONLY BIT(12)
#define CXL_HDM_DECODER0_TL_LOW(i) (0x20 * (i) + 0x24)
#define CXL_HDM_DECODER0_TL_HIGH(i) (0x20 * (i) + 0x28)
#define CXL_HDM_DECODER0_SKIP_LOW(i) CXL_HDM_DECODER0_TL_LOW(i)
#define CXL_HDM_DECODER0_SKIP_HIGH(i) CXL_HDM_DECODER0_TL_HIGH(i)

/* HDM decoder control register constants CXL 3.0 8.2.5.19.7 */
#define CXL_DECODER_MIN_GRANULARITY 256
#define CXL_DECODER_MAX_ENCODED_IG 6

static inline int cxl_hdm_decoder_count(u32 cap_hdr)
{
	int val = FIELD_GET(CXL_HDM_DECODER_COUNT_MASK, cap_hdr);

	return val ? val * 2 : 1;
}

struct cxl_reg_map {
	bool valid;
	int id;
	unsigned long offset;
	unsigned long size;
};

struct cxl_component_reg_map {
	struct cxl_reg_map hdm_decoder;
	struct cxl_reg_map ras;
};

struct cxl_device_reg_map {
	struct cxl_reg_map status;
	struct cxl_reg_map mbox;
	struct cxl_reg_map memdev;
};

struct cxl_pmu_reg_map {
	struct cxl_reg_map pmu;
};

/**
 * struct cxl_register_map - DVSEC harvested register block mapping parameters
 * @host: device for devm operations and logging
 * @base: virtual base of the register-block-BAR + @block_offset
 * @resource: physical resource base of the register block
 * @max_size: maximum mapping size to perform register search
 * @reg_type: see enum cxl_regloc_type
 * @component_map: cxl_reg_map for component registers
 * @device_map: cxl_reg_maps for device registers
 * @pmu_map: cxl_reg_maps for CXL Performance Monitoring Units
 */
struct cxl_register_map {
	struct device *host;
	void __iomem *base;
	resource_size_t resource;
	resource_size_t max_size;
	u8 reg_type;
	union {
		struct cxl_component_reg_map component_map;
		struct cxl_device_reg_map device_map;
		struct cxl_pmu_reg_map pmu_map;
	};
};

struct pci_dev;

int cxl_find_regblock(struct pci_dev *pdev, enum cxl_regloc_type type,
		      struct cxl_register_map *map);
int cxl_setup_regs(struct cxl_register_map *map);

#endif /* __CXL_CXL_PCI_H__ */
