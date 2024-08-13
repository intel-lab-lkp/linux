// SPDX-License-Identifier: GPL-2.0 or MIT
/* SPDX-FileCopyrightText: Copyright Collabora 2024 */

#include <drm/drm_gem.h>
#include <linux/iosys-map.h>
#include <linux/devcoredump.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/types.h>
#include <uapi/drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_dump.h"
#include "panthor_mmu.h"
#include "panthor_sched.h"

/* A magic value used when starting a new section in the dump */
#define PANT_DUMP_MAGIC 0x544e4150 /* PANT */
#define PANT_DUMP_MAJOR 1
#define PANT_DUMP_MINOR 0

/* keep track of where we are in the underlying buffer */
struct dump_allocator {
	u8 *start;
	u8 *curr;
	size_t pos;
	size_t capacity;
};

struct vm_dump_count {
	u64 size;
	u32 vas;
};

struct queue_count {
	u32 queues;
};

struct dump_group_args {
	struct panthor_device *ptdev;
	struct dump_allocator *alloc;
	struct panthor_group *group;
};

struct dump_va_args {
	struct panthor_device *ptdev;
	struct dump_allocator *alloc;
};

static void *alloc_bytes(struct dump_allocator *alloc, size_t size)
{
	void *ret;

	if (alloc->pos + size > alloc->capacity)
		return ERR_PTR(-ENOMEM);

	ret = alloc->curr;
	alloc->curr += size;
	alloc->pos += size;
	return ret;
}

static struct drm_panthor_dump_header *
alloc_header(struct dump_allocator *alloc, u32 type, size_t size)
{
	struct drm_panthor_dump_header *hdr;
	int header_size = sizeof(*hdr);

	hdr = alloc_bytes(alloc, header_size);
	if (IS_ERR(hdr))
		return hdr;

	hdr->magic = PANT_DUMP_MAGIC;
	hdr->header_type = type;
	hdr->header_size = header_size;
	hdr->data_size = size;
	return hdr;
}

static int dump_bo(struct panthor_device *ptdev, u8 *dst,
		   struct drm_gem_object *obj, int offset, int size)
{
	struct iosys_map map = {};
	int ret;

	ret = drm_gem_vmap_unlocked(obj, &map);
	if (ret)
		return ret;

	drm_dbg(&ptdev->base, "dumping bo %p, offset %d, size %d\n", obj,
		offset, size);

	memcpy(dst, map.vaddr + offset, size);
	drm_gem_vunmap_unlocked(obj, &map);
	return ret;
}

static int dump_va(struct dump_va_args *dump_va_args,
		   const struct drm_gpuva *va, int type)
{
	struct drm_gem_object *obj = va->gem.obj;
	const int hdr_size =
		sizeof(struct drm_panthor_dump_gpuva) + va->va.range;
	struct drm_panthor_dump_gpuva *dump_va;
	struct drm_panthor_dump_header *dump_hdr;
	u8 *bo_data;

	dump_hdr = alloc_header(dump_va_args->alloc, type, hdr_size);
	if (IS_ERR(dump_hdr))
		return PTR_ERR(dump_hdr);

	dump_va = alloc_bytes(dump_va_args->alloc, sizeof(*dump_va));
	if (IS_ERR(dump_va))
		return PTR_ERR(dump_va);

	bo_data = alloc_bytes(dump_va_args->alloc, va->va.range);
	if (IS_ERR(bo_data))
		return PTR_ERR(bo_data);

	dump_va->addr = va->va.addr;
	dump_va->range = va->va.range;

	return dump_bo(dump_va_args->ptdev, bo_data, obj, va->gem.offset,
		       va->va.range);
}

static int dump_va_cb(void *priv, const struct drm_gpuva *va)
{
	struct dump_va_args *dump_va_args = priv;
	int ret;

	ret = dump_va(dump_va_args, va, DRM_PANTHOR_DUMP_HEADER_TYPE_VM);
	if (ret)
		return ret;

	return 0;
}

static int count_va_cb(void *priv, const struct drm_gpuva *va)
{
	struct vm_dump_count *count = priv;

	count->vas++;
	count->size += va->va.range;
	return 0;
}

static void count_queues(struct queue_count *count,
			 struct drm_panthor_dump_group_info *info)
{
	count->queues += info->queue_count;
}

static int compute_dump_size(struct vm_dump_count *va_count,
			     struct queue_count *group_and_q_cnt,
			     bool job_list_is_empty)
{
	int size = 0;
	int i;

	if (job_list_is_empty) {
		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_dump_version);

		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_gpu_info);

		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_csif_info);

		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_fw_info);
	}

	for (i = 0; i < va_count->vas; i++) {
		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_dump_gpuva);
	}

	size += va_count->size;

	size += sizeof(struct drm_panthor_dump_header);
	size += sizeof(struct drm_panthor_dump_group_info);

	for (i = 0; i < group_and_q_cnt->queues; i++) {
		size += sizeof(struct drm_panthor_dump_header);
		size += sizeof(struct drm_panthor_dump_queue_info);
	}

	return size;
}

static int dump_queue_info(struct dump_group_args *dump_group_args,
			   struct drm_panthor_dump_queue_info *info)
{
	struct drm_panthor_dump_header *hdr;
	struct drm_panthor_dump_queue_info *queue_info;

	drm_dbg(&dump_group_args->ptdev->base,
		"dumping queue info for cs_id %d, gpuva: %llx, insert: %llx, extract: %llx\n",
		info->cs_id, info->ringbuf_gpuva, info->ringbuf_insert,
		info->ringbuf_extract);

	hdr = alloc_header(dump_group_args->alloc,
			   DRM_PANTHOR_DUMP_HEADER_TYPE_QUEUE_INFO,
			   sizeof(*info));
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	queue_info = alloc_bytes(dump_group_args->alloc, sizeof(*queue_info));
	if (IS_ERR(queue_info))
		return PTR_ERR(queue_info);

	*queue_info = *info;
	return 0;
}

static int dump_group_info(struct dump_group_args *dump_group_args,
			   struct drm_panthor_dump_group_info *info)
{
	struct drm_panthor_dump_header *hdr;
	struct drm_panthor_dump_group_info *group_info;
	int ret = 0;

	drm_dbg(&dump_group_args->ptdev->base,
		"dumping group info for num_queues: %d, faulty bitmask: %d\n",
		info->queue_count, info->faulty_bitmask);

	hdr = alloc_header(dump_group_args->alloc,
			   DRM_PANTHOR_DUMP_HEADER_TYPE_GROUP_INFO,
			   sizeof(*info));
	if (IS_ERR(hdr))
		return PTR_ERR(hdr);

	group_info = alloc_bytes(dump_group_args->alloc, sizeof(*group_info));
	if (IS_ERR(group_info))
		return PTR_ERR(group_info);

	*group_info = *info;

	for (int i = 0; i < info->queue_count; i++) {
		struct drm_panthor_dump_queue_info qinfo;

		ret = panthor_sched_get_queueinfo(dump_group_args->group, i,
						  &qinfo);
		if (ret)
			break;
		ret = dump_queue_info(dump_group_args, &qinfo);
		if (ret)
			break;
	}

	return ret;
}

static void clean_job_list(struct list_head *joblist)
{
	struct panthor_dump_job_entry *job, *tmp;

	list_for_each_entry_safe(job, tmp, joblist, node) {
		list_del(&job->node);
		vfree(job->mem);
		kfree(job);
	}
}

static int append_job(struct panthor_core_dump_args *args, void *mem,
		      size_t size)
{
	struct panthor_dump_job_entry *job;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;

	job->mem = mem;
	job->size = size;
	list_add_tail(&job->node, args->job_list);
	return 0;
}

static int copy_from_job_list(struct list_head *job_list, void **out_mem,
			      u32 *out_size)
{
	u32 total_size = 0;
	u32 offset = 0;
	struct panthor_dump_job_entry *entry;
	void *mem;

	list_for_each_entry(entry, job_list, node) {
		total_size += entry->size;
	}

	mem = vzalloc(total_size);
	if (!mem)
		return -ENOMEM;

	list_for_each_entry(entry, job_list, node) {
		memcpy(mem + offset, entry->mem, entry->size);
		offset += entry->size;
	}

	*out_mem = mem;
	*out_size = total_size;
	return 0;
}

int panthor_core_dump(struct panthor_core_dump_args *args)
{
	u8 *mem;
	int dump_size;
	int ret = 0;
	struct dump_allocator alloc = {};
	struct vm_dump_count va_count = {};
	struct drm_panthor_dump_header *hdr;
	struct drm_panthor_dump_version *version;
	struct drm_panthor_gpu_info *gpu_info;
	struct drm_panthor_csif_info *csif_info;
	struct drm_panthor_fw_info *fw_info;
	struct queue_count group_and_q_cnt = {};
	struct dump_va_args dump_va_args = {};
	struct drm_panthor_dump_group_info group_info;
	struct dump_group_args dump_group_args;

	panthor_vm_foreach_va(args->group_vm, count_va_cb, &va_count);

	panthor_sched_get_groupinfo(args->group, &group_info);

	count_queues(&group_and_q_cnt, &group_info);

	dump_size = compute_dump_size(&va_count, &group_and_q_cnt,
				      list_empty(args->job_list));

	mem = vzalloc(dump_size);
	if (!mem)
		return ret;

	alloc = (struct dump_allocator){
		.start = mem,
		.curr = mem,
		.pos = 0,
		.capacity = dump_size,
	};

	if (list_empty(args->job_list)) {
		hdr = alloc_header(&alloc, DRM_PANTHOR_DUMP_HEADER_TYPE_VERSION,
				   sizeof(struct drm_panthor_dump_version));
		if (IS_ERR(hdr)) {
			ret = PTR_ERR(hdr);
			goto free_valloc;
		}

		version = alloc_bytes(&alloc, sizeof(*version));
		if (IS_ERR(version)) {
			ret = PTR_ERR(version);
			goto free_valloc;
		}

		*version = (struct drm_panthor_dump_version){
			.major = PANT_DUMP_MAJOR,
			.minor = PANT_DUMP_MINOR,
		};

		hdr = alloc_header(&alloc,
				   DRM_PANTHOR_DUMP_HEADER_TYPE_GPU_INFO,
				   sizeof(args->ptdev->gpu_info));
		if (IS_ERR(hdr)) {
			ret = PTR_ERR(hdr);
			goto free_valloc;
		}

		gpu_info = alloc_bytes(&alloc, sizeof(*gpu_info));
		if (IS_ERR(gpu_info)) {
			ret = PTR_ERR(gpu_info);
			goto free_valloc;
		}

		*gpu_info = args->ptdev->gpu_info;

		hdr = alloc_header(&alloc,
				   DRM_PANTHOR_DUMP_HEADER_TYPE_CSIF_INFO,
				   sizeof(args->ptdev->csif_info));
		if (IS_ERR(hdr)) {
			ret = PTR_ERR(hdr);
			goto free_valloc;
		}

		csif_info = alloc_bytes(&alloc, sizeof(*csif_info));
		if (IS_ERR(csif_info)) {
			ret = PTR_ERR(csif_info);
			goto free_valloc;
		}

		*csif_info = args->ptdev->csif_info;

		hdr = alloc_header(&alloc, DRM_PANTHOR_DUMP_HEADER_TYPE_FW_INFO,
				   sizeof(args->ptdev->fw_info));
		if (IS_ERR(hdr)) {
			ret = PTR_ERR(hdr);
			goto free_valloc;
		}

		fw_info = alloc_bytes(&alloc, sizeof(*fw_info));
		if (IS_ERR(fw_info)) {
			ret = PTR_ERR(fw_info);
			goto free_valloc;
		}

		*fw_info = args->ptdev->fw_info;
	}

	dump_va_args.ptdev = args->ptdev;
	dump_va_args.alloc = &alloc;
	ret = panthor_vm_foreach_va(args->group_vm, dump_va_cb, &dump_va_args);
	if (ret)
		goto free_valloc;

	dump_group_args =
		(struct dump_group_args){ args->ptdev, &alloc, args->group };
	panthor_sched_get_groupinfo(args->group, &group_info);
	dump_group_info(&dump_group_args, &group_info);

	if (alloc.pos < dump_size)
		drm_warn(&args->ptdev->base,
			 "dump size mismatch: expected %d, got %zu\n",
			 dump_size, alloc.pos);

	if (args->append) {
		ret = append_job(args, alloc.start, alloc.pos);
		if (ret)
			goto free_valloc;
	} else if (!list_empty(args->job_list)) {
		void *mem;
		u32 size;

		/* append ourselves */
		append_job(args, alloc.start, alloc.pos);
		if (ret)
			goto free_valloc;

		ret = copy_from_job_list(args->job_list, &mem, &size);
		if (ret)
			goto free_valloc;

		dev_coredumpv(args->ptdev->base.dev, mem, size, GFP_KERNEL);
		clean_job_list(args->job_list);
	} else {
		dev_coredumpv(args->ptdev->base.dev, alloc.start, alloc.pos,
			      GFP_KERNEL);
	}

	return ret;

free_valloc:
	clean_job_list(args->job_list);
	vfree(mem);
	return ret;
}
