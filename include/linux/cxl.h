#ifndef _LINUX_CXL_H
#define _LINUX_CXL_H

#include <linux/xarray.h>
#include <linux/errno.h>

struct cxl_dport;

#if IS_ENABLED(CONFIG_CXL_ACPI)
struct cxl_dport *cxl_find_rch_dport_by_rcrb(resource_size_t rcrb_base);
#else
struct cxl_dport *cxl_find_rch_dport_by_rcrb(resource_size_t rcrb_base)
{
	return NULL;
}
#endif

#endif
