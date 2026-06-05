/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_KHO_ABI_PSTORE_H
#define _LINUX_KHO_ABI_PSTORE_H

#include <linux/types.h>

#define KHO_PSTORE_FDT_NAME	"pstore-kho"
#define KHO_PSTORE_VERSION	1

struct pstore_kho_record {
	s64			size;
	s64			time_sec;
	u32			time_nsec;
	s32			count;
	u32			reason;
	u32			part;
	u32			compressed;
	char			buf[];
};

struct pstore_ser {
	u32			version;
	struct pstore_kho_record record;
};

#endif /* _LINUX_KHO_ABI_PSTORE_H */
