/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Advanced Micro Devices, Inc. */

#include <linux/cdev.h>
#include <linux/pci.h>

#ifndef __CXL_ACCEL_MEM_H
#define __CXL_ACCEL_MEM_H

#define CXL_DECODER_F_RAM   BIT(0)
#define CXL_DECODER_F_PMEM  BIT(1)
#define CXL_DECODER_F_TYPE2 BIT(2)

enum accel_resource{
	CXL_ACCEL_RES_DPA,
	CXL_ACCEL_RES_RAM,
	CXL_ACCEL_RES_PMEM,
};

#define CXL_ACCEL_DRIVER_CAP_HDM	0x1
#define CXL_ACCEL_DRIVER_CAP_MBOX	0x2

typedef struct cxl_dev_state cxl_accel_state;
cxl_accel_state *cxl_accel_state_create(struct device *dev, uint8_t caps);

void cxl_accel_set_dvsec(cxl_accel_state *cxlds, u16 dvsec);
void cxl_accel_set_serial(cxl_accel_state *cxlds, u64 serial);
void cxl_accel_set_resource(struct cxl_dev_state *cxlds, struct resource res,
			    enum accel_resource);
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_dev_state *cxlds);
int cxl_accel_request_resource(struct cxl_dev_state *cxlds, bool is_ram);
void cxl_accel_set_media_ready(struct cxl_dev_state *cxlds);
int cxl_await_media_ready(struct cxl_dev_state *cxlds);

struct cxl_memdev *devm_cxl_add_memdev(struct device *host,
				       struct cxl_dev_state *cxlds);

struct cxl_port *cxl_acquire_endpoint(struct cxl_memdev *cxlmd);
void cxl_release_endpoint(struct cxl_memdev *cxlmd, struct cxl_port *endpoint);

struct cxl_root_decoder *cxl_get_hpa_freespace(struct cxl_port *endpoint,
					       int interleave_ways,
					       unsigned long flags,
					       resource_size_t *max);

struct cxl_endpoint_decoder *cxl_request_dpa(struct cxl_port *endpoint,
					     bool is_ram,
					     resource_size_t min,
					     resource_size_t max);
int cxl_dpa_free(struct cxl_endpoint_decoder *cxled);
struct cxl_region *cxl_create_region(struct cxl_root_decoder *cxlrd,
				     struct cxl_endpoint_decoder **cxled,
				     int ways);

int cxl_region_detach(struct cxl_endpoint_decoder *cxled);
#endif
