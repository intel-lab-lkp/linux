/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UAPI_LINUX_ULTRAETH_H
#define _UAPI_LINUX_ULTRAETH_H

#include <asm/byteorder.h>
#include <linux/types.h>

#define UET_DEFAULT_PORT 5432
#define UET_SVC_MAX_LEN 64
#define UET_DEFAULT_ACK_GEN_TRIGGER (1 << 14)
#define UET_DEFAULT_ACK_GEN_MIN_PKT_ADD (1 << 10)

/* types used for prologue's type field */
enum {
	UET_PDS_TYPE_RSVD0,
	UET_PDS_TYPE_ENC_HDR,
	UET_PDS_TYPE_RUD_REQ,
	UET_PDS_TYPE_ROD_REQ,
	UET_PDS_TYPE_RUDI_REQ,
	UET_PDS_TYPE_RUDI_RESPONSE,
	UET_PDS_TYPE_UUD_REQ,
	UET_PDS_TYPE_ACK,
	UET_PDS_TYPE_ACK_CC,
	UET_PDS_TYPE_ACK_CCX,
	UET_PDS_TYPE_NACK,
	UET_PDS_TYPE_CTRL_MSG
};

/* ctl_type when type is UET_PDS_CTRL_MSG (control message) */
enum {
	UET_CTL_TYPE_NOOP,
	UET_CTL_TYPE_REQ_ACK,
	UET_CTL_TYPE_CLEAR,
	UET_CTL_TYPE_REQ_CLEAR,
	UET_CTL_TYPE_CLOSE,
	UET_CTL_TYPE_REQ_CLOSE,
	UET_CTL_TYPE_PROBE,
	UET_CTL_TYPE_CREDIT,
	UET_CTL_TYPE_REQ_CREDIT
};

/* next header, 0x06-0x0E reserved */
enum {
	UET_PDS_NEXT_HDR_NONE		= 0x00,
	UET_PDS_NEXT_HDR_REQ_SMALL	= 0x01,
	UET_PDS_NEXT_HDR_REQ_MEDIUM	= 0x02,
	UET_PDS_NEXT_HDR_REQ_STD	= 0x03,
	UET_PDS_NEXT_HDR_RSP		= 0x04,
	UET_PDS_NEXT_HDR_RSP_DATA	= 0x05,
	UET_PDS_NEXT_HDR_RSP_DATA_SMALL	= 0x06,
	UET_PDS_NEXT_HDR_PDS		= 0x0F,
};

/* fields(union): type_next_flags, type_ctl_flags  */
#define UET_PROLOGUE_FLAGS_BITS 7
#define UET_PROLOGUE_FLAGS_MASK 0x7f
#define UET_PROLOGUE_NEXT_BITS 4
#define UET_PROLOGUE_NEXT_MASK 0x0f
#define UET_PROLOGUE_NEXT_SHIFT UET_PROLOGUE_FLAGS_BITS
#define UET_PROLOGUE_CTL_BITS UET_PROLOGUE_NEXT_BITS
#define UET_PROLOGUE_CTL_SHIFT UET_PROLOGUE_NEXT_SHIFT
#define UET_PROLOGUE_CTL_MASK UET_PROLOGUE_NEXT_MASK
#define UET_PROLOGUE_TYPE_BITS 5
#define UET_PROLOGUE_TYPE_MASK 0x1f
#define UET_PROLOGUE_TYPE_SHIFT (UET_PROLOGUE_NEXT_SHIFT + UET_PROLOGUE_NEXT_BITS)
struct uet_prologue_hdr {
	union {
		__be16 type_next_flags;
		__be16 type_ctl_flags;
	};
} __attribute__ ((__packed__));

static inline __u8 uet_prologue_flags(const struct uet_prologue_hdr *hdr)
{
	return __be16_to_cpu(hdr->type_next_flags) & UET_PROLOGUE_FLAGS_MASK;
}

static inline __u8 uet_prologue_next_hdr(const struct uet_prologue_hdr *hdr)
{
	return (__be16_to_cpu(hdr->type_next_flags) >> UET_PROLOGUE_NEXT_SHIFT) &
	       UET_PROLOGUE_NEXT_MASK;
}

static inline __u8 uet_prologue_ctl_type(const struct uet_prologue_hdr *hdr)
{
	return (__be16_to_cpu(hdr->type_ctl_flags) >> UET_PROLOGUE_CTL_SHIFT) &
	       UET_PROLOGUE_CTL_MASK;
}

static inline __u8 uet_prologue_type(const struct uet_prologue_hdr *hdr)
{
	return (__be16_to_cpu(hdr->type_next_flags) >> UET_PROLOGUE_TYPE_SHIFT) &
	       UET_PROLOGUE_TYPE_MASK;
}

/* rud/rod request flags */
enum {
	UET_PDS_REQ_FLAG_RSV2	= (1 << 0),
	UET_PDS_REQ_FLAG_CC	= (1 << 1),
	UET_PDS_REQ_FLAG_SYN	= (1 << 2),
	UET_PDS_REQ_FLAG_AR	= (1 << 3),
	UET_PDS_REQ_FLAG_RETX	= (1 << 4),
	UET_PDS_REQ_FLAG_RSV	= (1 << 5),
	UET_PDS_REQ_FLAG_CRC	= (1 << 6),
};

/* field: pdc_mode_psn_offset */
#define UET_PDS_REQ_PSN_OFF_BITS 12
#define UET_PDS_REQ_PSN_OFF_MASK 0xff1
#define UET_PDS_REQ_MODE_BITS 4
#define UET_PDS_REQ_MODE_MASK 0xf
#define UET_PDS_REQ_MODE_SHIFT UET_PDS_REQ_PSN_OFF_BITS
struct uet_pds_req_hdr {
	struct uet_prologue_hdr prologue;
	__be16 clear_psn_offset;
	__be32 psn;
	__be16 spdcid;
	union {
		__be16 pdc_mode_psn_offset;
		__be16 dpdcid;
	};
} __attribute__ ((__packed__));

static inline __u16 uet_pds_request_psn_offset(const struct uet_pds_req_hdr *req)
{
	return __be16_to_cpu(req->pdc_mode_psn_offset) & UET_PDS_REQ_PSN_OFF_MASK;
}

static inline __u8 uet_pds_request_pdc_mode(const struct uet_pds_req_hdr *req)
{
	return (__be16_to_cpu(req->pdc_mode_psn_offset) >> UET_PDS_REQ_MODE_SHIFT) &
	       UET_PDS_REQ_MODE_MASK;
}

/* rud/rod ack flags */
enum {
	UET_PDS_ACK_FLAG_RSVD	= (1 << 0),
	UET_PDS_ACK_FLAG_REQ1	= (1 << 1),
	UET_PDS_ACK_FLAG_REQ2	= (1 << 2),
	UET_PDS_ACK_FLAG_P	= (1 << 3),
	UET_PDS_ACK_FLAG_RETX	= (1 << 4),
	UET_PDS_ACK_FLAG_M	= (1 << 5),
	UET_PDS_ACK_FLAG_CRC	= (1 << 6)
};

struct uet_pds_ack_hdr {
	struct uet_prologue_hdr prologue;
	__be16 ack_psn_offset;
	__be32 cack_psn;
	__be16 spdcid;
	__be16 dpdcid;
} __attribute__ ((__packed__));

/* ext ack CC flags */
enum {
	UET_PDS_ACK_EXT_CC_F_RSVD	= (1 << 0)
};

/* field: cc_type_mpr_sack_off */
#define UET_PDS_ACK_EXT_MPR_BITS 8
#define UET_PDS_ACK_EXT_MPR_MASK 0xff
#define UET_PDS_ACK_EXT_CC_FLAGS_BITS 4
#define UET_PDS_ACK_EXT_CC_FLAGS_MASK 0xf
#define UET_PDS_ACK_EXT_CC_FLAGS_SHIFT UET_PDS_ACK_EXT_MPR_BITS
#define UET_PDS_ACK_EXT_CC_TYPE_BITS 4
#define UET_PDS_ACK_EXT_CC_TYPE_MASK 0xf
#define UET_PDS_ACK_EXT_CC_TYPE_SHIFT (UET_PDS_ACK_EXT_CC_FLAGS_SHIFT + \
				       UET_PDS_ACK_EXT_CC_FLAGS_BITS)
/* header used for ACK_CC */
struct uet_pds_ack_ext_hdr {
	__be16 cc_type_flags_mpr;
	__be16 sack_psn_offset;
	__be64 sack_bitmap;
	__be64 ack_cc_state;
} __attribute__ ((__packed__));

static inline __u8 uet_pds_ack_ext_mpr(const struct uet_pds_ack_ext_hdr *ack)
{
	return __be16_to_cpu(ack->cc_type_flags_mpr) & UET_PDS_ACK_EXT_MPR_MASK;
}

static inline __u8 uet_pds_ack_ext_cc_flags(const struct uet_pds_ack_ext_hdr *ack)
{
	return (__be16_to_cpu(ack->cc_type_flags_mpr) >> UET_PDS_ACK_EXT_CC_FLAGS_SHIFT) &
	       UET_PDS_ACK_EXT_CC_FLAGS_MASK;
}

static inline __u8 uet_pds_ack_ext_cc_type(const struct uet_pds_ack_ext_hdr *ack)
{
	return (__be16_to_cpu(ack->cc_type_flags_mpr) >> UET_PDS_ACK_EXT_CC_TYPE_SHIFT) &
	       UET_PDS_ACK_EXT_CC_TYPE_MASK;
}

/* NACK codes */
enum {
	UET_PDS_NACK_TRIMMED		= 0x01,
	UET_PDS_NACK_TRIMMED_LASTHOP	= 0x02,
	UET_PDS_NACK_TRIMMED_ACK	= 0x03,
	UET_PDS_NACK_NO_PDC_AVAIL	= 0x04,
	UET_PDS_NACK_NO_CCC_AVAIL	= 0x05,
	UET_PDS_NACK_NO_BITMAP		= 0x06,
	UET_PDS_NACK_NO_PKT_BUFFER	= 0x07,
	UET_PDS_NACK_NO_GTD_DEL_AVAIL	= 0x08,
	UET_PDS_NACK_NO_SES_MSG_AVAIL	= 0x09,
	UET_PDS_NACK_NO_RESOURCE	= 0x0A,
	UET_PDS_NACK_PSN_OOR_WINDOW	= 0x0B,
	UET_PDS_NACK_FIRST_ROD_OOO	= 0x0C,
	UET_PDS_NACK_ROD_OOO		= 0x0D,
	UET_PDS_NACK_INV_DPDCID		= 0x0E,
	UET_PDS_NACK_PDC_HDR_MISMATCH	= 0x0F,
	UET_PDS_NACK_CLOSING		= 0x10,
	UET_PDS_NACK_CLOSING_IN_ERR	= 0x11,
	UET_PDS_NACK_PKT_NOT_RCVD	= 0x12,
	UET_PDS_NACK_GTD_RESP_UNAVAIL	= 0x13,
	UET_PDS_NACK_ACK_WITH_DATA	= 0x14,
	UET_PDS_NACK_INVALID_SYN	= 0x15,
	UET_PDS_NACK_PDC_MODE_MISMATCH	= 0x16,
	UET_PDS_NACK_NEW_START_PSN	= 0x17,
	UET_PDS_NACK_RCVD_SES_PROCG	= 0x18,
	UET_PDS_NACK_UNEXP_EVENT	= 0x19,
	UET_PDS_NACK_RCVR_INFER_LOSS	= 0x1A,
	/* 0x1B - 0xFC reserved for UET */
	UET_PDS_NACK_EXP_NACK_NORMAL	= 0xFD,
	UET_PDS_NACK_T_EXP_NACK_ERR	= 0xFE,
	UET_PDS_NACK_EXP_NACK_FATAL	= 0xFF
};

/* NACK flags */
enum {
	UET_PDS_NACK_FLAG_RSV21	= (1 << 0),
	UET_PDS_NACK_FLAG_RSV22	= (1 << 1),
	UET_PDS_NACK_FLAG_RSV23	= (1 << 2),
	UET_PDS_NACK_FLAG_NT	= (1 << 3),
	UET_PDS_NACK_FLAG_RETX	= (1 << 4),
	UET_PDS_NACK_FLAG_M	= (1 << 5),
	UET_PDS_NACK_FLAG_RSV	= (1 << 6)
};

struct uet_pds_nack_hdr {
	struct uet_prologue_hdr prologue;
	__u8 nack_code;
	__u8 vendor_code;
	__be32 nack_psn_pkt_id;
	__be16 spdcid;
	__be16 dpdcid;
	__be32 payload;
} __attribute__ ((__packed__));

/* ses request op codes */
enum {
	UET_SES_REQ_OP_NOOP			= 0x00,
	UET_SES_REQ_OP_WRITE			= 0x01,
	UET_SES_REQ_OP_READ			= 0x02,
	UET_SES_REQ_OP_ATOMIC			= 0x03,
	UET_SES_REQ_OP_FETCHING_ATOMIC		= 0x04,
	UET_SES_REQ_OP_SEND			= 0x05,
	UET_SES_REQ_OP_RENDEZVOUS_SEND		= 0x06,
	UET_SES_REQ_OP_DGRAM_SEND		= 0x07,
	UET_SES_REQ_OP_DEFERRABLE_SEND		= 0x08,
	UET_SES_REQ_OP_TAGGED_SEND		= 0x09,
	UET_SES_REQ_OP_RENDEZVOUS_TSEND		= 0x0A,
	UET_SES_REQ_OP_DEFERRABLE_TSEND		= 0x0B,
	UET_SES_REQ_OP_DEFERRABLE_RTR		= 0x0C,
	UET_SES_REQ_OP_TSEND_ATOMIC		= 0x0D,
	UET_SES_REQ_OP_TSEND_FETCH_ATOMIC	= 0x0E,
	UET_SES_REQ_OP_MSG_ERROR		= 0x0F,
	UET_SES_REQ_OP_INC_PUSH			= 0x10,
};

enum {
	UET_SES_REQ_FLAG_SOM		= (1 << 0),
	UET_SES_REQ_FLAG_EOM		= (1 << 1),
	UET_SES_REQ_FLAG_HD		= (1 << 2),
	UET_SES_REQ_FLAG_RELATIVE	= (1 << 3),
	UET_SES_REQ_FLAG_IE		= (1 << 4),
	UET_SES_REQ_FLAG_DC		= (1 << 5)
};

/* field: resv_opcode */
#define UET_SES_REQ_OPCODE_MASK 0x3f
/* field: flags */
#define UET_SES_REQ_FLAGS_MASK 0x3f
#define UET_SES_REQ_FLAGS_VERSION_MASK 0x3
#define UET_SES_REQ_FLAGS_VERSION_SHIFT 6
/* field: resv_idx */
#define UET_SES_REQ_INDEX_MASK 0xfff
/* field: idx_gen_job_id */
#define UET_SES_REQ_JOB_ID_BITS 24
#define UET_SES_REQ_JOB_ID_MASK 0xffffff
#define UET_SES_REQ_INDEX_GEN_MASK 0xff
#define UET_SES_REQ_INDEX_GEN_SHIFT UET_SES_REQ_JOB_ID_BITS
/* field: resv_pid_on_fep */
#define UET_SES_REQ_PID_ON_FEP_MASK 0xfff
struct uet_ses_req_hdr {
	__u8 resv_opcode;
	__u8 flags;
	__be16 msg_id;
	__be32 idx_gen_job_id;
	__be16 resv_pid_on_fep;
	__be16 resv_idx;
	__be64 buffer_offset;
	__be32 initiator;
	__be64 match_bits;
	__be64 header_data;
	__be32 request_len;
} __attribute__ ((__packed__));

static inline __u8 uet_ses_req_opcode(const struct uet_ses_req_hdr *sreq)
{
	return sreq->resv_opcode & UET_SES_REQ_OPCODE_MASK;
}

static inline __u8 uet_ses_req_flags(const struct uet_ses_req_hdr *sreq)
{
	return sreq->flags & UET_SES_REQ_FLAGS_MASK;
}

static inline __u8 uet_ses_req_version(const struct uet_ses_req_hdr *sreq)
{
	return (sreq->flags >> UET_SES_REQ_FLAGS_VERSION_SHIFT) &
	       UET_SES_REQ_FLAGS_VERSION_MASK;
}

static inline __u16 uet_ses_req_index(const struct uet_ses_req_hdr *sreq)
{
	return __be16_to_cpu(sreq->resv_idx) & UET_SES_REQ_INDEX_MASK;
}

static inline __u32 uet_ses_req_job_id(const struct uet_ses_req_hdr *sreq)
{
	return __be32_to_cpu(sreq->idx_gen_job_id) & UET_SES_REQ_JOB_ID_MASK;
}

static inline __u8 uet_ses_req_index_gen(const struct uet_ses_req_hdr *sreq)
{
	return (__be32_to_cpu(sreq->idx_gen_job_id) >> UET_SES_REQ_INDEX_GEN_SHIFT) &
	       UET_SES_REQ_INDEX_GEN_MASK;
}

static inline __u16 uet_ses_req_pid_on_fep(const struct uet_ses_req_hdr *sreq)
{
	return __be16_to_cpu(sreq->resv_pid_on_fep) & UET_SES_REQ_PID_ON_FEP_MASK;
}

/* return codes */
enum {
	UET_SES_RSP_RC_NULL		= 0x00,
	UET_SES_RSP_RC_OK		= 0x01,
	UET_SES_RSP_RC_BAD_GEN		= 0x02,
	UET_SES_RSP_RC_DISABLED		= 0x03,
	UET_SES_RSP_RC_DISABLED_GEN	= 0x04,
	UET_SES_RSP_RC_NO_MATCH		= 0x05,
	UET_SES_RSP_RC_UNSUPP_OP	= 0x06,
	UET_SES_RSP_RC_UNSUPP_SIZE	= 0x07,
	UET_SES_RSP_RC_AT_INVALID	= 0x08,
	UET_SES_RSP_RC_AT_PERM		= 0x09,
	UET_SES_RSP_RC_AT_ATS_ERROR	= 0x0A,
	UET_SES_RSP_RC_AT_NO_TRANS	= 0x0B,
	UET_SES_RSP_RC_AT_OUT_OF_RANGE	= 0x0C,
	UET_SES_RSP_RC_HOST_POISONED	= 0x0D,
	UET_SES_RSP_RC_HOST_UNSUCC_CMPL	= 0x0E,
	UET_SES_RSP_RC_AMO_UNSUPP_OP	= 0x0F,
	UET_SES_RSP_RC_AMO_UNSUPP_DT	= 0x10,
	UET_SES_RSP_RC_AMO_UNSUPP_SIZE	= 0x11,
	UET_SES_RSP_RC_AMO_UNALIGNED	= 0x12,
	UET_SES_RSP_RC_AMO_FP_NAN	= 0x13,
	UET_SES_RSP_RC_AMO_FP_UNDERFLOW	= 0x14,
	UET_SES_RSP_RC_AMO_FP_OVERFLOW	= 0x15,
	UET_SES_RSP_RC_AMO_FP_INEXACT	= 0x16,
	UET_SES_RSP_RC_PERM_VIOLATION	= 0x17,
	UET_SES_RSP_RC_OP_VIOLATION	= 0x18,
	UET_SES_RSP_RC_BAD_INDEX	= 0x19,
	UET_SES_RSP_RC_BAD_PID		= 0x1A,
	UET_SES_RSP_RC_BAD_JOB_ID	= 0x1B,
	UET_SES_RSP_RC_BAD_MKEY		= 0x1C,
	UET_SES_RSP_RC_BAD_ADDR		= 0x1D,
	UET_SES_RSP_RC_CANCELLED	= 0x1E,
	UET_SES_RSP_RC_UNDELIVERABLE	= 0x1F,
	UET_SES_RSP_RC_UNCOR		= 0x20,
	UET_SES_RSP_RC_UNCOR_TRNSNT	= 0x21,
	UET_SES_RSP_RC_TOO_LONG		= 0x22,
	UET_SES_RSP_RC_INITIATOR_ERR	= 0x23,
	UET_SES_RSP_RC_DROPPED		= 0x24,
};

/* ses response list values */
enum {
	UET_SES_RSP_LIST_EXPECTED	= 0x00,
	UET_SES_RSP_LIST_OVERFLOW	= 0x01
};

/* ses response op codes */
enum {
	UET_SES_RSP_OP_DEF_RESP		= 0x00,
	UET_SES_RSP_OP_RESPONSE		= 0x01,
	UET_SES_RSP_OP_RESP_W_DATA	= 0x02
};

/* field: lst_opcode_ver_rc */
#define UET_SES_RSP_RC_BITS 6
#define UET_SES_RSP_RC_MASK 0x3f
#define UET_SES_RSP_RC_SHIFT 0
#define UET_SES_RSP_VERSION_BITS 2
#define UET_SES_RSP_VERSION_MASK 0x3
#define UET_SES_RSP_VERSION_SHIFT (UET_SES_RSP_RC_SHIFT + \
				   UET_SES_RSP_RC_BITS)
#define UET_SES_RSP_OPCODE_BITS 6
#define UET_SES_RSP_OPCODE_MASK 0x3f
#define UET_SES_RSP_OPCODE_SHIFT (UET_SES_RSP_VERSION_SHIFT + \
				  UET_SES_RSP_VERSION_BITS)
#define UET_SES_RSP_LIST_BITS 2
#define UET_SES_RSP_LIST_MASK 0x3
#define UET_SES_RSP_LIST_SHIFT (UET_SES_RSP_OPCODE_SHIFT + \
				UET_SES_RSP_OPCODE_BITS)
/* field: idx_gen_job_id */
#define UET_SES_RSP_JOB_ID_BITS 24
#define UET_SES_RSP_JOB_ID_MASK 0xffffff
#define UET_SES_RSP_INDEX_GEN_MASK 0xff
#define UET_SES_RSP_INDEX_GEN_SHIFT UET_SES_RSP_JOB_ID_BITS
struct uet_ses_rsp_hdr {
	__be16 lst_opcode_ver_rc;
	__be16 msg_id;
	__be32 idx_gen_job_id;
	__be32 mod_len;
} __attribute__ ((__packed__));

static inline __u8 uet_ses_rsp_rc(const struct uet_ses_rsp_hdr *rsp)
{
	return (__be32_to_cpu(rsp->lst_opcode_ver_rc) >>
		UET_SES_RSP_RC_SHIFT) & UET_SES_RSP_RC_MASK;
}

static inline __u8 uet_ses_rsp_list(const struct uet_ses_rsp_hdr *rsp)
{
	return (__be32_to_cpu(rsp->lst_opcode_ver_rc) >>
		UET_SES_RSP_LIST_SHIFT) & UET_SES_RSP_LIST_MASK;
}

static inline __u8 uet_ses_rsp_version(const struct uet_ses_rsp_hdr *rsp)
{
	return (__be32_to_cpu(rsp->lst_opcode_ver_rc) >>
		UET_SES_RSP_VERSION_SHIFT) & UET_SES_RSP_VERSION_MASK;
}

static inline __u8 uet_ses_rsp_opcode(const struct uet_ses_rsp_hdr *rsp)
{
	return (__be32_to_cpu(rsp->lst_opcode_ver_rc) >>
		UET_SES_RSP_OPCODE_SHIFT) & UET_SES_RSP_OPCODE_MASK;
}

static inline __u32 uet_ses_rsp_job_id(const struct uet_ses_rsp_hdr *rsp)
{
	return __be32_to_cpu(rsp->idx_gen_job_id) & UET_SES_RSP_JOB_ID_MASK;
}

static inline __u8 uet_ses_rsp_index_gen(const struct uet_ses_req_hdr *rsp)
{
	return (__be32_to_cpu(rsp->idx_gen_job_id) >> UET_SES_RSP_INDEX_GEN_SHIFT) &
	       UET_SES_RSP_INDEX_GEN_MASK;
}

enum {
	UET_ADDR_F_VALID_FEP_CAP	= (1 << 0),
	UET_ADDR_F_VALID_ADDR		= (1 << 1),
	UET_ADDR_F_VALID_PID_ON_FEP	= (1 << 2),
	UET_ADDR_F_VALID_RI		= (1 << 3),
	UET_ADDR_F_VALID_INIT_ID	= (1 << 4),
	UET_ADDR_F_ADDRESS_MODE		= (1 << 5),
	UET_ADDR_F_ADDRESS_TYPE		= (1 << 6),
	UET_ADDR_F_MTU_LIMITED		= (1 << 7),
};

#define UET_ADDR_FLAG_IP_VER (1 << 6)

struct fep_in_address {
	union {
		__be32 ip;
		__u8 ip6[16];
	};
	__u16 family;
};

struct fep_address {
	struct fep_in_address in_address;

	__u16 flags;
	__u16 fep_caps;
	__u16 start_resource_index;
	__u16 num_resource_indices;
	__u32 initiator_id;
	__u16 pid_on_fep;
	__u16 padding;
	__u8 version;
};
#endif /* _UAPI_LINUX_ULTRAETH_H */
