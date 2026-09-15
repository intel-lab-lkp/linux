/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Altera Corporation
 *
 * SDOS-only subset of the SoCFPGA FCS (FPGA Crypto Service) interface,
 * shared between the driver front-end (socfpga-fcs.c) and the command
 * engine (socfpga-fcs-core.c).
 *
 * The command engine deals in kernel pointers only: front-ends own every
 * transfer to and from user space. In-kernel consumers can therefore drive
 * the same engine directly.
 */
#ifndef __SOCFPGA_FCS_H
#define __SOCFPGA_FCS_H

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/firmware/intel/stratix10-svc-client.h>

#define SDOS_HEADER_SZ		40
#define SDOS_HMAC_SZ		48
#define SDOS_PLAINDATA_MIN_SZ	32
#define SDOS_PLAINDATA_MAX_SZ	32672
#define SDOS_DECRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ)
#define SDOS_DECRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ)
#define SDOS_ENCRYPTED_MIN_SZ	(SDOS_PLAINDATA_MIN_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)
#define SDOS_ENCRYPTED_MAX_SZ	(SDOS_PLAINDATA_MAX_SZ + SDOS_HEADER_SZ + SDOS_HMAC_SZ)

/**
 * struct fcs_sdos_req - parameters for one SDOS encrypt/decrypt operation
 * @op_mode: non-zero to encrypt, zero to decrypt
 * @src: input buffer, obtained from fcs_alloc_buf()
 * @src_len: number of valid bytes in @src
 * @dst: output buffer, obtained from fcs_alloc_buf()
 * @dst_len: on entry the capacity of @dst, on return the number of bytes the
 *           SDM produced
 * @status: SDM mailbox status, valid only when @status_valid is set
 * @status_valid: set by the engine once the mailbox transaction completed,
 *                whether it succeeded or reported a firmware error. Clear
 *                after a transport failure, where no firmware status exists.
 *
 * Every pointer is a kernel address, so the engine never touches user memory.
 */
struct fcs_sdos_req {
	u32		op_mode;
	const void	*src;
	u32		src_len;
	void		*dst;
	u32		dst_len;
	s32		status;
	bool		status_valid;
};

/**
 * Private driver state for the SoCFPGA FCS that holds the SDM/ATF service
 * channel, the lock serialising command submission, and the latest mailbox
 * status/response.
 */
struct socfpga_fcs_priv {
	/* Communication channel */
	struct stratix10_svc_chan *chan;
	struct stratix10_svc_client client;
	struct miscdevice miscdev;
	/*
	 * Held by the driver and by every open file. An fd may outlive driver
	 * detach, so this state is not devm-managed: the firmware channel and
	 * the allocation are released only when the last reference goes.
	 */
	struct kref refcount;
	/* Set on remove(); further operations fail with -ENODEV. */
	bool removed;
	struct completion completion;
	/*
	 * Serializes FCS command submission: guards the session state and the
	 * single in-flight mailbox transaction (completion/status/resp) so only
	 * one SDM request is outstanding at a time. The engine takes it around
	 * the session and mailbox work of each operation; buffer allocation and
	 * user-space copying happen outside it.
	 */
	struct mutex lock;
	int status;
	u32 resp;
	u32 session_id;
	/* non-zero while a crypto context is active */
	u32 context_id;
	u32 atf_version[3];
	bool atf_version_valid;
};

enum fcs_command_code {
	FCS_DEV_CRYPTO_OPEN_SESSION,
	FCS_DEV_CRYPTO_CLOSE_SESSION,
	FCS_DEV_SDOS_DATA_EXT,
	FCS_DEV_ATF_VERSION,
};

/*
 * Allocate a service-layer buffer usable as fcs_sdos_req.src or .dst.
 * Returns an ERR_PTR on failure; release with fcs_free_buf().
 */
void *fcs_alloc_buf(struct socfpga_fcs_priv *priv, size_t len);

/* Release a buffer from fcs_alloc_buf(); tolerates NULL and error pointers. */
void fcs_free_buf(struct socfpga_fcs_priv *priv, void *buf);
int fcs_sdos_output_size(u32 op_mode, u32 src_len, u32 *out_len);

/*
 * Allocate the per-device FCS state and set up the service channel; reads the
 * ATF version. The state is reference counted; release the driver's reference
 * with fcs_put(). Returns an ERR_PTR on failure.
 */
struct socfpga_fcs_priv *fcs_init(struct device *dev);

/* Take/drop a reference; the last put closes the session and frees the state. */
void fcs_get(struct socfpga_fcs_priv *priv);
void fcs_put(struct socfpga_fcs_priv *priv);

/* Refuse further operations with -ENODEV; call from the remove path. */
void fcs_mark_removed(struct socfpga_fcs_priv *priv);

int fcs_get_atf_version(struct socfpga_fcs_priv *priv, u32 *version);

/* Perform an SDOS (Secure Data Object Service) encrypt/decrypt operation. */
int fcs_sdos_crypt(struct socfpga_fcs_priv *priv, struct fcs_sdos_req *req);

#endif /* SOCFPGA_FCS_H */
