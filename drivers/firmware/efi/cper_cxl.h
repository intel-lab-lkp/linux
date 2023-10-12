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

/* CXL Protocol Error Section */
#define CPER_SEC_CXL_PROT_ERR						\
	GUID_INIT(0x80B9EFB4, 0x52B5, 0x4DE3, 0xA7, 0x77, 0x68, 0x78,	\
		  0x4B, 0x77, 0x10, 0x48)

/* CXL General Media Section */
#define CPER_SEC_CXL_GEN_MEDIA						\
	GUID_INIT(0xFBCD0A77, 0xC260, 0x417F, 0x85, 0xA9, 0x08, 0x8B,	\
		  0x16, 0x21, 0xEB, 0xA6)

/* CXL DRAM Section */
#define CPER_SEC_CXL_DRAM						\
	GUID_INIT(0x601DCBB3, 0x9C06, 0x4EAB, 0xB8, 0xAF, 0x4E, 0x9B,	\
		  0xFB, 0x5C, 0x96, 0x24)

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

/* Compute Express Link Component Events Section, UEFI v2.10 sec N.2.14 */
struct cper_sec_comp_event {
	u32 length;
	u64 valid_bits;

	struct {
		u16 vendor_id;
		u16 device_id;
		u8 function;
		u8 device;
		u8 bus;
		u16 segment;
		u16 slot;
		u8 reserved_1;
	} device_id;

	struct {
		u32 lower_dw;
		u32 upper_dw;
	} dev_serial_num;

};

/*
 * Compute Express Link Common Event Record
 * CXL rev 3.0 section 8.2.9.2.1; Table 8-42
 */
struct common_event_record {
	u8 identifier[16];
	u8 length;
	u8 flags[3];
	u16 handle;
	u16 related_handle;
	u64 timestamp;
	u8 maint_op_class;
	u8 reserved[15];
};

/*
 * CXL General Media Event Record - GMER
 * CXL rev 3.0 section 8.2.9.2.1.1; Table 8-43
 */
struct cper_sec_gen_media {
	struct common_event_record record;
	u64 dpa;
	u8 descriptor;
	u8 type;
	u8 transaction_type;
	u16 validity_flags;
	u8 channel;
	u8 rank;
	u8 device[3];
	u8 comp_id[16];
	u8 reserved[46];
};

/*
 * CXL DRAM Event Record
 * CXL rev 3.0 sec 8.2.9.2.1.2; Table 8-44
 */
struct cper_sec_dram {
	struct common_event_record record;
	u64 dpa;
	u8 descriptor;
	u8 type;
	u8 transaction_type;
	u16 validity_flags;
	u8 channel;
	u8 rank;
	u8 nibble_mask[3];
	u8 bank_group;
	u8 bank;
	u8 row[3];
	u16 column;
	u8 cor_mask[32];
	u8 reserved[23];
};

#pragma pack()

void cper_print_prot_err(const char *pfx, const struct cper_sec_prot_err *prot_err);
void cper_print_gen_media(const char *pfx, const struct cper_sec_comp_event *event);
void cper_print_dram(const char *pfx, const struct cper_sec_comp_event *event);

#endif //__CPER_CXL_
