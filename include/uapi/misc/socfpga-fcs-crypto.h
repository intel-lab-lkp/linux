/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Description:
 * This driver is developed for the SDM SoCFPGA Crypto Service (FCS). It
 * provides an ioctl interface for the SDOS (Secure Data Object Service)
 * encrypt/decrypt operation. The crypto session and the per-request context
 * ID are managed by the kernel internally, so neither is part of the user
 * ABI.
 */
#ifndef __SOCFPGA_FCS_CRYPTO_H
#define __SOCFPGA_FCS_CRYPTO_H

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * struct fcs_ioc_sdos - argument for FCS_IOC_SDOS
 * @error_code: address of a ``__s32`` that receives the SDM status. Written
 *		whenever the request reached the SDM, including when the SDM
 *		reported a failure, and left unchanged otherwise. Initialise
 *		it to 0 before the call.
 * @src: address of the input buffer
 * @dst: address of the output buffer. Must be non-NULL.
 * @dst_size: address of a ``__u32``. On entry it holds the capacity of @dst
 *	      in bytes; on success the kernel overwrites it with the number of
 *	      bytes the SDM produced. Must be non-NULL.
 * @op_mode: non-zero to encrypt, zero to decrypt
 * @src_size: number of bytes at @src. For encrypt the input is a 40-byte
 *	      header followed by 32..32672 bytes of plaintext; for decrypt it
 *	      additionally carries a trailing 48-byte HMAC.
 *
 * The four ``__u64`` members carry user-space addresses as fixed-width
 * values, so the structure has the same layout for 32-bit and 64-bit callers.
 *
 * For an encrypt request the owner ID is read from the caller-supplied header
 * as a little-endian 64-bit value at offset 12 and is passed to the SDM with
 * the request.
 */
struct fcs_ioc_sdos {
	__u64 error_code;	/* __user ptr to __s32 (out)              */
	__u64 src;		/* __user ptr to input buffer (in)        */
	__u64 dst;		/* __user ptr to output buffer (out)      */
	__u64 dst_size;		/* __user ptr to __u32 capacity/len (in/out) */
	__u32 op_mode;		/* (in)  */
	__u32 src_size;		/* (in)  */
};

#define FCS_IOC_MAGIC		0xA6

/**
 * DOC: FCS_IOC_SDOS
 *
 * Encrypt or decrypt one SDOS object. Takes a pointer to a
 * &struct fcs_ioc_sdos.
 *
 * The SDM derives the working key from a device-unique SDOS root key that
 * never leaves the secure boundary, so the host never handles raw key
 * material. Only one SDM transaction is in flight at a time.
 *
 * Return: 0 on success, -1 on failure with ``errno`` set. The SDM status for
 * the request is written to the ``__s32`` addressed by ``error_code``; some
 * non-zero statuses indicate success with a warning. See
 * Documentation/misc-devices/socfpga-fcs.rst for the error codes and the SDM
 * status values.
 */
#define FCS_IOC_SDOS		_IOWR(FCS_IOC_MAGIC, 1, struct fcs_ioc_sdos)

#endif /* __SOCFPGA_FCS_CRYPTO_H */
