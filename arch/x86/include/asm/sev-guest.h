/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc.
 *
 * Author: Brijesh Singh <brijesh.singh@amd.com>
 *
 * SEV-SNP API spec is available at https://developer.amd.com/sev
 */

#ifndef __VIRT_SEVGUEST_H__
#define __VIRT_SEVGUEST_H__

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <asm/sev.h>

#define SNP_REQ_MAX_RETRY_DURATION    (60*HZ)
#define SNP_REQ_RETRY_DELAY           (2*HZ)

#define MAX_AUTHTAG_LEN		32
#define AUTHTAG_LEN		16
#define AAD_LEN			48
#define MSG_HDR_VER		1

/* See SNP spec SNP_GUEST_REQUEST section for the structure */
enum msg_type {
	SNP_MSG_TYPE_INVALID = 0,
	SNP_MSG_CPUID_REQ,
	SNP_MSG_CPUID_RSP,
	SNP_MSG_KEY_REQ,
	SNP_MSG_KEY_RSP,
	SNP_MSG_REPORT_REQ,
	SNP_MSG_REPORT_RSP,
	SNP_MSG_EXPORT_REQ,
	SNP_MSG_EXPORT_RSP,
	SNP_MSG_IMPORT_REQ,
	SNP_MSG_IMPORT_RSP,
	SNP_MSG_ABSORB_REQ,
	SNP_MSG_ABSORB_RSP,
	SNP_MSG_VMRK_REQ,
	SNP_MSG_VMRK_RSP,
	SNP_MSG_TSC_INFO_REQ = 17,
	SNP_MSG_TSC_INFO_RSP,

	SNP_MSG_TYPE_MAX
};

enum aead_algo {
	SNP_AEAD_INVALID,
	SNP_AEAD_AES_256_GCM,
};

struct snp_guest_msg_hdr {
	u8 authtag[MAX_AUTHTAG_LEN];
	u64 msg_seqno;
	u8 rsvd1[8];
	u8 algo;
	u8 hdr_version;
	u16 hdr_sz;
	u8 msg_type;
	u8 msg_version;
	u16 msg_sz;
	u32 rsvd2;
	u8 msg_vmpck;
	u8 rsvd3[35];
} __packed;

/* SNP Guest message request */
struct snp_req_data {
	unsigned long req_gpa;
	unsigned long resp_gpa;
};

struct snp_guest_msg {
	struct snp_guest_msg_hdr hdr;
	u8 payload[4000];
} __packed;

struct sev_guest_platform_data {
	/* request and response are in unencrypted memory */
	struct snp_guest_msg *request;
	struct snp_guest_msg *response;

	struct snp_secrets_page_layout *layout;
	struct snp_req_data input;
};

#define SNP_TSC_INFO_REQ_SZ 128

struct snp_tsc_info_req {
	/* Must be zero filled */
	u8 rsvd[SNP_TSC_INFO_REQ_SZ];
} __packed;

struct snp_tsc_info_resp {
	/* Status of TSC_INFO message */
	u32 status;
	u32 rsvd1;
	u64 tsc_scale;
	u64 tsc_offset;
	u32 tsc_factor;
	u8 rsvd2[100];
} __packed;

struct snp_guest_dev {
	struct device *dev;
	struct miscdevice misc;

	/* Mutex to serialize the shared buffer access and command handling. */
	struct mutex cmd_mutex;

	void *certs_data;
	struct aesgcm_ctx *ctx;

	/*
	 * Avoid information leakage by double-buffering shared messages
	 * in fields that are in regular encrypted memory
	 */
	struct snp_guest_msg secret_request;
	struct snp_guest_msg secret_response;

	struct sev_guest_platform_data *pdata;
	union {
		struct snp_report_req report;
		struct snp_derived_key_req derived_key;
		struct snp_ext_report_req ext_report;
		struct snp_tsc_info_req tsc_info;
	} req;
	unsigned int vmpck_id;
};

struct snp_guest_req {
	void *req_buf;
	size_t req_sz;

	void *resp_buf;
	size_t resp_sz;

	void *data;
	size_t data_npages;

	u64 exit_code;
	unsigned int vmpck_id;
	u8 msg_version;
	u8 msg_type;
};

int snp_setup_psp_messaging(struct snp_guest_dev *snp_dev);
int snp_send_guest_request(struct snp_guest_dev *dev, struct snp_guest_req *req,
			   struct snp_guest_request_ioctl *rio);
bool snp_assign_vmpck(struct snp_guest_dev *dev, unsigned int vmpck_id);
bool snp_is_vmpck_empty(unsigned int vmpck_id);

static inline void free_shared_pages(void *buf, size_t sz)
{
	unsigned int npages = PAGE_ALIGN(sz) >> PAGE_SHIFT;
	int ret;

	if (!buf)
		return;

	ret = set_memory_encrypted((unsigned long)buf, npages);
	if (ret) {
		WARN_ONCE(ret, "failed to restore encryption mask (leak it)\n");
		return;
	}

	__free_pages(virt_to_page(buf), get_order(sz));
}

static inline void *alloc_shared_pages(size_t sz)
{
	unsigned int npages = PAGE_ALIGN(sz) >> PAGE_SHIFT;
	struct page *page;
	int ret;

	page = alloc_pages(GFP_KERNEL_ACCOUNT, get_order(sz));
	if (!page)
		return NULL;

	ret = set_memory_decrypted((unsigned long)page_address(page), npages);
	if (ret) {
		pr_err("%s: failed to mark page shared, ret=%d\n", __func__, ret);
		__free_pages(page, get_order(sz));
		return NULL;
	}

	return page_address(page);
}

#endif /* __VIRT_SEVGUEST_H__ */
