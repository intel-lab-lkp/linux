/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_CTCU_H
#define _CORESIGHT_CTCU_H

#include "coresight-trace-id.h"

/* Maximum number of supported ETR devices for a single CTCU. */
#define ETR_MAX_NUM	2

/**
 * struct ctcu_etr_config
 * @atid_offset:	offset to the ATID0 Register.
 * @irq_ctrl_offset:	offset to the BYTECNTRVAL register.
 * @irq_name:		IRQ name in dt node.
 * @port_num:		in-port number of the CTCU device that connected to ETR.
 */
struct ctcu_etr_config {
	const u32 atid_offset;
	const u32 irq_ctrl_offset;
	const char *irq_name;
	const u32 port_num;
};

struct ctcu_config {
	const struct ctcu_etr_config *etr_cfgs;
	int num_etr_config;
};

/**
 * struct ctcu_byte_cntr
 * @enable:		indicates that byte_cntr function is enabled or not.
 * @read_active:	indicates that byte-cntr node is opened or not.
 * @thresh_val:		threshold to trigger a interruption.
 * @total_size		total size of transferred data.
 * @byte_cntr_irq:	IRQ number.
 * @irq_cnt:		IRQ count.
 * @wq:			workqueue of reading ETR data.
 * @read_work:		work of reading ETR data.
 * @spin_lock:		spinlock of byte cntr data.
 * @r_offset:		offset of the pointer where reading begins.
 * @w_offset:		offset of the write pointer in the ETR buffer when
 *			the byte cntr is stopped.
 * @irq_ctrl_offset:	offset to the BYTECNTVAL Register.
 * @irq_name:		IRQ name in DT.
 */
struct ctcu_byte_cntr {
	bool			enable;
	bool			read_active;
	u32			thresh_val;
	u64			total_size;
	int			byte_cntr_irq;
	atomic_t		irq_cnt;
	wait_queue_head_t	wq;
	struct work_struct	read_work;
	raw_spinlock_t		spin_lock;
	long			r_offset;
	long			w_offset;
	u32			irq_ctrl_offset;
	const char		*irq_name;
};

struct ctcu_drvdata {
	void __iomem		*base;
	struct clk		*apb_clk;
	struct device		*dev;
	struct coresight_device	*csdev;
	struct ctcu_byte_cntr   byte_cntr_data[ETR_MAX_NUM];
	raw_spinlock_t		spin_lock;
	u32			atid_offset[ETR_MAX_NUM];
	/* refcnt for each traceid of each sink */
	u8			traceid_refcnt[ETR_MAX_NUM][CORESIGHT_TRACE_ID_RES_TOP];
};

/* Generic functions */
int ctcu_get_active_port(struct coresight_device *sink, struct coresight_device *helper);

/* Byte-cntr functions */
void ctcu_byte_cntr_start(struct coresight_device *csdev, struct coresight_path *path);
void ctcu_byte_cntr_stop(struct coresight_device *csdev, struct coresight_path *path);
void ctcu_byte_cntr_init(struct device *dev, struct ctcu_drvdata *drvdata, int port_num);

#endif
