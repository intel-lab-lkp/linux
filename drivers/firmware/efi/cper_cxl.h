/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * UEFI Common Platform Error Record (CPER) support for CXL Section.
 *
 * Copyright (C) 2022 Advanced Micro Devices, Inc.
 *
 * Author: Smita Koralahalli <Smita.KoralahalliChannabasappa@amd.com>
 */

#ifndef LINUX_CPER_CXL_H
#define LINUX_CPER_CXL_H

#include <linux/cxl-event.h>

/* CXL Protocol Error Section */
#define CPER_SEC_CXL_PROT_ERR						\
	GUID_INIT(0x80B9EFB4, 0x52B5, 0x4DE3, 0xA7, 0x77, 0x68, 0x78,	\
		  0x4B, 0x77, 0x10, 0x48)

/* CXL Event record UUIDs are used as the section type */
/*
 * General Media Event Record
 * CXL rev 3.0 Section 8.2.9.2.1.1; Table 8-43
 */
#define CPER_SEC_CXL_GEN_MEDIA						\
	GUID_INIT(0xfbcd0a77, 0xc260, 0x417f,				\
		  0x85, 0xa9, 0x08, 0x8b, 0x16, 0x21, 0xeb, 0xa6)

/*
 * DRAM Event Record
 * CXL rev 3.0 section 8.2.9.2.1.2; Table 8-44
 */
#define CPER_SEC_CXL_DRAM						\
	GUID_INIT(0x601dcbb3, 0x9c06, 0x4eab,				\
		  0xb8, 0xaf, 0x4e, 0x9b, 0xfb, 0x5c, 0x96, 0x24)

/*
 * Memory Module Event Record
 * CXL rev 3.0 section 8.2.9.2.1.3; Table 8-45
 */
#define CPER_SEC_CXL_MEM_MODULE						\
	GUID_INIT(0xfe927475, 0xdd59, 0x4339,				\
		  0xa5, 0x86, 0x79, 0xba, 0xb1, 0x13, 0xb7, 0x74)

#pragma pack(1)

/* Compute Express Link Protocol Error Section, UEFI v2.10 sec N.2.13 */
struct cper_sec_prot_err {
	u64 valid_bits;
	u8 agent_type;
	u8 reserved[7];

	/*
	 * Except for RCH Downstream Port, all the remaining CXL Agent
	 * types are uniquely identified by the PCIe compatible SBDF number.
	 */
	union {
		u64 rcrb_base_addr;
		struct {
			u8 function;
			u8 device;
			u8 bus;
			u16 segment;
			u8 reserved_1[3];
		};
	} agent_addr;

	struct {
		u16 vendor_id;
		u16 device_id;
		u16 subsystem_vendor_id;
		u16 subsystem_id;
		u8 class_code[2];
		u16 slot;
		u8 reserved_1[4];
	} device_id;

	struct {
		u32 lower_dw;
		u32 upper_dw;
	} dev_serial_num;

	u8 capability[60];
	u16 dvsec_len;
	u16 err_len;
	u8 reserved_2[4];
};

#pragma pack()

void cper_print_prot_err(const char *pfx, const struct cper_sec_prot_err *prot_err);
void cper_post_cxl_event(const char *pfx, guid_t *sec_type,
			 struct cper_cxl_event_rec *rec);

#endif //__CPER_CXL_
