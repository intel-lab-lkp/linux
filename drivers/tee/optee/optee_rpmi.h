/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/*
 * Copyright 2026 NXP
 */
#ifndef OPTEE_RPMI_H
#define OPTEE_RPMI_H

#include <linux/types.h>
#include <linux/uuid.h>

/*
 * This file defines how the OP-TEE SMC ABI (optee_smc.h) is carried by the
 * TEE service group of the RISC-V Platform Management Interface (RPMI). It
 * is kept in sync between secure world and the normal world driver.
 *
 * An invocation of the SMC ABI is a TEE_CALL service request sent to the
 * RPMI TEE framework on an SBI MPXY channel where:
 * - SENDER_ID identifies the REE endpoint and TARGET_ID the OP-TEE
 *   endpoint, both assigned by the framework,
 * - SERVICE is OPTEE_RPMI_SERVICE_UUID in RFC 4122 byte order,
 * - SERVICE_DATA carries the register arguments a0-a7 of the SMC ABI as
 *   little-endian 64-bit words.
 *
 * The SERVICE_RSP of the TEE_CALL response carries the return values a0-a3
 * of the SMC ABI as little-endian 64-bit words. The STATUS of the response
 * is set by the framework and is RPMI_SUCCESS whenever OP-TEE was reached,
 * errors reported by OP-TEE itself are returned in a0 as usual.
 */

/*
 * UUID identifying the OP-TEE API as a TEE_CALL service, the same value
 * as returned by OPTEE_SMC_CALLS_UID (OPTEE_MSG_UID_0..3).
 */
#define OPTEE_RPMI_SERVICE_UUID \
	UUID_INIT(0x384fb3e0, 0xe7f8, 0x11e3, \
		  0xaf, 0x63, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)

#define OPTEE_RPMI_CALL_NUM_ARGS	8
#define OPTEE_RPMI_CALL_NUM_RETS	4

/**
 * struct optee_rpmi_call_req - TEE_CALL request data invoking the SMC ABI
 * @sender_id:	SENDER_ID, endpoint identifier of the REE
 * @target_id:	TARGET_ID, endpoint identifier of OP-TEE
 * @service:	SERVICE, bytes of OPTEE_RPMI_SERVICE_UUID
 * @data_len:	SERVICE_DATA_LEN, sizeof(@args)
 * @args:	SERVICE_DATA, register arguments a0-a7 of the SMC ABI
 */
struct optee_rpmi_call_req {
	__le32 sender_id;
	__le32 target_id;
	u8 service[UUID_SIZE];
	__le32 data_len;
	__le64 args[OPTEE_RPMI_CALL_NUM_ARGS];
} __packed;

/**
 * struct optee_rpmi_call_rsp - TEE_CALL response data of the SMC ABI
 * @status:	STATUS, RPMI error code set by the framework
 * @rsp_len:	SERVICE_RSP_LEN, sizeof(@rets)
 * @rets:	SERVICE_RSP, return values a0-a3 of the SMC ABI
 */
struct optee_rpmi_call_rsp {
	__le32 status;
	__le32 rsp_len;
	__le64 rets[OPTEE_RPMI_CALL_NUM_RETS];
} __packed;

#endif /*OPTEE_RPMI_H*/
