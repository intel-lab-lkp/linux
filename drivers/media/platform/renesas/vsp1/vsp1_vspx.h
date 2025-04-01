/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * vsp1_vspx.h  --  R-Car Gen 4 VSPX
 *
 * Copyright (C) 2025 Ideas On Board Oy
 * Copyright (C) 2025 Renesas Electronics Corporation
 */
#ifndef __VSP1_VSPX_H__
#define __VSP1_VSPX_H__

#include <linux/container_of.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include <media/vsp1.h>

#include "vsp1.h"
#include "vsp1_pipe.h"

/* Pixel format for ConfigDMA buffers. */
#define V4L2_META_FMT_RCAR_V4H	v4l2_fourcc('R', 'C', 'A', '4')

/*
 * struct vsp1_vspx_pipeline - VSPX pipeline
 * @pipe: the VSP1 pipeline
 * @partition: the pre-calculated partition used by the pipeline
 * @vspx_lock: protect access to the VSPX configuration
 * @processing: VSPX busy flag
 * @jobs_lock: protect the jobs queue
 * @jobs: jobs queue
 * @vspx_frame_end: frame end callback
 * @frame_end_data: data for the frame end callback
 */
struct vsp1_vspx_pipeline {
	struct vsp1_pipeline pipe;
	struct vsp1_partition partition;

	/* Protects the pipeline configuration */
	spinlock_t vspx_lock;
	bool processing;
	bool enabled;

	/* Protects the jobs list */
	spinlock_t jobs_lock;
	struct list_head jobs;

	void (*vspx_frame_end)(void *frame_end_data);
	void *frame_end_data;
};

static inline struct vsp1_vspx_pipeline *
to_vsp1_vspx_pipeline(struct vsp1_pipeline *pipe)
{
	return container_of(pipe, struct vsp1_vspx_pipeline, pipe);
}

/*
 * struct vsp1_vspx_job - VSPX transfer job
 * @dl: Display list populated by vsp1_isp_job_prepare
 * @job_queue: List handle
 */
struct vsp1_vspx_job {
	struct vsp1_dl_list *dl;
	struct list_head job_queue;
};

/*
 * struct vsp1_vspx - VSPX device
 * @vsp1: the VSP1 device
 * @pipe: the VSPX pipeline
 */
struct vsp1_vspx {
	struct vsp1_device *vsp1;
	struct vsp1_vspx_pipeline pipe;
};

static inline struct vsp1_vspx *
to_vsp1_vspx(struct vsp1_vspx_pipeline *vspx_pipe)
{
	return container_of(vspx_pipe, struct vsp1_vspx, pipe);
}

int vsp1_vspx_init(struct vsp1_device *vsp1);
void vsp1_vspx_cleanup(struct vsp1_device *vsp1);

#endif /* __VSP1_VSPX_H__ */
