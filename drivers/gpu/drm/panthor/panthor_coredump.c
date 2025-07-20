// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2025 Google LLC */

#include <drm/drm_drv.h>
#include <drm/drm_print.h>
#include <drm/drm_managed.h>
#include <generated/utsrelease.h>
#include <linux/devcoredump.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "panthor_coredump.h"
#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_gem.h"
#include "panthor_mmu.h"
#include "panthor_regs.h"
#include "panthor_sched.h"

/**
 * enum panthor_coredump_mask - Coredump state
 */
enum panthor_coredump_mask {
	PANTHOR_COREDUMP_GROUP = BIT(0),
	PANTHOR_COREDUMP_GPU = BIT(1),
	PANTHOR_COREDUMP_GLB = BIT(2),
	PANTHOR_COREDUMP_CSG = BIT(3),
	PANTHOR_COREDUMP_CS = BIT(4),
	PANTHOR_COREDUMP_AS = BIT(5),
	PANTHOR_COREDUMP_VMA = BIT(6),
};

/**
 * struct panthor_coredump_header - Coredump header
 */
struct panthor_coredump_header {
	enum panthor_coredump_reason reason;
	ktime_t timestamp;
};

/**
 * struct panthor_coredump - Coredump
 */
struct panthor_coredump {
	/** @ptdev: Device. */
	struct panthor_device *ptdev;

	/** @gfp: Allocation flags for panthor_coredump_capture. */
	gfp_t gfp;

	/** @work: Bottom half of panthor_coredump_capture. */
	struct work_struct work;

	/** @header: Header. */
	struct panthor_coredump_header header;

	/** @mask: Bitmask of captured states. */
	u32 mask;

	struct panthor_coredump_group_state group;
	struct panthor_coredump_gpu_state gpu;
	struct panthor_coredump_glb_state glb;
	struct panthor_coredump_csg_state csg;
	struct panthor_coredump_cs_state cs[MAX_CS_PER_CSG];
	struct panthor_coredump_as_state as;
	struct panthor_coredump_vma_state *vma;
	u32 vma_count;

	/* @data: Serialized coredump data. */
	void *data;

	/* @size: Serialized coredump size. */
	size_t size;
};

static const char *reason_str(enum panthor_coredump_reason reason)
{
	switch (reason) {
	case PANTHOR_COREDUMP_REASON_MMU_FAULT:
		return "MMU_FAULT";
	case PANTHOR_COREDUMP_REASON_CSG_REQ_TIMEOUT:
		return "CSG_REQ_TIMEOUT";
	case PANTHOR_COREDUMP_REASON_CSG_UNKNOWN_STATE:
		return "CSG_UNKNOWN_STATE";
	case PANTHOR_COREDUMP_REASON_CSG_PROGRESS_TIMEOUT:
		return "CSG_PROGRESS_TIMEOUT";
	case PANTHOR_COREDUMP_REASON_CS_FATAL:
		return "CS_FATAL";
	case PANTHOR_COREDUMP_REASON_CS_FAULT:
		return "CS_FAULT";
	case PANTHOR_COREDUMP_REASON_CS_TILER_OOM:
		return "CS_TILER_OOM";
	case PANTHOR_COREDUMP_REASON_JOB_TIMEOUT:
		return "JOB_TIMEOUT";
	default:
		return "UNKNOWN";
	}
}

static void print_vma(struct drm_printer *p,
		      const struct panthor_coredump_vma_state *vma, u32 vma_id,
		      size_t *max_dyn_size)
{
	struct panthor_gem_object *bo = vma->bo;

	if (!vma_id)
		drm_puts(p, "vma:\n");

	drm_printf(p, "  - flags: 0x%x\n", vma->flags);
	drm_printf(p, "    iova: 0x%llx\n", vma->iova);
	drm_printf(p, "    size: 0x%llx\n", vma->size);

	if (!bo)
		return;

	/* bo->label is dynamic */
	if (max_dyn_size) {
		drm_puts(p, "    label: |\n");
		drm_puts(p, "      \n");
		*max_dyn_size += 32;
	} else {
		scoped_guard(mutex, &bo->label.lock)
		{
			if (bo->label.str) {
				drm_puts(p, "    label: |\n");
				drm_printf(p, "      %.32s\n", bo->label.str);
			}
		}
	}
}

static void print_as(struct drm_printer *p,
		     const struct panthor_coredump_as_state *as, u32 as_id)
{
	drm_printf(p, "as%d:\n", as_id);
	drm_printf(p, "  FAULTSTATUS: 0x%x\n", as->faultstatus);
	drm_printf(p, "  FAULTADDRESS: 0x%llx\n", as->faultaddress);
	drm_printf(p, "  FAULTEXTRA: 0x%llx\n", as->faultextra);
}

static void print_cs(struct drm_printer *p,
		     const struct panthor_coredump_cs_state *cs, u32 cs_id)
{
	drm_printf(p, "cs%d:\n", cs_id);
	drm_printf(p, "  STREAM_FEATURES: 0x%x\n", cs->features);

	drm_printf(p, "  CS_REQ: 0x%x\n", cs->req);
	drm_printf(p, "  CS_CONFIG: 0x%x\n", cs->config);
	drm_printf(p, "  CS_BASE: 0x%llx\n", cs->base);
	drm_printf(p, "  CS_SIZE: 0x%x\n", cs->size);

	drm_printf(p, "  CS_ACK: 0x%x\n", cs->ack);
	drm_printf(p, "  CS_STATUS_CMD_PTR: 0x%llx\n", cs->status_cmd_ptr);
	drm_printf(p, "  CS_STATUS_WAIT: 0x%x\n", cs->status_wait);
	drm_printf(p, "  CS_STATUS_REQ_RESOURCE: 0x%x\n",
		   cs->status_req_resource);
	drm_printf(p, "  CS_STATUS_SCOREBOARDS: 0x%x\n",
		   cs->status_scoreboards);
	drm_printf(p, "  CS_STATUS_BLOCKED_REASON: 0x%x\n",
		   cs->status_blocked_reason);
	drm_printf(p, "  CS_FAULT: 0x%x\n", cs->fault);
	drm_printf(p, "  CS_FATAL: 0x%x\n", cs->fatal);
	drm_printf(p, "  CS_FAULT_INFO: 0x%llx\n", cs->fault_info);
	drm_printf(p, "  CS_FATAL_INFO: 0x%llx\n", cs->fatal_info);

	drm_printf(p, "  CS_INSERT: 0x%llx\n", cs->insert);
	drm_printf(p, "  CS_EXTRACT_INIT: 0x%llx\n", cs->extract_init);
	drm_printf(p, "  CS_EXTRACT: 0x%llx\n", cs->extract);
	drm_printf(p, "  CS_ACTIVE: 0x%x\n", cs->active);
}

static void print_csg(struct drm_printer *p,
		      const struct panthor_coredump_csg_state *csg, u32 csg_id)
{
	drm_printf(p, "csg%d:\n", csg_id);
	drm_printf(p, "  GROUP_FEATURES: 0x%x\n", csg->features);
	drm_printf(p, "  GROUP_STREAM_NUM: 0x%x\n", csg->stream_num);

	drm_printf(p, "  CSG_REQ: 0x%x\n", csg->req);
	drm_printf(p, "  CSG_ALLOW_COMPUTE: 0x%llx\n", csg->allow_compute);
	drm_printf(p, "  CSG_ALLOW_FRAGMENT: 0x%llx\n", csg->allow_fragment);
	drm_printf(p, "  CSG_ALLOW_OTHER: 0x%x\n", csg->allow_other);
	drm_printf(p, "  CSG_EP_REQ: 0x%x\n", csg->ep_req);
	drm_printf(p, "  CSG_CONFIG: 0x%x\n", csg->config);

	drm_printf(p, "  CSG_ACK: 0x%x\n", csg->ack);
	drm_printf(p, "  CSG_STATUS_EP_CURRENT: 0x%x\n",
		   csg->status_ep_current);
	drm_printf(p, "  CSG_STATUS_EP_REQ: 0x%x\n", csg->status_ep_req);
	drm_printf(p, "  CSG_STATUS_STATE: 0x%x\n", csg->status_state);
	drm_printf(p, "  CSG_RESOURCE_DEP: 0x%x\n", csg->resource_dep);
}

static void print_glb(struct drm_printer *p,
		      const struct panthor_coredump_glb_state *glb)
{
	drm_puts(p, "glb:\n");
	drm_printf(p, "  GLB_VERSION: 0x%x\n", glb->version);
	drm_printf(p, "  GLB_FEATURES: 0x%x\n", glb->features);
	drm_printf(p, "  GLB_GROUP_NUM: 0x%x\n", glb->group_num);
	drm_printf(p, "  GLB_REQ: 0x%x\n", glb->req);
	drm_printf(p, "  GLB_ACK: 0x%x\n", glb->ack);
}

static void print_gpu(struct drm_printer *p,
		      const struct panthor_coredump_gpu_state *gpu,
		      const struct drm_panthor_gpu_info *info)
{
	drm_puts(p, "gpu:\n");
	drm_printf(p, "  GPU_ID: 0x%x\n", info->gpu_id);
	drm_printf(p, "  L2_FEATURES: 0x%x\n", info->l2_features);
	drm_printf(p, "  CORE_FEATURES: 0x%x\n", info->core_features);
	drm_printf(p, "  TILER_FEATURES: 0x%x\n", info->tiler_features);
	drm_printf(p, "  MEM_FEATURES: 0x%x\n", info->mem_features);
	drm_printf(p, "  MMU_FEATURES: 0x%x\n", info->mmu_features);
	drm_printf(p, "  AS_PRESENT: 0x%x\n", info->as_present);
	drm_printf(p, "  CSF_ID: 0x%x\n", info->csf_id);
	drm_printf(p, "  MMU_FEATURES: 0x%x\n", info->mmu_features);

	if (gpu) {
		drm_printf(p, "  GPU_STATUS: 0x%x\n", gpu->gpu_status);
		drm_printf(p, "  GPU_FAULTSTATUS: 0x%x\n",
			   gpu->gpu_faultstatus);
		drm_printf(p, "  GPU_FAULTADDRESS: 0x%llx\n",
			   gpu->gpu_faultaddress);
		drm_printf(p, "  L2_CONFIG: 0x%x\n", gpu->l2_config);
	}

	drm_printf(p, "  THREAD_MAX_THREADS: 0x%x\n", info->max_threads);
	drm_printf(p, "  THREAD_MAX_WORKGROUP_SIZE: 0x%x\n",
		   info->thread_max_workgroup_size);
	drm_printf(p, "  THREAD_MAX_BARRIER_SIZE: 0x%x\n",
		   info->thread_max_barrier_size);
	drm_printf(p, "  THREAD_FEATURES: 0x%x\n", info->thread_features);
	drm_printf(p, "  TEXTURE_FEATURES_0: 0x%x\n",
		   info->texture_features[0]);
	drm_printf(p, "  TEXTURE_FEATURES_1: 0x%x\n",
		   info->texture_features[1]);
	drm_printf(p, "  TEXTURE_FEATURES_2: 0x%x\n",
		   info->texture_features[2]);
	drm_printf(p, "  TEXTURE_FEATURES_3: 0x%x\n",
		   info->texture_features[3]);

	if (gpu) {
		drm_printf(p, "  DOORBELL_FEATURES: 0x%x\n",
			   gpu->doorbell_features);
	}

	drm_printf(p, "  SHADER_PRESENT: 0x%llx\n", info->shader_present);
	drm_printf(p, "  TILER_PRESENT: 0x%llx\n", info->tiler_present);
	drm_printf(p, "  L2_PRESENT: 0x%llx\n", info->l2_present);
	drm_printf(p, "  REVIDR: 0x%x\n", info->gpu_rev);
	drm_printf(p, "  AMBA_FEATURES: 0x%x\n", info->coherency_features);

	if (gpu) {
		drm_printf(p, "  AMBA_ENABLE: 0x%x\n", gpu->amba_enable);
		drm_printf(p, "  MCU_STATUS: 0x%x\n", gpu->mcu_status);
		drm_printf(p, "  MCU_FEATURES: 0x%x\n", gpu->mcu_features);
	}
}

static void print_group(struct drm_printer *p,
			const struct panthor_coredump_group_state *group)
{
	drm_puts(p, "group:\n");
	drm_printf(p, "  priority: %d\n", group->priority);
	drm_printf(p, "  queue_count: %u\n", group->queue_count);
	drm_printf(p, "  pid: %d\n", group->pid);
	drm_printf(p, "  comm: %s\n", group->comm);
	drm_printf(p, "  destroyed: %d\n", group->destroyed);
	drm_printf(p, "  csg_id: %d\n", group->csg_id);
}

static void print_header(struct drm_printer *p,
			 const struct panthor_coredump_header *header,
			 const struct drm_driver *drv)
{
	drm_puts(p, "header:\n");
	drm_puts(p, "  kernel: " UTS_RELEASE "\n");
	drm_puts(p, "  module: " KBUILD_MODNAME "\n");
	drm_printf(p, "  driver_version: %d.%d\n", drv->major, drv->minor);

	drm_printf(p, "  reason: %s\n", reason_str(header->reason));
	drm_printf(p, "  timestamp: %lld\n", ktime_to_ns(header->timestamp));
}

static void print_cd(struct drm_printer *p, const struct panthor_coredump *cd,
		     size_t *max_dyn_size)
{
	/* in YAML format */
	drm_puts(p, "---\n");
	print_header(p, &cd->header, cd->ptdev->base.driver);

	if (cd->mask & PANTHOR_COREDUMP_GROUP)
		print_group(p, &cd->group);

	/* many gpu states are static and are captured in drm_panthor_gpu_info */
	print_gpu(p, cd->mask & PANTHOR_COREDUMP_GPU ? &cd->gpu : NULL,
		  &cd->ptdev->gpu_info);

	if (cd->mask & PANTHOR_COREDUMP_GLB)
		print_glb(p, &cd->glb);

	if (cd->mask & PANTHOR_COREDUMP_CSG) {
		print_csg(p, &cd->csg, cd->group.csg_id);
	}

	if (cd->mask & PANTHOR_COREDUMP_CS) {
		for (u32 i = 0; i < cd->group.queue_count; i++)
			print_cs(p, &cd->cs[i], i);
	}

	if (cd->mask & PANTHOR_COREDUMP_AS) {
		const u32 as_id = cd->csg.config & 0xf;

		print_as(p, &cd->as, as_id);
	}

	if (cd->mask & PANTHOR_COREDUMP_VMA) {
		for (u32 i = 0; i < cd->vma_count; i++)
			print_vma(p, &cd->vma[i], i, max_dyn_size);
	}
}

static void process_cd(struct panthor_device *ptdev,
		       struct panthor_coredump *cd)
{
	struct drm_print_iterator iter = {
		.remain = SSIZE_MAX,
	};
	struct drm_printer p = drm_coredump_printer(&iter);
	size_t max_dyn_size = 0;

	print_cd(&p, cd, &max_dyn_size);
	if (max_dyn_size > iter.remain)
		return;

	iter.remain = SSIZE_MAX - iter.remain + max_dyn_size;
	iter.data = kvmalloc(iter.remain, GFP_USER);
	if (!iter.data)
		return;

	cd->data = iter.data;
	cd->size = iter.remain;

	drm_info(&ptdev->base, "generating coredump of estimated size %zu\n",
		 cd->size);

	p = drm_coredump_printer(&iter);
	print_cd(&p, cd, NULL);

	cd->size -= iter.remain;

	/* free vma now */
	if (cd->mask & PANTHOR_COREDUMP_VMA) {
		for (u32 i = 0; i < cd->vma_count; i++) {
			struct panthor_coredump_vma_state *vma = &cd->vma[i];

			drm_gem_object_put(&vma->bo->base.base);
		}
		kfree(cd->vma);

		cd->mask &= ~PANTHOR_COREDUMP_VMA;
	}
}

static void capture_as(struct panthor_device *ptdev,
		       struct panthor_coredump_as_state *as, u32 as_id)
{
	as->faultstatus = gpu_read(ptdev, AS_FAULTSTATUS(as_id));
	as->faultaddress = gpu_read64(ptdev, AS_FAULTADDRESS(as_id));
	as->faultextra = gpu_read64(ptdev, AS_FAULTEXTRA(as_id));
}

static void capture_cs(struct panthor_device *ptdev,
		       struct panthor_coredump_cs_state *cs, u32 csg_id,
		       u32 cs_id, const struct panthor_group *group)
{
	const struct panthor_fw_cs_iface *cs_iface =
		panthor_fw_get_cs_iface(ptdev, csg_id, cs_id);
	const struct panthor_fw_ringbuf_input_iface *input_iface;
	const struct panthor_fw_ringbuf_output_iface *output_iface;

	cs->features = cs_iface->control->features;

	cs->req = cs_iface->input->req;
	cs->config = cs_iface->input->config;
	cs->base = cs_iface->input->ringbuf_base;
	cs->size = cs_iface->input->ringbuf_size;

	cs->ack = cs_iface->output->ack;
	cs->status_cmd_ptr = cs_iface->output->status_cmd_ptr;
	cs->status_wait = cs_iface->output->status_wait;
	cs->status_req_resource = cs_iface->output->status_req_resource;
	cs->status_scoreboards = cs_iface->output->status_scoreboards;
	cs->status_blocked_reason = cs_iface->output->status_blocked_reason;
	cs->fault = cs_iface->output->fault;
	cs->fatal = cs_iface->output->fatal;
	cs->fault_info = cs_iface->output->fault_info;
	cs->fatal_info = cs_iface->output->fatal_info;

	panthor_group_get_ringbuf_iface(group, cs_id, &input_iface,
					&output_iface);

	cs->insert = input_iface->insert;
	cs->extract_init = input_iface->extract;

	cs->extract = output_iface->extract;
	cs->active = output_iface->active;
}

static void capture_csg(struct panthor_device *ptdev,
			struct panthor_coredump_csg_state *csg, u32 csg_id)
{
	const struct panthor_fw_csg_iface *csg_iface =
		panthor_fw_get_csg_iface(ptdev, csg_id);

	csg->features = csg_iface->control->features;
	csg->stream_num = csg_iface->control->stream_num;

	csg->req = csg_iface->input->req;
	csg->allow_compute = csg_iface->input->allow_compute;
	csg->allow_fragment = csg_iface->input->allow_fragment;
	csg->allow_other = csg_iface->input->allow_other;
	csg->ep_req = csg_iface->input->endpoint_req;
	csg->config = csg_iface->input->config;

	csg->ack = csg_iface->output->ack;
	csg->status_ep_current = csg_iface->output->status_endpoint_current;
	csg->status_ep_req = csg_iface->output->status_endpoint_req;
	csg->status_state = csg_iface->output->status_state;
	csg->resource_dep = csg_iface->output->resource_dep;
}

static void capture_glb(struct panthor_device *ptdev,
			struct panthor_coredump_glb_state *glb)
{
	const struct panthor_fw_global_iface *glb_iface =
		panthor_fw_get_glb_iface(ptdev);

	glb->version = glb_iface->control->version;
	glb->features = glb_iface->control->features;
	glb->group_num = glb_iface->control->group_num;
	glb->req = glb_iface->input->req;
	glb->ack = glb_iface->output->ack;
}

static void capture_gpu(struct panthor_device *ptdev,
			struct panthor_coredump_gpu_state *gpu)
{
	gpu->gpu_status = gpu_read(ptdev, GPU_STATUS);
	gpu->gpu_faultstatus = gpu_read(ptdev, GPU_FAULT_STATUS);
	gpu->gpu_faultaddress = gpu_read64(ptdev, GPU_FAULT_ADDR);
	gpu->l2_config = gpu_read(ptdev, GPU_L2_CONFIG);
	gpu->doorbell_features = gpu_read(ptdev, GPU_DOORBELL_FEATURES);
	gpu->amba_enable = gpu_read(ptdev, GPU_COHERENCY_PROTOCOL);
	gpu->mcu_status = gpu_read(ptdev, MCU_STATUS);
	gpu->mcu_features = gpu_read(ptdev, MCU_FEATURES);
}

static void capture_cd(struct panthor_device *ptdev,
		       struct panthor_coredump *cd, struct panthor_group *group)
{
	struct panthor_vm *vm;

	drm_info(&ptdev->base, "capturing coredump states\n");

	if (group) {
		panthor_group_capture_coredump(group, &cd->group);
		cd->mask |= PANTHOR_COREDUMP_GROUP;
	}

	/* remaining states require the device to be powered on */
	if (!pm_runtime_active(ptdev->base.dev))
		return;

	capture_gpu(ptdev, &cd->gpu);
	cd->mask |= PANTHOR_COREDUMP_GPU;

	capture_glb(ptdev, &cd->glb);
	cd->mask |= PANTHOR_COREDUMP_GLB;

	/* remaining states require an active group */
	if (!group || cd->group.csg_id < 0)
		return;

	capture_csg(ptdev, &cd->csg, cd->group.csg_id);
	cd->mask |= PANTHOR_COREDUMP_CSG;

	for (u32 i = 0; i < cd->group.queue_count; i++)
		capture_cs(ptdev, &cd->cs[i], cd->group.csg_id, i, group);
	cd->mask |= PANTHOR_COREDUMP_CS;

	vm = panthor_group_vm(group);

	capture_as(ptdev, &cd->as, panthor_vm_as(vm));
	cd->mask |= PANTHOR_COREDUMP_AS;

	cd->vma = panthor_vm_capture_coredump(vm, &cd->vma_count, cd->gfp);
	if (cd->vma_count)
		cd->mask |= PANTHOR_COREDUMP_VMA;
}

static void panthor_coredump_free(void *data)
{
	struct panthor_coredump *cd = data;
	struct panthor_device *ptdev = cd->ptdev;

	kvfree(cd->data);
	kfree(cd);

	atomic_set(&ptdev->coredump.pending, 0);
}

static ssize_t panthor_coredump_read(char *buffer, loff_t offset, size_t count,
				     void *data, size_t datalen)
{
	const struct panthor_coredump *cd = data;

	if (offset >= cd->size)
		return 0;

	if (count > cd->size - offset)
		count = cd->size - offset;

	memcpy(buffer, cd->data + offset, count);

	return count;
}

static void panthor_coredump_process_work(struct work_struct *work)
{
	struct panthor_coredump *cd =
		container_of(work, struct panthor_coredump, work);
	struct panthor_device *ptdev = cd->ptdev;

	process_cd(ptdev, cd);

	dev_coredumpm(ptdev->base.dev, THIS_MODULE, cd, 0, GFP_KERNEL,
		      panthor_coredump_read, panthor_coredump_free);
}

void panthor_coredump_capture(struct panthor_coredump *cd,
			      struct panthor_group *group)
{
	struct panthor_device *ptdev = cd->ptdev;

	capture_cd(ptdev, cd, group);

	queue_work(system_unbound_wq, &cd->work);
}

struct panthor_coredump *
panthor_coredump_alloc(struct panthor_device *ptdev,
		       enum panthor_coredump_reason reason, gfp_t gfp)
{
	struct panthor_coredump *cd;

	/* reject all but the first coredump until it is handled */
	if (atomic_cmpxchg(&ptdev->coredump.pending, 0, 1)) {
		drm_dbg(&ptdev->base, "skip subsequent coredump\n");
		return NULL;
	}

	cd = kzalloc(sizeof(*cd), gfp);
	if (!cd) {
		atomic_set(&ptdev->coredump.pending, 0);
		return NULL;
	}

	cd->ptdev = ptdev;
	cd->gfp = gfp;
	INIT_WORK(&cd->work, panthor_coredump_process_work);

	cd->header.reason = reason;
	cd->header.timestamp = ktime_get_real();

	return cd;
}
