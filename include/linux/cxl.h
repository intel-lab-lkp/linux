#ifndef _LINUX_CXL_H
#define _LINUX_CXL_H

#include <linux/xarray.h>
#include <linux/errno.h>

struct cxl_dport;

struct cxl_dport *cxl_find_rch_dport_by_rcrb(resource_size_t rcrb_base);

#endif
