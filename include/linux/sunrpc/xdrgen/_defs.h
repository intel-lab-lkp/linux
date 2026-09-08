/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Oracle and/or its affiliates.
 *
 * This header defines XDR data type primitives specified in
 * Section 4 of RFC 4506, used by RPC programs implemented
 * in the Linux kernel.
 *
 * A generated definition can also embed a struct xdr_buf, which
 * carries a page-resident payload captured by reference, so this
 * header pulls in the kernel's XDR types as well.
 */

#ifndef _SUNRPC_XDRGEN__DEFS_H_
#define _SUNRPC_XDRGEN__DEFS_H_

#include <linux/sunrpc/xdr.h>

#define TRUE	(true)
#define FALSE	(false)

typedef struct {
	u32 len;
	unsigned char *data;
} string;

typedef struct {
	u32 len;
	u8 *data;
} opaque;

/*
 * Cursor a hook-driven aggregate codec hands to its application hooks,
 * one element at a time, in place of a materialized C array. The
 * generated framing owns it. @xdr is the RPC layer's stream; @ctx is
 * that stream's xdrgen_ctx, the svc_rqst on the server. @index is
 * the current element and @count the wire array length: a decoder
 * fills @count from the wire before the begin hook runs, an
 * encoder's begin hook sets it. @member_id selects among a type's
 * marked members. The begin/item/end contract is under "Pragma
 * aggregate" in tools/net/sunrpc/xdrgen/README.
 */
struct xdrgen_aggregate_cursor {
	struct xdr_stream	*xdr;
	u32			index;
	u32			count;
	unsigned int		member_id;
	void			*ctx;
};

#define XDR_void		(0)
#define XDR_bool		(1)
#define XDR_short		(1)
#define XDR_unsigned_short	(1)
#define XDR_int			(1)
#define XDR_unsigned_int	(1)
#define XDR_long		(1)
#define XDR_unsigned_long	(1)
#define XDR_hyper		(2)
#define XDR_unsigned_hyper	(2)

#endif /* _SUNRPC_XDRGEN__DEFS_H_ */
