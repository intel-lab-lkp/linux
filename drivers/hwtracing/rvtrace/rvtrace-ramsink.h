/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 SiFive, Inc.
 */

#ifndef __RVTRACE_RAMSINK_H__
#define __RVTRACE_RAMSINK_H__

int rvtrace_ramsink_setup(struct rvtrace_component *comp);
size_t rvtrace_ramsink_copyto_auxbuf(struct rvtrace_component *comp,
				     struct rvtrace_perf_auxbuf *buf);

#endif /* __RVTRACE_RAMSINK_H__ */
