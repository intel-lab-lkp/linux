/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 *
 * MbedTEE RPC wire protocol between REE (Linux) and TEE (MbedTEE OS).
 *
 * This header is the Linux-kernel mirror of
 * mbedtee-os/drivers/rpc/include/rpc/rpc.h. Both copies MUST define
 * identical struct layouts. The static_assert() checks at the bottom
 * of this file (and the _Static_assert() checks in rpc.h) enforce this
 * contract at compile time on every architecture.
 *
 * Type mapping (wire -> Linux kernel alias):
 *   uint8_t   -> u8   -- boolean flag / padding byte
 *   uint16_t  -> u16  -- byte-count field (< 64 KiB payload limit)
 *   uint32_t  -> u32  -- 32-bit ID, counter, flag, or enum
 *   int32_t   -> s32  -- function-defined signed result code
 *   uint64_t  -> u64  -- physical address, kernel pointer token, or size
 *
 * Using u64 for all pointer-sized fields eliminates ABI width mismatches
 * in mixed-width configurations (e.g. RV64 REE + RV32 TEE, or ARM32-LPAE
 * REE + ARM32 TEE). It also prevents phys_addr_t/unsigned-long confusion
 * when CONFIG_PHYS_ADDR_T_64BIT=y on a 32-bit REE kernel.
 */

#ifndef MBEDTEE_MSG_H
#define MBEDTEE_MSG_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/types.h>

/*
 * Fastcall / yield-call classification
 */
#define MBEDTEE_RPC_FASTCALL            BIT(31)
#define MBEDTEE_RPC_IS_FASTCALL(fn)     ((fn) & MBEDTEE_RPC_FASTCALL)

/*
 * REE -> TEE fast calls
 */
#define MBEDTEE_RPC_OS_VERSION          (0x0100 | MBEDTEE_RPC_FASTCALL)
#define MBEDTEE_RPC_SUPPORT_YIELD       (0x0101 | MBEDTEE_RPC_FASTCALL)
#define MBEDTEE_RPC_COMPLETE_TEE        (0x0102 | MBEDTEE_RPC_FASTCALL)

/*
 * REE -> TEE yield calls
 */
#define MBEDTEE_RPC_OPEN_SESSION        1
#define MBEDTEE_RPC_INVOKE_SESSION      2
#define MBEDTEE_RPC_CLOSE_SESSION       3
#define MBEDTEE_RPC_REGISTER_SHM        5
#define MBEDTEE_RPC_UNREGISTER_SHM      6
#define MBEDTEE_RPC_CANCEL              7

/*
 * TEE -> REE RPC calls
 */
#define MBEDTEE_RPC_COMPLETE_REE        0
#define MBEDTEE_RPC_REETIME             1
#define MBEDTEE_RPC_REEFS               2
#define MBEDTEE_RPC_RPMB                3
#define MBEDTEE_RPC_MAX                 4

/*
 * Supplicant types (TEE -> REE -> tee-supplicant)
 */
#define MBEDTEE_SUPP_REEFS              1
#define MBEDTEE_SUPP_RPMB               2

/* MbedTEE RPC protocol uses 4 KiB page units regardless of host PAGE_SIZE. */
#define MBEDTEE_PAGE_SIZE           4096UL

/*
 * REE <-> TEE RPC call command (wire format, 32 bytes fixed header).
 *
 * Fixed layout:
 *   +0   id          u32 - RPC function identifier
 *   +4   size        u16 - inline payload byte count (0..65535)
 *   +6   interrupted u8  - set by REE when caller is interrupted
 *   +7   reserved    u8  - must be zero (explicit alignment pad)
 *   +8   ret         s32 - return value written by callee
 *   +12  pad         u32 - must be zero (explicit alignment pad)
 *   +16  waiter_id   u64 - sync RPC request ID echoed on completion
 *   +24  shm         u64 - physical address of sync-RPC shared memory
 *   +32  data[]      u64 - inline payload (waiter_id==0) or empty
 *
 * Yield-call contract:
 *   - Session/control RPCs return GlobalPlatform result codes here.
 *   - Host-local errno values must be translated before being put on the wire.
 *   - Fast calls may use function-specific return values.
 */
struct rpc_cmd {
	u32 id;
	u16 size;
	u8  interrupted;
	u8  reserved;
	s32 ret;
	u32 pad;
	u64 waiter_id;
	u64 shm;
	u64 data[];
};

/*
 * REE -> TEE pages for GP shared memory.
 *
 * All fields are u64 so the layout is identical when REE is 64-bit
 * but TEE is 32-bit (physical addresses, sizes and counts use 64 bits).
 */
struct rpc_memref {
	u64 id;
	u64 pages;
	u64 offset;
	u64 size;
	u64 cnt;
};

/*
 * REE -> TEE parameter entry: value or shared memory reference.
 */
union rpc_tee_param {
	struct rpc_memref memref;

	struct {
		u32 a;
		u32 b;
	} value;
};

/*
 * REE -> TEE parameters for RPC session operations
 * (MBEDTEE_RPC_OPEN_SESSION / MBEDTEE_RPC_INVOKE_SESSION /
 *  MBEDTEE_RPC_CLOSE_SESSION).
 */
struct rpc_param {
	s32 session_id;
	u32 cmd_id;
	u32 ret_origin;
	u32 params_type;
	union rpc_tee_param params[4];
	u8  uuid[16];
	u8  clnt_uuid[16];
};

/*
 * REE -> TEE cancellation request (MBEDTEE_RPC_CANCEL).
 */
struct rpc_cancel_req {
	u32 session_id;
	u32 cancel_id;
};

/*
 * TEE <-> REE supplicant payloads for REEFS and RPMB RPCs.
 * Only the fixed headers are interpreted in the Linux driver.
 */
/*
 * Common supplicant command header. Both reefs_cmd and rpmb_cmd embed this
 * as their first member so the kernel can write a status code through a
 * single cast to (struct supp_cmd_hdr *) without caring about the full
 * payload type. The wire layout is identical on every architecture.
 */
struct supp_cmd_hdr {
	s32 ret;
	s32 op;
};

struct reefs_cmd {
	struct supp_cmd_hdr hdr;
	s32 flags;
	s32 fd;
	u64 len;
	u8 data[];
};

struct rpmb_cmd {
	struct supp_cmd_hdr hdr;
	u32 nframes;
	u8 data[];
};

/*
 * TEE <-> REE RPC ring buffer header (24 bytes fixed).
 */
struct rpc_ringbuf {
	u32 wr;              /* producer write pointer */
	u32 rd;              /* consumer read pointer */
	u32 callee_ready;    /* callee ready flag */
	u32 callee_imsic_id; /* RISC-V only: IMSIC local interrupt id */
	u32 callee_hartid;   /* RISC-V only: target hart-id for T2R notification */
	u32 reserved;        /* padding, must be zero */
	u8  mem[];
};

/*
 * Compile-time ABI layout assertions.
 * These must match the _Static_assert() checks in mbedtee-os rpc/rpc.h.
 */
static_assert(sizeof(struct rpc_cmd) == 32,
	      "rpc_cmd wire size mismatch");
static_assert(offsetof(struct rpc_cmd, ret) == 8,
	      "rpc_cmd.ret offset mismatch");
static_assert(offsetof(struct rpc_cmd, waiter_id) == 16,
	      "rpc_cmd.waiter_id offset mismatch");
static_assert(offsetof(struct rpc_cmd, shm) == 24,
	      "rpc_cmd.shm offset mismatch");
static_assert(offsetof(struct rpc_cmd, data) == 32,
	      "rpc_cmd.data offset mismatch");
static_assert(sizeof(struct rpc_memref) == 40,
	      "rpc_memref wire size mismatch");
static_assert(sizeof(struct rpc_ringbuf) == 24,
	      "rpc_ringbuf header size mismatch");
static_assert(sizeof(struct rpc_param) == 208,
	      "rpc_param wire size mismatch");
static_assert(sizeof(struct rpc_cancel_req) == 8,
	      "rpc_cancel_req wire size mismatch");

#endif /* MBEDTEE_MSG_H */
