// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - debug interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 */

#include <linux/types.h>
#include <linux/debugfs.h>
#include "wave6-vpu-core.h"
#include "wave6-vpu-dbg.h"

static int wave6_vpu_dbg_instance(struct seq_file *s, void *data)
{
	struct vpu_instance *inst = s->private;
	struct vpu_performance_info *perf = &inst->performance;
	struct vb2_queue *vq;
	s64 tmp;
	s64 fps;
	s64 duration;

	if (!inst->v4l2_fh.m2m_ctx)
		return 0;

	seq_printf(s, "[%s]\n",
		   inst->type == VPU_INST_TYPE_DEC ? "Decoder" : "Encoder");

	seq_printf(s, "%s : product 0x%x, fw_ver %d.%d.%d(r%d), hw_ver 0x%x\n",
		   dev_name(inst->dev->dev),
		   inst->dev->attr.product_code,
		   FW_VERSION_MAJOR(inst->dev->attr.fw_version),
		   FW_VERSION_MINOR(inst->dev->attr.fw_version),
		   FW_VERSION_REL(inst->dev->attr.fw_version),
		   inst->dev->attr.fw_revision,
		   inst->dev->attr.hw_version);

	seq_printf(s, "state = %s\n",
		   wave6_vpu_instance_state_name(inst->state));

	vq = v4l2_m2m_get_src_vq(inst->v4l2_fh.m2m_ctx);
	seq_printf(s, "output (%2d, %2d): fmt = %c%c%c%c %d x %d, %d;\n",
		   vb2_is_streaming(vq),
		   vb2_get_num_buffers(vq),
		   inst->src_fmt.pixelformat,
		   inst->src_fmt.pixelformat >> 8,
		   inst->src_fmt.pixelformat >> 16,
		   inst->src_fmt.pixelformat >> 24,
		   inst->src_fmt.width,
		   inst->src_fmt.height,
		   vq->last_buffer_dequeued);

	vq = v4l2_m2m_get_dst_vq(inst->v4l2_fh.m2m_ctx);
	seq_printf(s, "capture(%2d, %2d): fmt = %c%c%c%c %d x %d, %d;\n",
		   vb2_is_streaming(vq),
		   vb2_get_num_buffers(vq),
		   inst->dst_fmt.pixelformat,
		   inst->dst_fmt.pixelformat >> 8,
		   inst->dst_fmt.pixelformat >> 16,
		   inst->dst_fmt.pixelformat >> 24,
		   inst->dst_fmt.width,
		   inst->dst_fmt.height,
		   vq->last_buffer_dequeued);

	seq_printf(s, "crop: (%d, %d) %d x %d\n",
		   inst->crop.left,
		   inst->crop.top,
		   inst->crop.width,
		   inst->crop.height);

	if (inst->scaler_info.enable)
		seq_printf(s, "scale: %d x %d\n",
			   inst->scaler_info.width, inst->scaler_info.height);

	seq_printf(s, "queued src %d, dst %d, process %d, sequence %d, error %d, drain %d:%d\n",
		   inst->queued_src_buf_num,
		   inst->queued_dst_buf_num,
		   inst->processed_buf_num,
		   inst->sequence,
		   inst->error_buf_num,
		   inst->v4l2_fh.m2m_ctx->out_q_ctx.buffered,
		   inst->eos);

	seq_puts(s, "fps");
	tmp = MSEC_PER_SEC * inst->processed_buf_num;
	if (perf->ts_last > perf->ts_first + NSEC_PER_MSEC) {
		fps = DIV_ROUND_CLOSEST(tmp, (perf->ts_last - perf->ts_first) / NSEC_PER_MSEC);
		seq_printf(s, " actual: %lld;", fps);
	}
	duration = perf->total_sw_time / NSEC_PER_MSEC;
	if (duration > 0) {
		fps = DIV_ROUND_CLOSEST(tmp, duration);
		seq_printf(s, " sw: %lld;", fps);
	}
	duration = perf->total_hw_time / NSEC_PER_MSEC;
	if (duration > 0) {
		fps = DIV_ROUND_CLOSEST(tmp, duration);
		seq_printf(s, " hw: %lld", fps);
	}
	seq_putc(s, '\n');

	seq_printf(s, "latency(ms) first: %llu.%06llu, max %llu.%06llu, setup %llu.%06llu\n",
		   perf->latency_first / NSEC_PER_MSEC,
		   perf->latency_first % NSEC_PER_MSEC,
		   perf->latency_max / NSEC_PER_MSEC,
		   perf->latency_max % NSEC_PER_MSEC,
		   (perf->ts_first - perf->ts_start) / NSEC_PER_MSEC,
		   (perf->ts_first - perf->ts_start) % NSEC_PER_MSEC);

	seq_printf(s, "process frame time(ms) min: %llu.%06llu, max %llu.%06llu\n",
		   perf->min_process_time / NSEC_PER_MSEC,
		   perf->min_process_time % NSEC_PER_MSEC,
		   perf->max_process_time / NSEC_PER_MSEC,
		   perf->max_process_time % NSEC_PER_MSEC);

	if (inst->type == VPU_INST_TYPE_DEC) {
		seq_printf(s, "%s order\n",
			   inst->disp_mode == DISP_MODE_DISP_ORDER ? "display" : "decode");
	} else {
		struct enc_info *p_enc_info = &inst->codec_info->enc_info;
		struct enc_codec_param *param = &p_enc_info->open_param.codec_param;

		seq_printf(s, "profile %d, level %d, tier %d\n",
			   param->profile, param->level, param->tier);

		seq_printf(s, "frame_rate %d, idr_period %d, intra_period %d\n",
			   param->frame_rate, param->idr_period, param->intra_period);

		seq_printf(s, "rc %d, mode %d, bitrate %d\n",
			   param->en_rate_control,
			   param->rc_mode,
			   param->bitrate);

		seq_printf(s, "qp %d, i_qp [%d, %d], p_qp [%d, %d], b_qp [%d, %d]\n",
			   param->qp,
			   param->min_qp_i, param->max_qp_i,
			   param->min_qp_p, param->max_qp_p,
			   param->min_qp_b, param->max_qp_b);
	}

	return 0;
}

static int wave6_vpu_dbg_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, wave6_vpu_dbg_instance, inode->i_private);
}

static const struct file_operations wave6_vpu_dbg_fops = {
	.owner = THIS_MODULE,
	.open = wave6_vpu_dbg_open,
	.release = single_release,
	.read = seq_read,
	.llseek = seq_lseek,
};

int wave6_vpu_create_dbgfs_file(struct vpu_instance *inst)
{
	char name[64];

	if (WARN_ON(!inst || !inst->dev))
		return -EINVAL;

	if (IS_ERR_OR_NULL(inst->dev->debugfs))
		return 0;

	scnprintf(name, sizeof(name), "instance.%d", inst->id);
	inst->debugfs = debugfs_create_file((const char *)name,
					    0444,
					    inst->dev->debugfs,
					    inst,
					    &wave6_vpu_dbg_fops);

	return 0;
}

void wave6_vpu_remove_dbgfs_file(struct vpu_instance *inst)
{
	if (!inst || !inst->debugfs)
		return;

	debugfs_remove(inst->debugfs);
	inst->debugfs = NULL;
}
