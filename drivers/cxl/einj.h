/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CXL protocol Error INJection support.
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Ben Cheatham <benjamin.cheatham@amd.com>
 */
#ifndef CXL_EINJ_H
#define CXL_EINJ_H
#include <linux/pci.h>

int einj_cxl_available_error_type_show(struct seq_file *m, void *v);
int einj_cxl_inject_error(struct pci_dev *dport_dev, u64 type);
int einj_cxl_inject_rch_error(u64 rcrb, u64 type);

#if IS_ENABLED(CONFIG_CXL_EINJ)
static inline int cxl_einj_available_error_type_show(struct seq_file *m,
						     void *v)
{
	return einj_cxl_available_error_type_show(m, v);
}

static inline int cxl_einj_inject_error(struct pci_dev *dport_dev, u64 type)
{
	return einj_cxl_inject_error(dport_dev, type);
}

static inline int cxl_einj_inject_rch_error(u64 rcrb, u64 type)
{
	return einj_cxl_inject_rch_error(rcrb, type);
}

#else
static inline int cxl_einj_available_error_type_show(struct seq_file *m,
						     void *v)
{
	return -ENXIO;
}

static inline int cxl_einj_type_show(struct seq_file *m, void *data)
{
	return -ENXIO;
}

static inline int cxl_einj_inject_error(struct pci_dev *dport_dev, u64 type)
{
	return -ENXIO;
}

static inline int cxl_einj_inject_rch_error(u64 rcrb, u64 type)
{
	return -ENXIO;
}
#endif

#endif
