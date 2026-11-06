/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef QCOM_JENC_V420_H
#define QCOM_JENC_V420_H

#include <linux/irqreturn.h>
#include <linux/types.h>

#include "qcom_jenc_dev.h"

void qcom_jenc_v420_get_hw_caps(struct qcom_jenc_dev *jenc, u32 *caps);

int qcom_jenc_v420_get_block_width(u32 fourcc, u32 *blk_w);

int qcom_jenc_v420_get_block_height(u32 fourcc, u32 *blk_h);

int qcom_jenc_v420_acquire(struct jenc_context *ectx);

int qcom_jenc_v420_release(struct jenc_context *ectx);

void qcom_jenc_v420_prepare(struct qcom_jenc_dev *jenc);

struct qcom_jenc_queue *qcom_jenc_v420_get_buff_queue(struct jenc_context *ectx,
						      enum qcom_enc_qid id);

int qcom_jenc_v420_queue_setup(struct jenc_context *ectx, enum qcom_enc_qid id);

int qcom_jenc_v420_src_fmt_update(struct jenc_context *ectx, u32 old_fourcc, u32 new_fourcc);

int qcom_jenc_v420_retune_clk(struct jenc_context *ectx);

int qcom_jenc_v420_buffer_prepare(struct jenc_context *ectx, struct vb2_buffer *vb2);

int qcom_jenc_v420_process_exec(struct qcom_jenc_dev *jenc, struct jenc_context *ectx,
				struct vb2_buffer *vb);

irqreturn_t qcom_jenc_v420_irq_top(int irq_num, void *data);
irqreturn_t qcom_jenc_v420_irq_bot(int irq_num, void *data);

#endif /* QCOM_JENC_V420_H */
