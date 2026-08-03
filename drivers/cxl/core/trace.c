// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include <cxl.h>
#include <cxlmem.h>
#include "core.h"

const char *cxl_trace_memdev_name(struct cxl_port *port)
{
	if (is_cxl_endpoint(port)) {
		struct cxl_memdev *cxlmd = to_cxl_memdev(port->uport_dev);

		return dev_name(&cxlmd->dev);
	}

	return "";
}

const char *cxl_trace_host_name(struct cxl_port *port)
{
	if (is_cxl_endpoint(port)) {
		struct cxl_memdev *cxlmd = to_cxl_memdev(port->uport_dev);

		return dev_name(cxlmd->dev.parent);
	}

	return dev_name(port->uport_dev);
}

const char *cxl_trace_port_name(struct cxl_port *port)
{
	return dev_name(&port->dev);
}

const char *cxl_trace_dport_name(struct cxl_dport *dport)
{
	if (dport)
		return dev_name(dport->dport_dev);
	return "";
}

#define CREATE_TRACE_POINTS
#include "trace.h"
