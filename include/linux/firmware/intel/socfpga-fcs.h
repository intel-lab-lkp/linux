/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Altera Corporation
 *
 * SDOS-only subset of the SoCFPGA FCS (FPGA Crypto Service) interface,
 * shared between the driver front-end (socfpga-fcs.c) and the command
 * engine (socfpga-fcs-core.c).
 */
#ifndef SOCFPGA_FCS_H
#define SOCFPGA_FCS_H

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define SDOS_HEADER_SZ		40
#define SDOS_HMAC_SZ		48
#define SDOS_PLAINDATA_MIN_SZ	32
#define SDOS_PLAINDATA_MAX_SZ	32672
#define SDOS_DECRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ)
#define SDOS_DECRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ)
#define SDOS_ENCRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)
#define SDOS_ENCRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)

#pragma pack(push, 1)
struct fcs_cmd_context {
	/* Error status variable address */
	int *error_code_addr;
	union {
		struct {
			uuid_t *suuid;
			unsigned int *suuid_len;
		} open_session;

		struct {
			uuid_t suuid;
		} close_session;

		struct {
			uuid_t suuid;
			u32 context_id;
			char *rng;
			u32 rng_len;
		} rng;

		struct {
			uuid_t suuid;
			u32 context_id;
			u32 op_mode;
			char *src;
			u32 src_size;
			char *dst;
			u32 *dst_size;
			u16 id;
			u64 own;
			int pad;
		} sdos;
	};
};

#pragma pack(pop)

/**
 * Private driver state for the SoCFPGA FCS that holds the SDM/ATF service
 * channel, the shared command context and the lock that guards it, and the
 * latest mailbox status/response.
 */
struct socfpga_fcs_priv {
	/* Communication channel */
	struct stratix10_svc_chan *chan;
	struct fcs_cmd_context k_ctx;
	struct stratix10_svc_client client;
	struct completion completion;
	/*
	 * Serializes FCS command submission: guards the shared k_ctx and the
	 * single in-flight mailbox transaction (completion/status/resp) so only
	 * one SDM request is outstanding at a time. This is the lock taken by
	 * fcs_acquire_cmd_ctx() and dropped by fcs_release_cmd_ctx().
	 */
	struct mutex lock;
	int status;
	u32 resp;
	u32 session_id;
	uuid_t uuid_id;
	struct device *dev;
	u32 atf_version[3];
	/*
	 * Backing store for the SDOS output-buffer capacity. The outgoing
	 * request stages k_ctx.sdos.dst_size to point here (device-lifetime)
	 * instead of at a caller stack variable, so the pointer never dangles.
	 */
	u32 sdos_output_size;
};

enum fcs_command_code {
	FCS_DEV_COMMAND_NONE = 0,
	FCS_DEV_CRYPTO_OPEN_SESSION,
	FCS_DEV_CRYPTO_CLOSE_SESSION,
	FCS_DEV_SDOS_DATA_EXT,
	FCS_DEV_ATF_VERSION,
};

/* Take the FCS lock and return the shared command context. */
struct fcs_cmd_context *fcs_acquire_cmd_ctx(void);

/* Release the FCS lock previously taken by fcs_acquire_cmd_ctx(). */
void fcs_release_cmd_ctx(struct fcs_cmd_context *const k_ctx);

/* Allocate the FCS state and set up the service channel; read ATF version. */
int fcs_init(struct device *dev);

/* Close any open session and release the service channel. */
void fcs_deinit(void);

/* Release the service channel and clear the FCS state. */
void fcs_cleanup(void);

/* Request the SDM to open a crypto service session. */
int fcs_session_open(struct fcs_cmd_context *const k_ctx);

/* Request the SDM to close a previously opened session. */
int fcs_session_close(struct fcs_cmd_context *const k_ctx);

/* Return the cached Arm Trusted Firmware build version. */
void fcs_get_atf_version(u32 *version);

/* Perform an SDOS (Secure Data Object Service) encrypt/decrypt operation. */
int fcs_sdos_crypt(struct fcs_cmd_context *const k_ctx);

#endif /* SOCFPGA_FCS_H */
