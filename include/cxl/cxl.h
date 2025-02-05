/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2025 Advanced Micro Devices, Inc. */

#ifndef __CXL_H
#define __CXL_H

#include <linux/types.h>

/* Capabilities as defined for:
 *
 *	Component Registers (Table 8-22 CXL 3.1 specification)
 *	Device Registers (8.2.8.2.1 CXL 3.1 specification)
 *
 * and currently being used for kernel CXL support.
 */

enum cxl_dev_cap {
	/* capabilities from Component Registers */
	CXL_DEV_CAP_RAS,
	CXL_DEV_CAP_HDM,
	/* capabilities from Device Registers */
	CXL_DEV_CAP_DEV_STATUS,
	CXL_DEV_CAP_MAILBOX_PRIMARY,
	CXL_DEV_CAP_MEMDEV,
	CXL_MAX_CAPS,
};

/*
 * enum cxl_devtype - delineate type-2 from a generic type-3 device
 * @CXL_DEVTYPE_DEVMEM - Vendor specific CXL Type-2 device implementing HDM-D or
 *			 HDM-DB, no requirement that this device implements a
 *			 mailbox, or other memory-device-standard manageability
 *			 flows.
 * @CXL_DEVTYPE_CLASSMEM - Common class definition of a CXL Type-3 device with
 *			   HDM-H and class-mandatory memory device registers
 */
enum cxl_devtype {
	CXL_DEVTYPE_DEVMEM,
	CXL_DEVTYPE_CLASSMEM,
};

#define CXL_DECODER_F_RAM   BIT(0)
#define CXL_DECODER_F_PMEM  BIT(1)
#define CXL_DECODER_F_TYPE2 BIT(2)

/*
 * struct for an accel driver giving partition data when Type2 device without a
 * mailbox.
 */
struct mds_info {
	u64 total_bytes;
	u64 volatile_only_bytes;
	u64 persistent_only_bytes;
};

enum cxl_partition_mode {
	CXL_PARTMODE_NONE,
	CXL_PARTMODE_RAM,
	CXL_PARTMODE_PMEM,
};

#define CXL_NR_PARTITIONS_MAX 2

struct cxl_dpa_info {
	u64 size;
	struct cxl_dpa_part_info {
		struct range range;
		enum cxl_partition_mode mode;
	} part[CXL_NR_PARTITIONS_MAX];
	int nr_partitions;
};

struct device;
struct cxl_memdev_state *cxl_memdev_state_create(struct device *dev, u64 serial,
					   u16 dvsec, enum cxl_devtype type);
struct pci_dev;
struct cxl_dev_state;
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_memdev_state *cxlmds,
			     unsigned long *caps);
int cxl_await_media_ready(struct cxl_memdev_state *mds);
void cxl_set_media_ready(struct cxl_memdev_state *mds);
void cxl_dev_state_setup(struct cxl_memdev_state *mds, struct mds_info *info);
int cxl_mem_dpa_fetch(struct cxl_memdev_state *mds, struct cxl_dpa_info *info);
int cxl_dpa_setup(struct cxl_memdev_state *cxlmds, const struct cxl_dpa_info *info);
struct cxl_memdev *devm_cxl_add_memdev(struct device *host,
				       struct cxl_memdev_state *cxlmds);
struct cxl_port;
struct cxl_root_decoder *cxl_get_hpa_freespace(struct cxl_memdev *cxlmd,
					       int interleave_ways,
					       unsigned long flags,
					       resource_size_t *max);
void cxl_put_root_decoder(struct cxl_root_decoder *cxlrd);
struct cxl_endpoint_decoder *cxl_request_dpa(struct cxl_memdev *cxlmd,
					     bool is_ram,
					     resource_size_t alloc);
int cxl_dpa_free(struct cxl_endpoint_decoder *cxled);
struct cxl_region *cxl_create_region(struct cxl_root_decoder *cxlrd,
				     struct cxl_endpoint_decoder *cxled, int ways);

int cxl_accel_region_detach(struct cxl_endpoint_decoder *cxled);
#endif
