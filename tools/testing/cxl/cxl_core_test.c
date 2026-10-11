// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */

#include <linux/bitfield.h>
#include <linux/sizes.h>
#include "cxl.h"
#include "core.h"
#include "cxl_core_test.h"
#include "watermark.h"

cxl_test_watermark(cxl_core);

/* Exercise the shared unpacker without HDM MMIO or a PCI device. */
int cxl_test_hdm_ranges(void)
{
	static const struct {
		const char *name;
		u32 ctrl;
		u64 base;
		u64 size;
		int rc;
		unsigned long flags;
		int ways;
		int granularity;
		enum cxl_decoder_type target_type;
	} cases[] = {
		{ "valid", CXL_HDM_DECODER0_CTRL_COMMITTED,
		  SZ_1G, SZ_256M, 0, CXL_DECODER_F_ENABLE,
		  1, 256, CXL_DECODER_DEVMEM },
		{ "overflow", CXL_HDM_DECODER0_CTRL_COMMITTED,
		  U64_MAX - 127, 256, -ENXIO },
		{ "all-ones base", CXL_HDM_DECODER0_CTRL_COMMITTED,
		  U64_MAX, SZ_256M, -ENXIO },
		{ "last address", CXL_HDM_DECODER0_CTRL_COMMITTED,
		  U64_MAX - 255, 256, 0, CXL_DECODER_F_ENABLE,
		  1, 256, CXL_DECODER_DEVMEM },
		{ "zero size", CXL_HDM_DECODER0_CTRL_COMMITTED,
		  SZ_1G, 0, 0, CXL_DECODER_F_ENABLE,
		  1, 256, CXL_DECODER_DEVMEM },
		{ "uncommitted stale size", 0,
		  SZ_1G, SZ_256M, 0, 0, 1, 256, 0 },
		{ "locked host-only interleave",
		  CXL_HDM_DECODER0_CTRL_COMMITTED | CXL_HDM_DECODER0_CTRL_LOCK |
		  CXL_HDM_DECODER0_CTRL_HOSTONLY |
		  FIELD_PREP_CONST(CXL_HDM_DECODER0_CTRL_IW_MASK, 10) |
		  FIELD_PREP_CONST(CXL_HDM_DECODER0_CTRL_IG_MASK, 2),
		  SZ_1G, SZ_256M, 0, CXL_DECODER_F_ENABLE | CXL_DECODER_F_LOCK,
		  12, SZ_1K, CXL_DECODER_HOSTONLYMEM },
	};

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		struct cxl_decoder_config config;
		u64 size;
		int rc;

		rc = cxl_hdm_unpack_decoder(&config, 3, cases[i].ctrl,
					    cases[i].base, cases[i].size);
		if (rc != cases[i].rc) {
			pr_err("cxl_test: HDM %s returned %d, expected %d\n",
			       cases[i].name, rc, cases[i].rc);
			return -EINVAL;
		}
		if (rc)
			continue;
		size = cases[i].size;
		if (!(cases[i].flags & CXL_DECODER_F_ENABLE))
			size = 0;
		if (config.id != 3 || config.hpa_range.start != cases[i].base ||
		    range_len(&config.hpa_range) != size ||
		    config.flags != cases[i].flags ||
		    config.interleave_ways != cases[i].ways ||
		    config.interleave_granularity != cases[i].granularity ||
		    config.target_type != cases[i].target_type) {
			pr_err("cxl_test: HDM %s decoded unexpected state\n",
			       cases[i].name);
			return -EINVAL;
		}
	}

	pr_info("cxl_test: HDM decoder range tests passed\n");
	return 0;
}

EXPORT_SYMBOL_GPL(cxl_test_hdm_ranges);
