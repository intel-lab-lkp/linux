/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2025 Intel Corporation */
#ifndef __MOCK_CXL_EXPORTS_H_
#define __MOCK_CXL_EXPORTS_H_

typedef struct cxl_dport *(*cxl_add_dport_by_dev_fn)(struct cxl_port *port,
							  struct device *dport_dev);
extern cxl_add_dport_by_dev_fn _devm_cxl_add_dport_by_dev;

typedef int(*cxl_switch_decoders_setup_fn)(struct cxl_port *port);
extern cxl_switch_decoders_setup_fn _devm_cxl_switch_port_decoders_setup;

typedef bool(*platform_cxlrd_matches_cxled_fn)(const struct cxl_root_decoder *cxlrd,
					       const struct cxl_endpoint_decoder *cxled);
extern platform_cxlrd_matches_cxled_fn _platform_cxlrd_matches_cxled;

typedef bool(*platform_region_matches_cxld_fn)(const struct cxl_region_params *p,
					       const struct cxl_decoder *cxld);
extern platform_region_matches_cxld_fn _platform_region_matches_cxld;
#endif
