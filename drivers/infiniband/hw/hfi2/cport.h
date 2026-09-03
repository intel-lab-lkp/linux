/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _CPORT_H
#define _CPORT_H

/*****************************************************
 * "Public" software interfaces (inside driver only).
 */

extern uint hfi2_cport_adm_to;

/*
 * Op-codes for requests (and associated responses).
 * CPORT firmware must have the same definitions.
 */
#define CH_OP_PING 0 /* simple ping/echo command */
#define CH_OP_WHO 1
#define CH_OP_HOW 2
#define CH_OP_START 3 /* driver start/options */
#define CH_OP_STOP 4 /* driver stop (unload) */
#define CH_OP_TRAP 5 /* notification of TRAP condition */
#define CH_OP_TRAP_REPRESS 6 /* TRAP acknowledge */
#define CH_OP_MAD_9B 7 /* Local MAD packets 9B */
#define CH_OP_MAD_16B 8 /* Local MAD packets 16B */
#define CH_OP_UMAD_9B 9 /* User MAD packets 9B */
#define CH_OP_UMAD_16B 10 /* User MAD packets 16B */

/*
 * Error codes for responses.
 * CPORT firmware must have the same definitions.
 */
#define MSG_RSP_STATUS_OK 0
#define MSG_RSP_STATUS_SEQ_NO_ERROR 1
#define MSG_RSP_STATUS_OPCODE_UNSUPPORTED 2
#define MSG_RSP_STATUS_INVALID_STATE 3
#define MSG_RSP_STATUS_RETRY 4
#define MSG_RSP_STATUS_DENIED 5

/*
 * The structures below describe payloads exchanged with CPORT firmware
 * over a fixed-size message protocol. Use plain integer fields (rather
 * than C bitfields) so the layout is portable across compilers and
 * architectures, and access sub-fields through the SHIFT/MASK macros
 * below.
 */

/* cport_options.flags bits */
#define CPORT_OPT_BARE_METAL BIT(0)
#define CPORT_OPT_GSI BIT(1)
#define CPORT_OPT_FLR BIT(2)
#define CPORT_OPT_SPI_WE BIT(3)
#define CPORT_OPT_LOCAL_MAD BIT(4)
/* bits 5-15 reserved */

struct cport_options {
	u16 flags;
};

/* cport_trap_status.flags bits */
#define CPORT_TRAP_PSC BIT(0) /* Port State Change */
#define CPORT_TRAP_LI BIT(1) /* Link Integrity */
#define CPORT_TRAP_BO BIT(2) /* Buffer Overrun */
#define CPORT_TRAP_FW BIT(3) /* Flow Watchdog */
#define CPORT_TRAP_CC BIT(4) /* Capability Change */
#define CPORT_TRAP_SIC BIT(5) /* System Image Change */
#define CPORT_TRAP_BMK BIT(6) /* Bad M Key */
#define CPORT_TRAP_BQK BIT(7) /* Bad Q Key */
#define CPORT_TRAP_LWC BIT(8) /* Link Width Change */
#define CPORT_TRAP_QSFP BIT(9) /* QSFP Fault */
#define CPORT_TRAP_OVTM BIT(10) /* Over-temp */
/* bits 11-31 reserved */

struct cport_trap_status {
	u32 flags;
};

/* CPORT firmware version sub-field accessors (operate on the u64 value) */
#define CPORT_FW_VER_BLD(v) ((u32)((v) & 0xffffffffULL))
#define CPORT_FW_VER_PAT(v) ((u32)(((v) >> 32) & 0xfULL))
#define CPORT_FW_VER_QLT(v) ((u32)(((v) >> 36) & 0xfULL))
#define CPORT_FW_VER_MNT(v) ((u32)(((v) >> 40) & 0xffULL))
#define CPORT_FW_VER_MIN(v) ((u32)(((v) >> 48) & 0xffULL))
#define CPORT_FW_VER_MAJ(v) ((u32)(((v) >> 56) & 0xffULL))

/* Fields in 4-qword payload of WHO response */
struct cport_who_payload {
	/* qword 1: low byte reserved, next byte interop, then options */
	u16 introp; /* low 8 bits reserved, high 8 bits interop level */
	struct cport_options fixed;
	struct cport_options suppt;
	u16 max_msg; /* max cport msg length */
	/* qword 2 */
	u64 vers; /* fw version; see CPORT_FW_VER_* accessors */
	/* qword 3 */
	u64 node_guid;
	/* qword 4 */
	struct cport_trap_status trap_sup;
	u16 _resv2;
	u16 max_aux; /* max cport aux length */
};

/*
 * HOW response payload sub-field accessors. Logical and physical state
 * fields are packed into qword 1; temperature and validity flags occupy
 * qword 4.
 */
#define CPORT_HOW_LOG_ST(qw1, port) (((qw1) >> ((port) * 4)) & 0x7)
#define CPORT_HOW_PHY_ST(phy, port) \
	(((phy)[(port) / 2] >> (((port) % 2) * 8)) & 0xff)
#define CPORT_HOW_INTEROP_LEVEL(qw3) ((u8)((qw3) & 0xff))
#define CPORT_HOW_STARTED(qw4) ((u8)((qw4) & 0xff))
#define CPORT_HOW_TEMP_VALID(qw4) (((qw4) >> 8) & 0x1)
#define CPORT_HOW_QSFP1_TEMP_VALID(qw4) (((qw4) >> 9) & 0x1)
#define CPORT_HOW_QSFP2_TEMP_VALID(qw4) (((qw4) >> 10) & 0x1)
#define CPORT_HOW_TEMP(qw4) ((u16)(((qw4) >> 16) & 0xffff))
#define CPORT_HOW_QSFP1_TEMP(qw4) ((u16)(((qw4) >> 32) & 0xffff))
#define CPORT_HOW_QSFP2_TEMP(qw4) ((u16)(((qw4) >> 48) & 0xffff))

/* Fields in 4-qword payload of HOW response */
struct cport_how_payload {
	/* qword 1 */
	u16 log_st; /* per-port logical state, 4 bits each */
	struct cport_options opts_ena;
	u16 phy_st[2]; /* phy state, two ports per u16 */
	/* qword 2 */
	struct cport_trap_status trap_ena;
	struct cport_trap_status trap_sts;
	/* qword 3: low byte holds interoperability level; rest reserved */
	u64 interop;
	/* qword 4: temperature/qsfp data; see CPORT_HOW_* accessors */
	u64 temps;
};

/* START payload sub-field accessors */
#define CPORT_START_SIDX(s) ((u16)((s) & 0x7))
#define CPORT_START_INTEROP(s) ((u8)(((s) >> 8) & 0xff))

/* Fields in 1-qword payload of START request/response */
struct cport_start_payload {
	u16 sidx; /* low 3 bits sidx, high byte interop */
	struct cport_options opts_ena;
	struct cport_trap_status trap_ena;
};

/* Fields in 1-qword payload of STOP request/response */
struct cport_stop_payload {
	u64 sidx; /* low 3 bits sidx, rest reserved */
};

/* Fields in 1-qword payload of TRAP or TRAP_REPRESS request */
struct cport_trap_payload {
	struct cport_trap_status trap_sts;
	u32 _resv1;
};

/*
 * Non-blocking request interface.
 *
 * hfi2_cport_send_req_nb() returns 'handle' (or IS_ERR(handle)).
 * Caller supplies 'wait' which will be "upped" when the matching response
 * is received.  Caller also supplies a timeout for the OUTBOX_EMPTY wait.
 * This is generally needed if the caller will be using a timeout when waiting
 * for completion, to ensure that the send kworker also times out and exits.
 * Timeouts, etc, use hfi2_cport_send_cancel() to terminate without receiving response.
 *
 * Payload is always copied to internal buffer, so caller
 * may dispose of their buffer immediately on return.
 *
 * Timeout value of MAX_SCHEDULE_TIMEOUT causes infinite wait for OUTBOX_EMPTY.
 *
 * One of hfi2_cport_send_comp() or hfi2_cport_send_cancel() must be called in order
 * to fully release 'handle'.
 *
 * Response payload and length is provided in 'rsp_pld' and 'rsp_len',
 * which has been kalloc'ed and must be kfree'ed by caller.
 *
 * hfi2_cport_send_comp() returns the status (error code) from the response.
 */
void *hfi2_cport_send_req_nb(struct hfi2_devdata *dd, u8 op, u8 sideband,
			void *payload, int len, struct semaphore *wait,
			long timeout);
int hfi2_cport_send_comp(struct hfi2_devdata *dd, void *handle, void **rsp_pld,
		    int *rsp_len);
void hfi2_cport_send_cancel(struct hfi2_devdata *dd, void *handle);

/*
 * Blocking request interface with timeout.
 *
 * The caller may dispose of 'payload' immediately on return.
 * Response payload and length is provided in 'rsp_pld' and 'rsp_len',
 * which has been kalloc'ed and must be kfree'ed by caller.
 *
 * Timeout value of MAX_SCHEDULE_TIMEOUT causes infinite wait for response
 * and OUTBOX_EMPTY.
 *
 * Returns the status (error code) from the response.
 */
int hfi2_cport_send_req(struct hfi2_devdata *dd, u8 op, u8 sideband, void *payload,
		   int len, void **rsp_pld, int *rsp_len, long timeout);

/*
 * CPORT Notification interface.
 *
 * A notification is defined as a request that has no response.
 * This is implicitly non-blocking. timeout is applied to the
 * send wait for OUTBOX_EMPTY.
 *
 * The caller may dispose of 'payload' immediately on return.
 *
 * Returns 0 if the request was successfully queued.
 */
int hfi2_cport_send_notif(struct hfi2_devdata *dd, u8 op, u8 sideband, void *payload,
		     int len, long timeout);

/**************************************
 * API for notifications from CPORT
 */

/*
 * Handler prototype for callbacks.
 *
 * Returns response status (error) value. Must setup response payload
 * if appropriate. Whether or not a response is actually sent depends on
 * whether the request asked for one. Default is no payload (0 length).
 *
 * The semantics for responses are defined by the op-code. Error responses
 * may have different payloads than successful responses (or no payload at all).
 * Payloads may even be optional.
 */
typedef int (*cport_handler)(struct hfi2_devdata *dd, u8 op, u8 sideband,
			     void *payload, int len, void *handle);

/*
 * Register a callback for a range of op-codes. Only one callback may be
 * registered for a given op-code. Returns 0 on success (valid op-code range).
 */
int hfi2_cport_register_cb(struct hfi2_devdata *dd, u8 op_start, u8 op_end,
		      cport_handler func);

/*
 * Set static buffer for response payload of 'len' bytes from callback.
 *
 * Buffer may be disposed of immediately on return. If 'len' exceeds
 * maximum allowed, returns -EINVAL.
 */
int hfi2_cport_resp_set(void *handle, void *payload, int len);

/*
 * Interrupt handler for MctxtCportToPcieInt
 */
void hfi2_is_cport_int(struct hfi2_devdata *dd, unsigned int source);

/*
 * Start a CPORT ping for count iterations.
 */
int hfi2_cport_ping_start(struct hfi2_devdata *dd, unsigned int count);

/*
 * Initialize the CPORT communication facility.
 *
 * If the device does not have a CPORT, this returns 0.
 */
int hfi2_cport_init(struct hfi2_devdata *dd);

/*
 * Shutdown the CPORT communication facility.
 *
 * If the device has no CPORT, this does nothing.
 */
int hfi2_cport_exit(struct hfi2_devdata *dd);

#endif /* _CPORT_H */
