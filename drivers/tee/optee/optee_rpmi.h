/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef OPTEE_RPMI_H
#define OPTEE_RPMI_H

#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/wordpart.h>

/*
 * OP-TEE service ABI over RPMI TEE_CALL.
 *
 * Requests and responses are carried in the TEE_CALL service payload.
 * Control fields are little-endian. Response status fields contain unsigned
 * 32-bit TEE result codes: TEE_SUCCESS or TEE_ERROR_*. These are distinct
 * from the outer TEE_CALL status, which uses signed RPMI error codes, and
 * the GP command result in optee_msg_arg.ret.
 *
 * Memory references use a global_id combining the parcel ID and nonce.
 * A zero nonce denotes NULL regardless of the parcel ID. A NULL reference
 * must specify a zero buffer offset; its requested size is preserved.
 */

#define OPTEE_RPMI_SERVICE_UUID \
	UUID_INIT(0x486178e0, 0xe7f8, 0x11e3, \
		  0xbc, 0x5e, 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b)

#define OPTEE_RPMI_VERSION_MAJOR		1
#define OPTEE_RPMI_VERSION_MINOR		0

/**
 * optee_rpmi_global_id() - encode an opaque parcel memory identity
 * @parcel_id: firmware-assigned parcel identifier
 * @nonce: REE nonce supplied when sharing the parcel
 *
 * Return: A 64-bit identity for memory parcel.
 */
static inline u64 optee_rpmi_global_id(u32 parcel_id, u32 nonce)
{
	return ((u64)nonce << 32) | parcel_id;
}

/* Extract the firmware-assigned parcel ID from a memory identity. */
static inline u32 optee_rpmi_parcel_id(u64 global_id)
{
	return lower_32_bits(global_id);
}

/* Extract the nonce; zero denotes a NULL memory reference. */
static inline u32 optee_rpmi_nonce(u64 global_id)
{
	return upper_32_bits(global_id);
}

/**
 * struct optee_rpmi_probe_req - request without operation-specific arguments
 * @op: GET_API_VERSION, GET_OS_VERSION or EXCHANGE_CAPABILITIES
 */
struct optee_rpmi_probe_req {
	__le32 op;
};

/**
 * struct optee_rpmi_status_resp - response carrying only a TEE result
 * @status: TEE result code; TEE_SUCCESS on success
 */
struct optee_rpmi_status_resp {
	__le32 status;
};

/*
 * Return the service API version.
 *
 * Request:  struct optee_rpmi_probe_req
 * Response: struct optee_rpmi_api_resp
 */
#define OPTEE_RPMI_GET_API_VERSION	0

/**
 * struct optee_rpmi_api_resp - GET_API_VERSION response
 * @status: TEE result code; TEE_SUCCESS on success
 * @major: incompatible protocol revision
 * @minor: compatible protocol revision
 */
struct optee_rpmi_api_resp {
	__le32 status;
	__le32 major;
	__le32 minor;
};

/*
 * Return the trusted OS revision, not the service API revision.
 *
 * Request:  struct optee_rpmi_probe_req
 * Response: struct optee_rpmi_os_resp
 */
#define OPTEE_RPMI_GET_OS_VERSION	1

/**
 * struct optee_rpmi_os_resp - GET_OS_VERSION response
 * @status: TEE result code; TEE_SUCCESS on success
 * @major: trusted OS major revision
 * @minor: trusted OS minor revision
 * @reserved: must be zero
 * @build_id: trusted OS build identifier, zero if unspecified
 */
struct optee_rpmi_os_resp {
	__le32 status;
	__le32 major;
	__le32 minor;
	__le32 reserved;
	__le64 build_id;
};

/*
 * Query secure-world capabilities and limits.
 *
 * Request:  struct optee_rpmi_probe_req
 * Response: struct optee_rpmi_caps_resp
 */
#define OPTEE_RPMI_EXCHANGE_CAPABILITIES	2

/**
 * struct optee_rpmi_caps_resp - EXCHANGE_CAPABILITIES response
 * @status: TEE result code; TEE_SUCCESS on success
 * @secure_caps: reserved for future optional features; zero in version 1
 * @rpc_param_count: nonzero parameter capacity of each RPC argument buffer
 * @max_notification_key: inclusive maximum logical key, including synchronous
 *			  keys; zero selects the default maximum of 255
 *
 * Version 1 defines no capability bits. Unknown bits are ignored by Linux
 * for compatibility with future extensions.
 */
struct optee_rpmi_caps_resp {
	__le32 status;
	__le32 secure_caps;
	__le32 rpc_param_count;
	__le32 max_notification_key;
};

/*
 * Unregister a shared parcel from OP-TEE.
 *
 * Request:  struct optee_rpmi_unregister_req
 * Response: struct optee_rpmi_status_resp
 */
#define OPTEE_RPMI_UNREGISTER_SHM	3

/**
 * struct optee_rpmi_unregister_req - retire a shared parcel in OP-TEE
 * @op: OPTEE_RPMI_UNREGISTER_SHM
 * @reserved: must be zero
 * @global_id: identity of the parcel to retire; nonce must be nonzero
 *
 * Success means OP-TEE stopped using the mapping and released its receiver
 * interest.
 */
struct optee_rpmi_unregister_req {
	__le32 op;
	__le32 reserved;
	__le64 global_id;
};

/*
 * Enable asynchronous notification delivery.
 *
 * Request:  struct optee_rpmi_enable_notif_req
 * Response: struct optee_rpmi_status_resp
 */
#define OPTEE_RPMI_ENABLE_ASYNC_NOTIF	4

/**
 * struct optee_rpmi_enable_notif_req - bind an incoming RPMI doorbell
 * @op: OPTEE_RPMI_ENABLE_ASYNC_NOTIF
 * @signal_id: allocated TEE-to-REE signal, not a logical notification key
 *
 * Success activates delivery and raises the doorbell for pending work.
 * Raising the signal requests OPTEE_MSG_CMD_DO_BOTTOM_HALF.
 * OPTEE_MSG_CMD_STOP_ASYNC_NOTIF stops future doorbell generation, but does
 * not drain already-raised signals or release the signal ID.
 */
struct optee_rpmi_enable_notif_req {
	__le32 op;
	__le32 signal_id;
};

/*
 * Start a yielding command using shared command and RPC arguments.
 *
 * Request:  struct optee_rpmi_call_req
 * Response: struct optee_rpmi_call_resp
 */
#define OPTEE_RPMI_YIELDING_CALL_WITH_ARG		5

#define OPTEE_RPMI_YIELDING_CALL_RETURN_DONE		0
#define OPTEE_RPMI_YIELDING_CALL_RETURN_RPC_CMD		1
#define OPTEE_RPMI_YIELDING_CALL_RETURN_INTERRUPT	2

/**
 * struct optee_rpmi_call_req - start a yielding command
 * @op: OPTEE_RPMI_YIELDING_CALL_WITH_ARG
 * @flags: zero in version 1
 * @global_id: argument parcel identity; nonce must be nonzero
 * @arg_offset: command argument byte offset from the parcel's first byte
 * @rpc_offset: RPC argument byte offset from the parcel's first byte
 * @arg_size: command argument extent in bytes
 * @rpc_size: RPC argument capacity in bytes
 *
 * Firmware validates ownership, RW access, alignment and disjoint ranges.
 * TEE_ERROR_BUSY rejects an initial request without acquiring call ownership.
 */
struct optee_rpmi_call_req {
	__le32 op;
	__le32 flags;
	__le64 global_id;
	__le64 arg_offset;
	__le64 rpc_offset;
	__le32 arg_size;
	__le32 rpc_size;
};

/**
 * struct optee_rpmi_call_resp - yielding command response
 * @status: TEE result code, distinct from the command's GP result
 * @result: OPTEE_RPMI_YIELDING_CALL_RETURN_* value when status is success
 * @resume_token: opaque token for a suspended command; ignored for DONE
 *
 * DONE releases all access to the call's argument and RPC ranges. RPC_CMD
 * requests RPC command handling; INTERRUPT requests resumption without an
 * RPC command. Tokens belong to one accepted call, service and caller.
 */
struct optee_rpmi_call_resp {
	__le32 status;
	__le32 result;
	__le32 resume_token;
};

/*
 * Resume a suspended yielding command.
 *
 * Request:  struct optee_rpmi_resume_req
 * Response: struct optee_rpmi_call_resp
 */
#define OPTEE_RPMI_YIELDING_CALL_RESUME	6

/**
 * struct optee_rpmi_resume_req - resume a suspended command
 * @op: OPTEE_RPMI_YIELDING_CALL_RESUME
 * @reserved: must be zero
 * @resume_token: opaque token copied unchanged from the preceding response
 *
 * Resume must not return TEE_ERROR_BUSY.
 */
struct optee_rpmi_resume_req {
	__le32 op;
	__le32 reserved;
	__le32 resume_token;
};

#endif /* OPTEE_RPMI_H */
