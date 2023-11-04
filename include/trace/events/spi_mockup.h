/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SPI mockup controller transfer writeable tracepoint
 *
 * Copyright(c) 2022 Huawei Technologies Co., Ltd.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM spi_mockup

#if !defined(_TRACE_SPI_MOCKUP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SPI_MOCKUP_H

#include <linux/tracepoint.h>
#include <linux/spi/spi-mockup.h>

#ifndef DECLARE_TRACE_WRITABLE
#define DECLARE_TRACE_WRITABLE(call, proto, args, size) \
	DECLARE_TRACE(call, PARAMS(proto), PARAMS(args))
#endif

DECLARE_TRACE_WRITABLE(spi_transfer_writeable,
	TP_PROTO(struct spi_msg_ctx *msg, u8 chip_select, unsigned int len),
	TP_ARGS(msg, chip_select, len),
	sizeof(struct spi_msg_ctx)
);

#endif /* _TRACE_SPI_MOCKUP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
