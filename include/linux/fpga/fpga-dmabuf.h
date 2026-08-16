/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */
#ifndef _LINUX_FPGA_DMABUF_H
#define _LINUX_FPGA_DMABUF_H

struct fpga_manager;

#if IS_REACHABLE(CONFIG_FPGA_DMA_BUF)
int fpga_dmabuf_register(struct fpga_manager *mgr);
void fpga_dmabuf_unregister(struct fpga_manager *mgr);
#else
static inline int fpga_dmabuf_register(struct fpga_manager *mgr)
{
	return 0;
}

static inline void fpga_dmabuf_unregister(struct fpga_manager *mgr) {}
#endif

#endif /* _LINUX_FPGA_DMABUF_H */
