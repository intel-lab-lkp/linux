/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef __UAPI_HSR_PTP_H
#define __UAPI_HSR_PTP_H

#define HSR_INLINE_HDR  0xaf485352
#define HSR_INLINE_HDR_PORT_A	1
#define HSR_INLINE_HDR_PORT_B	2

struct hsr_inline_header {
	uint8_t tx_port;
	uint8_t hsr_hdr;
	uint8_t __pad0[4];
	__be32 magic;
	uint8_t __pad1[2];
	__be16 eth_type;
} __attribute__ ((packed));

#endif
