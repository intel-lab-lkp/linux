// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include "cxl.h"
#include "exports.h"

/* Exporting of cxl_core symbols that are only used by cxl_test */
EXPORT_SYMBOL_NS_GPL(cxl_num_decoders_committed, "CXL");

cxl_add_dport_by_dev_fn _devm_cxl_add_dport_by_dev =
	__devm_cxl_add_dport_by_dev;
EXPORT_SYMBOL_NS_GPL(_devm_cxl_add_dport_by_dev, "CXL");

struct cxl_dport *devm_cxl_add_dport_by_dev(struct cxl_port *port,
					    struct device *dport_dev)
{
	return _devm_cxl_add_dport_by_dev(port, dport_dev);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_dport_by_dev, "CXL");

cxl_add_pt_decoder_fn _devm_cxl_add_passthrough_decoder =
	__devm_cxl_add_passthrough_decoder;
EXPORT_SYMBOL_NS_GPL(_devm_cxl_add_passthrough_decoder, "CXL");

int devm_cxl_add_passthrough_decoder(struct cxl_port *port)
{
	return _devm_cxl_add_passthrough_decoder(port);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_passthrough_decoder, "CXL");

cxl_setup_hdm_fn _devm_cxl_setup_hdm = __devm_cxl_setup_hdm;
EXPORT_SYMBOL_NS_GPL(_devm_cxl_setup_hdm, "CXL");

struct cxl_hdm *devm_cxl_setup_hdm(struct cxl_port *port,
				   struct cxl_endpoint_dvsec_info *info)
{
	return _devm_cxl_setup_hdm(port, info);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_setup_hdm, "CXL");

cxl_enum_decoders_fn _devm_cxl_enumerate_decoders = __devm_cxl_enumerate_decoders;
EXPORT_SYMBOL_NS_GPL(_devm_cxl_enumerate_decoders, "CXL");

int devm_cxl_enumerate_decoders(struct cxl_hdm *cxlhdm,
				struct cxl_endpoint_dvsec_info *info)
{
	return _devm_cxl_enumerate_decoders(cxlhdm, info);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_enumerate_decoders, "CXL");
