/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_FAULT_H
#define _HFI2_FAULT_H

#include <linux/fault-inject.h>
#include <linux/dcache.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <rdma/rdma_vt.h>

#include "hfi2.h"

struct hfi2_ibdev;

#if defined(CONFIG_FAULT_INJECTION) && defined(CONFIG_FAULT_INJECTION_DEBUG_FS)
struct fault {
	struct fault_attr attr;
	struct dentry *dir;
	u64 n_rxfaults[(1U << BITS_PER_BYTE)];
	u64 n_txfaults[(1U << BITS_PER_BYTE)];
	u64 fault_skip;
	u64 skip;
	u64 fault_skip_usec;
	unsigned long skip_usec;
	unsigned long opcodes[(1U << BITS_PER_BYTE) / BITS_PER_LONG];
	bool enable;
	bool suppress_err;
	bool opcode;
	u8 direction;
};

int hfi2_fault_init_debugfs(struct hfi2_ibdev *ibd);
bool hfi2_dbg_should_fault_tx(struct rvt_qp *qp, u32 opcode);
bool hfi2_dbg_should_fault_rx(struct hfi2_packet *packet);
bool hfi2_dbg_fault_suppress_err(struct hfi2_ibdev *ibd);
void hfi2_fault_exit_debugfs(struct hfi2_ibdev *ibd);

#else

static inline int hfi2_fault_init_debugfs(struct hfi2_ibdev *ibd)
{
	return 0;
}

static inline bool hfi2_dbg_should_fault_rx(struct hfi2_packet *packet)
{
	return false;
}

static inline bool hfi2_dbg_should_fault_tx(struct rvt_qp *qp,
					    u32 opcode)
{
	return false;
}

static inline bool hfi2_dbg_fault_suppress_err(struct hfi2_ibdev *ibd)
{
	return false;
}

static inline void hfi2_fault_exit_debugfs(struct hfi2_ibdev *ibd)
{
}
#endif
#endif /* _HFI2_FAULT_H */
