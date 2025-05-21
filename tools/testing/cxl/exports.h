/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2025 Intel Corporation */
#ifndef __MOCK_CXL_EXPORTS_H_
#define __MOCK_CXL_EXPORTS_H_

typedef struct cxl_hdm *(*cxl_setup_hdm_fn)(struct cxl_port *port,
					    struct cxl_endpoint_dvsec_info *info);
extern cxl_setup_hdm_fn _devm_cxl_setup_hdm;

typedef int (*cxl_enum_decoders_fn)(struct cxl_hdm *cxlhdm,
				    struct cxl_endpoint_dvsec_info *info);
extern cxl_enum_decoders_fn _devm_cxl_enumerate_decoders;

typedef int (*cxl_add_pt_decoder_fn)(struct cxl_port *port);
extern cxl_add_pt_decoder_fn _devm_cxl_add_passthrough_decoder;

#endif
