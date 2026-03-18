/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2015 Intel Corporation.
 * Copyright 2025 Cornelis Networks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2015 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef _LINUX__HFI1_IOCTL_H
#define _LINUX__HFI1_IOCTL_H

#include <linux/types.h>
#include <rdma/rdma_user_ioctl_cmds.h>

/*
 * HFI1 ioctl commands (legacy private cdev interface).
 * Struct types reference hfi2_* directly to avoid a circular include
 * dependency: hfi2-abi.h includes rdma_user_ioctl.h which includes
 * this file, so hfi2-abi.h types are not yet visible here.
 */
#define HFI1_IOCTL_ASSIGN_CTXT \
	_IOWR(RDMA_IOCTL_MAGIC, 0xE1, struct hfi2_user_info)
#define HFI1_IOCTL_CTXT_INFO _IOW(RDMA_IOCTL_MAGIC, 0xE2, struct hfi2_ctxt_info)
#define HFI1_IOCTL_USER_INFO _IOW(RDMA_IOCTL_MAGIC, 0xE3, struct hfi2_base_info)
#define HFI1_IOCTL_TID_UPDATE \
	_IOWR(RDMA_IOCTL_MAGIC, 0xE4, struct hfi1_tid_info)
#define HFI1_IOCTL_TID_FREE _IOWR(RDMA_IOCTL_MAGIC, 0xE5, struct hfi1_tid_info)
#define HFI1_IOCTL_CREDIT_UPD _IO(RDMA_IOCTL_MAGIC, 0xE6)
#define HFI1_IOCTL_RECV_CTRL _IOW(RDMA_IOCTL_MAGIC, 0xE8, int)
#define HFI1_IOCTL_POLL_TYPE _IOW(RDMA_IOCTL_MAGIC, 0xE9, int)
#define HFI1_IOCTL_ACK_EVENT _IOW(RDMA_IOCTL_MAGIC, 0xEA, unsigned long)
#define HFI1_IOCTL_SET_PKEY _IOW(RDMA_IOCTL_MAGIC, 0xEB, __u16)
#define HFI1_IOCTL_CTXT_RESET _IO(RDMA_IOCTL_MAGIC, 0xEC)
#define HFI1_IOCTL_TID_INVAL_READ \
	_IOWR(RDMA_IOCTL_MAGIC, 0xED, struct hfi1_tid_info)
#define HFI1_IOCTL_GET_VERS _IOR(RDMA_IOCTL_MAGIC, 0xEE, int)

#endif /* _LINUX__HFI1_IOCTL_H */
