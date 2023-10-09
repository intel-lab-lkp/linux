/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 */

#ifndef __UAPI_LINUX_NSM_H
#define __UAPI_LINUX_NSM_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define NSM_MAGIC		0x0A

#define NSM_REQUEST_MAX_SIZE	0x1000
#define NSM_RESPONSE_MAX_SIZE	0x3000
#define NSM_PCR_MAX_SIZE        0x200
#define NSM_MAX_PCRS            0x100

struct nsm_iovec {
	__u64 addr; /* Virtual address of target buffer */
	__u64 len;  /* Length of target buffer */
};

/* Raw NSM message. Only available with CAP_SYS_ADMIN. */
struct nsm_raw {
	/* Request from user */
	struct nsm_iovec request;
	/* Response to user */
	struct nsm_iovec response;
};
#define NSM_IOCTL_RAW		_IOWR(NSM_MAGIC, 0x0, struct nsm_raw)

/* Maximum length input data */
struct nsm_data_req {
	__u32 len;
	__u8  data[NSM_REQUEST_MAX_SIZE];
};

/* Maximum length output data */
struct nsm_data_resp {
	__u32 len;
	__u8  data[NSM_RESPONSE_MAX_SIZE];
};

/* PCR hash. Currently at most 512 bits, but let's leave room for up to 4096. */
struct nsm_pcr_data {
	__u32 len;
	__u8  data[NSM_PCR_MAX_SIZE];
};

/*
 * DescribePCR
 *
 * Queries the PCR contents of a single PCR. Returns whether the PCR is
 * currently in locked state and the PCR hash value.
 */
struct nsm_describe_pcr_req {
	__u16 index;
};

struct nsm_describe_pcr_resp {
	__u16 lock;
	struct nsm_pcr_data data;
};

union nsm_describe_pcr {
	struct nsm_describe_pcr_req req;
	struct nsm_describe_pcr_resp resp;
};
#define NSM_IOCTL_DESCRIBE_PCR	_IOWR(NSM_MAGIC, 0x1, union nsm_describe_pcr)

/*
 * ExtendPCR
 *
 * Extends the PCR hash with additional binary data. Returns the new PCR
 * hash value after extension.
 */
struct nsm_extend_pcr_req {
	__u16 index;
	__u16 pad;
	struct nsm_data_req data;
};

struct nsm_extend_pcr_resp {
	struct nsm_pcr_data data;
};

union nsm_extend_pcr {
	struct nsm_extend_pcr_req req;
	struct nsm_extend_pcr_resp resp;
};
#define NSM_IOCTL_EXTEND_PCR	_IOWR(NSM_MAGIC, 0x2, union nsm_extend_pcr)

/*
 * LockPCR
 *
 * Enables lock state for a single PCR. After this operation, the PCR becomes
 * unmodifyable until Enclave destruction.
 */
struct nsm_lock_pcr_req {
	__u16 index;
};

union nsm_lock_pcr {
	struct nsm_lock_pcr_req req;
};
#define NSM_IOCTL_LOCK_PCR	_IOWR(NSM_MAGIC, 0x3, union nsm_lock_pcr)

/*
 * LockPCRs
 *
 * Enables lock state for all PCR from 0 up to the given range_from_zero
 * parameter. After this operation, all PCR in range become unmodifyable
 * until Enclave destruction.
 */
struct nsm_lock_pcrs_req {
	__u16 range_from_zero;
};

union nsm_lock_pcrs {
	struct nsm_lock_pcrs_req req;
};
#define NSM_IOCTL_LOCK_PCRS	_IOWR(NSM_MAGIC, 0x4, union nsm_lock_pcrs)

/*
 * DescribeNSM
 *
 * Provides metadata information about the NSM backend implementation,
 * such as version and maximum number of PCRs.
 */
struct nsm_u16_resp {
	__u32 u16s;
	__u16 u16[NSM_MAX_PCRS];
};

struct nsm_describe_nsm_resp {
	__u16 major;
	__u16 minor;
	__u16 patch;
	char module_id[256];			/* null-terminated C string */
	__u16 max_pcrs;
	struct nsm_u16_resp locked_pcrs;
	char digest[16];			/* null-terminated C string */
};

union nsm_describe_nsm {
	struct nsm_describe_nsm_resp resp;
};
#define NSM_IOCTL_DESCRIBE_NSM	_IOWR(NSM_MAGIC, 0x5, union nsm_describe_nsm)

/*
 * Attestation
 *
 * Provides an attestation document that you can use to attest the Enclave
 * against external services. Takes 3 binary input parameters that get
 * reflected 1:1 inside the attestation document.
 */
struct nsm_attestation_req {
	struct nsm_data_req user_data;
	struct nsm_data_req nonce;
	struct nsm_data_req public_key;
};

struct nsm_attestation_resp {
	struct nsm_data_resp document;
};

union nsm_attestation {
	struct nsm_attestation_req req;
	struct nsm_attestation_resp resp;
};
#define NSM_IOCTL_ATTESTATION	_IOWR(NSM_MAGIC, 0x6, union nsm_attestation)

/*
 * GetRandom
 *
 * Returns random bytes.
 */
struct nsm_get_random_resp {
	struct nsm_data_resp random;
};

union nsm_get_random {
	struct nsm_get_random_resp resp;
};
#define NSM_IOCTL_RANDOM	_IOWR(NSM_MAGIC, 0x7, union nsm_get_random)

#endif /* __UAPI_LINUX_NSM_H */
