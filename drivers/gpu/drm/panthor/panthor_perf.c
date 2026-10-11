// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2023 Collabora Ltd */
/* Copyright 2025 Arm ltd. */

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/panthor_drm.h>
#include <drm/drm_print.h>
#include <linux/bitops.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_fw_regs.h"
#include "panthor_gem.h"
#include "panthor_gpu.h"
#include "panthor_gpu_regs.h"
#include "panthor_mmu.h"
#include "panthor_perf.h"

/**
 * PANTHOR_PERF_COUNTERS_PER_BLOCK - On CSF architectures pre-11.x, the number of counters
 * per block was hardcoded to be 64. Arch 11.0 onwards supports the PRFCNT_FEATURES GPU register,
 * which indicates the same information.
 */
#define PANTHOR_PERF_COUNTERS_PER_BLOCK (64)

/**
 * PANTHOR_HW_COUNTER_SIZE - The size of a hardware counter in the FW ring buffer.
 */
#define PANTHOR_HW_COUNTER_SIZE (sizeof(u32))

#define MAX_SESSIONS_PER_FILE 1

/**
 * PANTHOR_PERF_EM_BITS - Number of bits in a user-facing enable mask. This must correspond
 *                        to the maximum number of counters available for selection on the newest
 *                        Mali GPUs (128 as of the Mali-Gx15).
 */
#define PANTHOR_PERF_EM_BITS (BITS_PER_TYPE(u64) * 2)

/**
 * PANTHOR_CTR_TIMESTAMP_LO - The first architecturally mandated counter of every block type
 *                            contains the low 32-bits of the TIMESTAMP value.
 */
#define PANTHOR_CTR_TIMESTAMP_LO (0)

/**
 * PANTHOR_CTR_TIMESTAMP_HI - The register offset containinig the high 32-bits of the TIMESTAMP
 *                            value.
 */
#define PANTHOR_CTR_TIMESTAMP_HI (1)

/**
 * PANTHOR_CTR_PRFCNT_EN - The register offset containing the enable mask for the enabled counters
 *                         that were written to memory.
 */
#define PANTHOR_CTR_PRFCNT_EN (2)

/**
 * PANTHOR_HEADER_COUNTERS - The first four counters of every block type are architecturally
 *                           defined to be equivalent. The fourth counter is always reserved,
 *                           and should be zero and as such, does not have a separate define.
 *
 *                           These are the only four counters that are the same between different
 *                           blocks and are consistent between different architectures.
 */
#define PANTHOR_HEADER_COUNTERS (4)

/**
 * PANTHOR_CTR_SAMPLE_REASON - The metadata block has a single value in position three which
 *                             indicates the reason a sample was taken.
 */
#define PANTHOR_CTR_SAMPLE_REASON (3)

/**
 * PANTHOR_PERF_RINGBUF_SLOTS_MIN - The minimum permitted number of slots in the Panthor perf
 *                                  ring buffer.
 */
#define PANTHOR_PERF_RINGBUF_SLOTS_MIN (16)

/**
 * PANTHOR_PERF_RINGBUF_SLOTS_MAX - The maximum permitted number of slots in the Panthor perf
 *                                  ring buffer.
 */
#define PANTHOR_PERF_RINGBUF_SLOTS_MAX (256)

static unsigned int perf_ringbuf_slots = 32;

static int perf_ringbuf_slots_set(const char *val, const struct kernel_param *kp)
{
	unsigned int slots;
	int ret = kstrtouint(val, 0, &slots);

	if (ret)
		return ret;

	if (!is_power_of_2(slots))
		return -EINVAL;

	return param_set_uint_minmax(val, kp, 16, 128);
}

static const struct kernel_param_ops perf_ringbuf_ops = {
	.set = perf_ringbuf_slots_set,
	.get = param_get_uint,
};
module_param_cb(perf_ringbuf_slots, &perf_ringbuf_ops, &perf_ringbuf_slots, 0400);
MODULE_PARM_DESC(perf_ringbuf_slots,
		 "Power of two slots allocated for the Panthor perf kernel-FW ringbuffer");

enum panthor_perf_session_state {
	/** @PANTHOR_PERF_SESSION_STOPPED: The session cannot sample. It starts out stopped. */
	PANTHOR_PERF_SESSION_STOPPED = 0,

	/** @PANTHOR_PERF_SESSION_STARTING: The session has not yet been started and is in the
	 *				    process of handling the clearing sample.
	 */
	PANTHOR_PERF_SESSION_STARTING,

	/** @PANTHOR_PERF_SESSION_ACTIVE: The session is active and can be used for sampling. */
	PANTHOR_PERF_SESSION_ACTIVE,

	/** @PANTHOR_PERF_SESSION_STOPPING: The session is currently stopping. After emitting the
	 *				    next sample, it will transition to inactive.
	 */
	PANTHOR_PERF_SESSION_STOPPING,
};

struct panthor_perf_enable_masks {
	/**
	 * @mask: Array of bitmasks indicating the counters userspace requested, where
	 *        one bit represents a single counter. Used to build the firmware configuration
	 *        and ensure that userspace clients obtain only the counters they requested.
	 */
	unsigned long mask[DRM_PANTHOR_PERF_BLOCK_MAX][BITS_TO_LONGS(PANTHOR_PERF_EM_BITS)];
};

struct panthor_perf_counter_block {
	struct drm_panthor_perf_block_header header;
	u64 counters[];
};

struct panthor_perf_fw_block {
	/** @block_count: Number of blocks of the given @type in the FW ringbuffer. */
	size_t block_count;

	/** @phys_mask: Bitmask of the physically available blocks. */
	u64 phys_mask;
};

struct panthor_perf_bo {
	struct drm_gem_object *obj;
	void *vaddr;
};

struct panthor_perf_sample_block_iter {
	const u8 *src;
	u8 *dst;
	size_t offset;
	size_t data_size;
	size_t block_size;
	const struct panthor_perf_counter_block *src_blk;
	struct panthor_perf_counter_block *dst_blk;
};

struct panthor_perf_session {
	/** @state_wq: Wait queue for state transitions driven by the sampling path. */
	wait_queue_head_t state_wq;

	/** @state_lock: Used to protect session state information and overflow bit. */
	struct mutex state_lock;

	/** @state: The current state of the session. */
	enum panthor_perf_session_state state;

	/** @overflow: The session is affected by a FW ring buffer overflow. */
	bool overflow;

	/**  @insert_idx: The insert index in the user-visible buffer for the current session. */
	u64 insert_idx;

	/**
	 * @accum_idx: Insert index of the populated slot. When this matches @insert_idx,
	 *             accumulate into that slot; otherwise populate a new slot.
	 */
	u64 accum_idx;

	/** @pending_sample: Whether this session has a sample requested on it. */
	bool pending_sample;

	/** @sample_flags: Accumulated @drm_panthor_perf_sample_flags for the pending sample. */
	u32 sample_flags;

	/** @sample_start_ns: Sample request time, obtained from a monotonic raw clock. */
	u64 sample_start_ns;

	/**
	 * @user_data: Opaque handle passed in when starting a session, requesting a sample (for
	 *             manual sampling sessions only) and when stopping a session. This handle
	 *             allows the disambiguation of a sample in the ringbuffer.
	 */
	u64 user_data;

	/**
	 * @eventfd: Event file descriptor context used to signal userspace of a new sample
	 *           being emitted.
	 */
	struct eventfd_ctx *eventfd;

	/**
	 * @enabled_counters: This session's requested counters. Note that these cannot change
	 *                    for the lifetime of the session.
	 */
	struct panthor_perf_enable_masks *enabled_counters;

	/** @ringbuf_slots: Slots in the user-facing ringbuffer. */
	size_t ringbuf_slots;

	/** @ring_buf: BO and mapping for the userspace ringbuffer. */
	struct panthor_perf_bo ring_buf;

	/**
	 * @control_buf: BO and mapping for the insert and extract indices.
	 */
	struct panthor_perf_bo control_buf;

	/** @control: The mapped insert and extract indices. */
	struct drm_panthor_perf_ringbuf_control *control;

	/** @samples: The mapping of the @ring_buf into the kernel's VA space. */
	u8 *samples;

	/**
	 * @sessions: The list node tracking all registered sessions.
	 */
	struct list_head sessions;

	/**
	 * @ref: Session reference count. The sample delivery to userspace is asynchronous, meaning
	 *       the lifetime of the session must extend at least until the sample is exposed to
	 *       userspace.
	 */
	struct kref ref;
};

struct panthor_perf {
	/** @info: Pointer to the previously initialized perf info. */
	struct drm_panthor_perf_info *info;

	/** @rb: Kernel BO in the FW AS containing the sample ringbuffer. */
	struct panthor_kernel_bo *rb;

	/** @sample_slots: Number of sample slots in the FW ringbuffer. */
	size_t sample_slots;

	/** @em: Combined enable mask for all of the active sessions. */
	struct panthor_perf_enable_masks *em;

	/**
	 * @fw_block_size: The size of a single block in the FW ring buffer, equal to
	 *                 sizeof(u32) * counters_per_block.
	 */
	size_t fw_block_size;

	/**
	 * @fw_sample_size: The size of a single FW sample in the FW ringbuffer.
	 */
	size_t fw_sample_size;

	/**
	 * @blocks: FW block layout for one sample. Metadata blocks are consumed
	 *          internally and omitted from userspace samples.
	 */
	struct panthor_perf_fw_block blocks[DRM_PANTHOR_PERF_BLOCK_MAX];

	/**
	 * @sample: Pointer to an upscaled and annotated sample that may be emitted to userspace.
	 *          This is used both as an intermediate buffer to do the zero-extension of the
	 *          32-bit counters to 64-bits and as a storage buffer in case of automatic samples.
	 */
	u8 *sample;

	/**
	 * @op_lock: Serializes session operations and global FW configuration. Acquire
	 *           before looking up session handles and before @session_lock or a
	 *           session's state_lock. The sample worker must not acquire this lock,
	 *           so synchronous session operations can wait for sample delivery.
	 *           Runtime-PM references must be acquired and released outside this
	 *           lock, as the PM callbacks also acquire it.
	 */
	struct mutex op_lock;

	/**
	 * @session_lock: Lock used to guard the @session_list.
	 */
	struct mutex session_lock;

	/** @session_list: List of all sessions. */
	struct list_head session_list;

	/** @pend_lock: Lock used to guard the requested sample field. */
	spinlock_t pend_lock;


	/** @fw_sample_requested: A sample has been requested. */
	bool fw_sample_requested;

	/** @set_config: The set that will be configured onto the hardware. */
	u8 set_config;

	/** @last_fw_sample_time: The CPU-side timestamp of the latest consumed FW sample. */
	u64 last_fw_sample_time;

	/** @sample_workq: Workqueue to handle and accumulate samples. */
	struct workqueue_struct *sample_workq;

	/** @sample_work: Worker to consume available FW samples and expose them to the user. */
	struct work_struct sample_work;

	/**
	 * @ptdev: Backpointer to the Panthor device, needed to ring the global doorbell and
	 *         interface with FW.
	 */
	struct panthor_device *ptdev;
};

struct panthor_perf_fw_block_iter {
	const struct panthor_perf *perf_ctx;
	enum drm_panthor_perf_block_type type;
	u32 idx;
	size_t offset;
};

struct panthor_perf_pool {
	/** @session_range: The number of sessions supported at a time. */
	struct xa_limit session_range;

	/** @xa: Xarray used to manage session handles. */
	struct xarray xa;

	/** @next: Next handle to allocate. */
	u32 next;
};

static size_t get_annotated_block_size(size_t counters_per_block)
{
	return struct_size_t(struct panthor_perf_counter_block, counters, counters_per_block);
}

static void
sample_block_iter_init(const struct drm_panthor_perf_info *info,
		       const u8 *src, u8 *dst,
		       struct panthor_perf_sample_block_iter *iter)
{
	const size_t sample_header_size = info->sample_header_size;

	*iter = (struct panthor_perf_sample_block_iter) {
		.src = src + sample_header_size,
		.dst = dst + sample_header_size,
		.data_size = info->sample_size - sample_header_size,
		.block_size = get_annotated_block_size(info->counters_per_block),
	};
}

static bool sample_block_iter_next(struct panthor_perf_sample_block_iter *iter)
{
	if (iter->offset >= iter->data_size)
		return false;

	iter->src_blk = (const struct panthor_perf_counter_block *)(iter->src + iter->offset);
	iter->dst_blk = (struct panthor_perf_counter_block *)(iter->dst + iter->offset);
	iter->offset += iter->block_size;

	return true;
}

static size_t session_get_sample_size(const struct drm_panthor_perf_info *const info)
{
	const size_t block_size = get_annotated_block_size(info->counters_per_block);
	const size_t block_nr = info->cshw_blocks + info->fw_blocks +
		info->tiler_blocks + info->memsys_blocks + info->shader_blocks;

	return info->sample_header_size + (block_size * block_nr);
}

static void panthor_perf_info_init(struct panthor_device *const ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 11) {
		perf_info->counters_per_block = PANTHOR_PERF_COUNTERS_PER_BLOCK;
	} else {
		u32 features = panthor_gpu_get_prfcnt_features(ptdev);

		perf_info->counters_per_block = PRFCNT_FEATURES_COUNTER_BLOCK_SIZE(features) /
			PANTHOR_HW_COUNTER_SIZE;
	}

	perf_info->sample_header_size = sizeof(struct drm_panthor_perf_sample_header);
	perf_info->block_header_size = sizeof(struct drm_panthor_perf_block_header);

	if (GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size))
		perf_info->fw_blocks = 1;

	perf_info->cshw_blocks = 1;
	perf_info->tiler_blocks = 1;
	perf_info->memsys_blocks = GPU_MEM_FEATURES_L2_SLICES(ptdev->gpu_info.mem_features);
	perf_info->shader_blocks = hweight64(ptdev->gpu_info.shader_present);

	perf_info->sample_size = session_get_sample_size(perf_info);
}

static struct panthor_perf_enable_masks *
panthor_perf_create_em(struct drm_panthor_perf_session_create *const create_args)
{
	struct panthor_perf_enable_masks *const em = kmalloc(sizeof(*em), GFP_KERNEL);

	if (IS_ERR_OR_NULL(em))
		return em;

	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_FW],
			  create_args->fw_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_CSHW],
			  create_args->cshw_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_TILER],
			  create_args->tiler_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_MEMSYS],
			  create_args->memsys_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_SHADER],
			  create_args->shader_enable_mask, PANTHOR_PERF_EM_BITS);

	return em;
}

static struct panthor_perf_session *
session_get(struct panthor_perf_session *session)
{
	if (session)
		kref_get(&session->ref);

	return session;
}

static void panthor_perf_em_add(struct panthor_perf_enable_masks *const dst_em,
				const struct panthor_perf_enable_masks *const src_em)
{
	size_t i = 0;

	for (i = DRM_PANTHOR_PERF_BLOCK_FIRST; i <= DRM_PANTHOR_PERF_BLOCK_LAST; i++)
		bitmap_or(dst_em->mask[i], dst_em->mask[i], src_em->mask[i], PANTHOR_PERF_EM_BITS);
}

static u64 session_read_extract_idx(struct panthor_perf_session *session)
{
	/* Userspace will update their own extract index to indicate that a sample is consumed
	 * from the ringbuffer, and we must ensure we read the latest value.
	 */
	return smp_load_acquire(&session->control->extract_idx);
}

static void session_write_insert_idx(struct panthor_perf_session *session, u64 idx)
{
	/* Userspace needs the insert index to know where to look for the sample. */
	smp_store_release(&session->control->insert_idx, idx);
	session->insert_idx = idx;
}

static int
panthor_perf_bo_get(struct drm_file *file, u32 handle, size_t expected_size,
		    struct panthor_perf_bo *bo)
{
	struct drm_device *ddev = file->minor->dev;
	struct drm_gem_object *obj;
	struct iosys_map map;
	int ret;

	if (WARN_ON_ONCE(!bo))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, handle);
	if (!obj) {
		drm_err(ddev, "Could not find handle %d!\n", handle);
		return -EINVAL;
	}

	if (expected_size && obj->size != PFN_ALIGN(expected_size)) {
		drm_err(ddev,
			"Incorrect BO size from userspace: user %zu vs kernel %lu\n",
			obj->size, PFN_ALIGN(expected_size));
		ret = -ENOMEM;
		goto err_put;
	}

	ret = drm_gem_vmap(obj, &map);
	if (ret)
		goto err_put;

	bo->obj = obj;
	bo->vaddr = map.vaddr;

	return 0;

err_put:
	drm_gem_object_put(obj);
	return ret;
}

static void panthor_perf_bo_put(struct panthor_perf_bo *bo)
{
	if (!bo->obj)
		return;

	if (bo->vaddr) {
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(bo->vaddr);

		drm_gem_vunmap(bo->obj, &map);
	}

	drm_gem_object_put(bo->obj);

	memset(bo, 0, sizeof(*bo));
}

static void session_free(struct kref *ref)
{
	struct panthor_perf_session *session = container_of(ref, typeof(*session), ref);

	panthor_perf_bo_put(&session->ring_buf);
	panthor_perf_bo_put(&session->control_buf);

	kfree(session->enabled_counters);

	eventfd_ctx_put(session->eventfd);

	kfree(session);
}

static void session_put(struct panthor_perf_session *session)
{
	kref_put(&session->ref, session_free);
}

/**
 * session_from_handle - Find a session associated with the pool and session_handle.
 *
 * @pool: Per-file pool.
 * @handle: Session handle.
 *
 * The reference count of a valid session is increased to ensure it does not disappear
 * in the window between the XA lock being dropped and the internal session functions
 * being called.
 *
 * Return: valid session pointer or NULL.
 */
static struct panthor_perf_session *
session_from_handle(struct panthor_perf_pool *pool, unsigned long handle)
{
	struct panthor_perf_session *session;

	xa_lock(&pool->xa);
	session = session_get(xa_load(&pool->xa, handle));
	xa_unlock(&pool->xa);

	return session;
}

static u32 compress_enable_mask(u32 counters, const unsigned long *const src)
{
	size_t i;
	u32 result = 0;
	const unsigned long ctr_per_bit = counters / BITS_PER_TYPE(u32);

	for_each_set_bit(i, src, counters) {
		const unsigned long pos = div_u64(i, ctr_per_bit);

		result |= BIT(pos);
	}

	return result;
}

static void expand_enable_mask(u32 counters, u32 em, unsigned long *const dst)
{
	size_t i;
	const unsigned long ctrs_per_bit = counters / BITS_PER_TYPE(u32);
	DECLARE_BITMAP(emb, BITS_PER_TYPE(u32));

	bitmap_from_arr32(emb, &em, BITS_PER_TYPE(u32));

	for_each_set_bit(i, emb, BITS_PER_TYPE(u32))
		bitmap_set(dst, i * ctrs_per_bit, ctrs_per_bit);
}

static inline bool
panthor_perf_fw_block_iter_valid(struct panthor_perf_fw_block_iter *iter)
{
	const struct panthor_perf *perf = iter->perf_ctx;

	while (iter->type < DRM_PANTHOR_PERF_BLOCK_MAX) {
		if (iter->idx < perf->blocks[iter->type].block_count)
			return true;

		iter->type++;
		iter->idx = 0;
	}

	return false;
}

static void panthor_perf_handle_fw_samples(struct panthor_device *ptdev,
					   u32 extract_idx, u32 insert_idx)
{
	struct panthor_perf *perf = ptdev->perf;
	const size_t ann_block_size =
		get_annotated_block_size(ptdev->perf_info.counters_per_block);
	u32 i;

	for (i = extract_idx; i != insert_idx; i++) {
		u32 slot = i % perf->sample_slots;
		u8 *fw_sample = (u8 *)perf->rb->kmap + slot * perf->fw_sample_size;
		struct panthor_perf_fw_block_iter fw_iter = { .perf_ctx = perf };
		size_t ann_off = ptdev->perf_info.sample_header_size;

		for (; panthor_perf_fw_block_iter_valid(&fw_iter);
		     fw_iter.offset += fw_iter.perf_ctx->fw_block_size, fw_iter.idx++) {
			struct panthor_perf_counter_block *blk =
				(typeof(blk))(perf->sample + ann_off);
			u32 *const block = (u32 *)(fw_sample + fw_iter.offset);
			const u32 prfcnt_en = block[PANTHOR_CTR_PRFCNT_EN];

			if (fw_iter.type == DRM_PANTHOR_PERF_BLOCK_METADATA) {
				/*
				 * The host must clear the SAMPLE_REASON to acknowledge it has
				 * consumed the sample.
				 */
				block[PANTHOR_CTR_SAMPLE_REASON] = 0;
				continue;
			}

			if (!(perf->blocks[fw_iter.type].phys_mask & BIT_ULL(fw_iter.idx)))
				continue;

			blk->header = (struct drm_panthor_perf_block_header) {
				.block_idx = fw_iter.idx,
				.block_type = fw_iter.type,
			};

			if (prfcnt_en == 0)
				goto advance_block;

			/*
			 * The four header counters must be treated differently, because they are
			 * not additive. For the fourth, the assignment does not matter, as it
			 * is reserved and should be zero.
			 */
			blk->counters[PANTHOR_CTR_TIMESTAMP_LO] = block[PANTHOR_CTR_TIMESTAMP_LO];
			blk->counters[PANTHOR_CTR_TIMESTAMP_HI] = block[PANTHOR_CTR_TIMESTAMP_HI];
			blk->counters[PANTHOR_CTR_PRFCNT_EN] = block[PANTHOR_CTR_PRFCNT_EN];

			/*
			 * The host must clear PRFCNT_EN to acknowledge it has consumed the sample.
			 */
			block[PANTHOR_CTR_PRFCNT_EN] = 0;

			if (prfcnt_en == 1)
				goto advance_block;

			for (size_t k = PANTHOR_HEADER_COUNTERS;
			     k < ptdev->perf_info.counters_per_block; k++)
				blk->counters[k] += block[k];

advance_block:
			ann_off += ann_block_size;
		}

		perf->last_fw_sample_time = ktime_get_raw_ns();
	}
}

static size_t panthor_perf_get_fw_reported_size(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);

	size_t fw_size = GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size);
	size_t hw_size = GLB_PERFCNT_HW_SIZE(glb_iface->control->perfcnt_size);
	size_t md_size = PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features);

	return md_size + fw_size + hw_size;
}

static size_t get_reserved_sc_blocks(struct panthor_device *ptdev)
{
	const u64 sc_mask = ptdev->gpu_info.shader_present;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 14)
		return fls64(sc_mask);

	return ptdev->perf_info.shader_blocks;
}

#define BLK_MASK(x) GENMASK_ULL((x) - 1, 0)

static u64 get_sc_mask(struct panthor_device *ptdev)
{
	const u64 sc_mask = ptdev->gpu_info.shader_present;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 14)
		return sc_mask;

	return BLK_MASK(hweight64(sc_mask));
}

static int panthor_perf_setup_fw_buffer(struct panthor_device *ptdev, struct panthor_perf *perf)
{
	struct drm_panthor_perf_info *info = &ptdev->perf_info;
	const size_t block_size = info->counters_per_block * PANTHOR_HW_COUNTER_SIZE;
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	const size_t reported_sample_size = panthor_perf_get_fw_reported_size(ptdev);
	const bool has_metadata = PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features);
	const struct panthor_perf_fw_block blocks[DRM_PANTHOR_PERF_BLOCK_MAX] = {
		[DRM_PANTHOR_PERF_BLOCK_METADATA] = {
			.block_count = has_metadata ? 1 : 0,
			.phys_mask = has_metadata ? BIT_ULL(0) : 0,
		},
		[DRM_PANTHOR_PERF_BLOCK_FW] = {
			.block_count = info->fw_blocks,
			.phys_mask = info->fw_blocks ? BLK_MASK(info->fw_blocks) : 0,
		},
		[DRM_PANTHOR_PERF_BLOCK_CSHW] = {
			.block_count = info->cshw_blocks,
			.phys_mask = BLK_MASK(info->cshw_blocks),
		},
		[DRM_PANTHOR_PERF_BLOCK_TILER] = {
			.block_count = info->tiler_blocks,
			.phys_mask = BLK_MASK(info->tiler_blocks),
		},
		[DRM_PANTHOR_PERF_BLOCK_MEMSYS] = {
			.block_count = info->memsys_blocks,
			.phys_mask = BLK_MASK(info->memsys_blocks),
		},
		[DRM_PANTHOR_PERF_BLOCK_SHADER] = {
			.block_count = get_reserved_sc_blocks(ptdev),
			.phys_mask = get_sc_mask(ptdev),
		},
	};
	size_t offset = 0;

	for (enum drm_panthor_perf_block_type type = 0; type < DRM_PANTHOR_PERF_BLOCK_MAX; type++)
		offset += blocks[type].block_count * block_size;

	if (offset != reported_sample_size) {
		drm_warn(&ptdev->base,
				"Could not align fw and kernel perf buffers, feature disabled.");
		return -EINVAL;
	}

	memcpy(perf->blocks, blocks, sizeof(blocks));
	perf->fw_block_size = block_size;
	perf->fw_sample_size = offset;

	return 0;
}

static int panthor_perf_fw_stop_sampling(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	u32 acked;
	int ret;

	if (~READ_ONCE(glb_iface->input->req) & GLB_PERFCNT_ENABLE)
		return 0;

	panthor_fw_update_reqs(glb_iface, req, 0, GLB_PERFCNT_ENABLE);
	panthor_fw_ring_doorbell(ptdev, CSF_GLB_DOORBELL_ID);
	ret = panthor_fw_glb_wait_acks(ptdev, GLB_PERFCNT_ENABLE, &acked, 100);
	if (ret)
		drm_warn(&ptdev->base, "Could not disable performance counters");

	return ret;
}

static int panthor_perf_fw_start_sampling(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	u32 acked;
	int ret;

	if (READ_ONCE(glb_iface->input->req) & GLB_PERFCNT_ENABLE)
		return 0;

	/*
	 * The spec mandates that the host zero the PRFCNT_EXTRACT register before an enable
	 * operation, and each (re-)enable will require an enable-disable pair to program
	 * the new changes onto the FW interface.
	 */
	WRITE_ONCE(glb_iface->input->perfcnt_extract, 0);

	panthor_fw_update_reqs(glb_iface, req, GLB_PERFCNT_ENABLE, GLB_PERFCNT_ENABLE);
	panthor_fw_ring_doorbell(ptdev, CSF_GLB_DOORBELL_ID);
	ret = panthor_fw_glb_wait_acks(ptdev, GLB_PERFCNT_ENABLE, &acked, 100);
	if (ret)
		drm_warn(&ptdev->base, "Could not enable performance counters");

	return ret;
}

static void panthor_perf_fw_write_config(struct panthor_perf *perf)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(perf->ptdev);
	const u32 counters = perf->info->counters_per_block;
	const struct panthor_perf_enable_masks *const em = perf->em;
	u32 perfcnt_config;

	glb_iface->input->perfcnt_csf_enable =
		compress_enable_mask(counters, em->mask[DRM_PANTHOR_PERF_BLOCK_CSHW]);
	glb_iface->input->perfcnt_shader_enable =
		compress_enable_mask(counters, em->mask[DRM_PANTHOR_PERF_BLOCK_SHADER]);
	glb_iface->input->perfcnt_mmu_l2_enable =
		compress_enable_mask(counters, em->mask[DRM_PANTHOR_PERF_BLOCK_MEMSYS]);
	glb_iface->input->perfcnt_tiler_enable =
		compress_enable_mask(counters, em->mask[DRM_PANTHOR_PERF_BLOCK_TILER]);
	glb_iface->input->perfcnt_fw_enable =
		compress_enable_mask(counters, em->mask[DRM_PANTHOR_PERF_BLOCK_FW]);
	glb_iface->input->perfcnt_csg_enable = 0;

	WRITE_ONCE(glb_iface->input->perfcnt_as, panthor_vm_as(panthor_fw_vm(perf->ptdev)));
	WRITE_ONCE(glb_iface->input->perfcnt_base, panthor_kernel_bo_gpuva(perf->rb));

	perfcnt_config = GLB_PERFCNT_CONFIG_SIZE(perf_ringbuf_slots);
	perfcnt_config |= GLB_PERFCNT_CONFIG_SET(perf->set_config);

	if (PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features))
		perfcnt_config |= GLB_PERFCNT_METADATA_ENABLE;

	WRITE_ONCE(glb_iface->input->perfcnt_config, perfcnt_config);
}

#define DEFINE_SESSION_STATE_CHECK(name, _state)			     \
	static bool session_is_## name(struct panthor_perf_session *session) \
	{								     \
		lockdep_assert_held(&session->state_lock);		     \
									     \
		return session->state == (_state);			    \
	}

DEFINE_SESSION_STATE_CHECK(active, PANTHOR_PERF_SESSION_ACTIVE);
DEFINE_SESSION_STATE_CHECK(starting, PANTHOR_PERF_SESSION_STARTING);
DEFINE_SESSION_STATE_CHECK(stopping, PANTHOR_PERF_SESSION_STOPPING);
DEFINE_SESSION_STATE_CHECK(stopped, PANTHOR_PERF_SESSION_STOPPED);

static bool session_has_overflow(struct panthor_perf_session *session)
{
	lockdep_assert_held(&session->state_lock);

	return session->overflow;
}

static void session_set_state(struct panthor_perf_session *session,
			      enum panthor_perf_session_state state)
{
	lockdep_assert_held(&session->state_lock);

	session->state = state;
}

static bool session_has_space(struct panthor_perf_session *session, u32 needed)
{
	const u64 insert = session->insert_idx;
	const u64 extract = session_read_extract_idx(session);

	return insert - extract + needed <= session->ringbuf_slots;
}

/**
 * session_accumulate_sample - Accumulate requested counters into an existing sample.
 * @ptdev: Panthor device.
 * @session: Perf session.
 * @session_sample: Start of the destination sample, including its header.
 * @perf_sample: Start of the intermediate sample, including its header.
 *
 * Firmware enable masks select groups of counters. Copy timestamps and accumulate
 * only the counters requested by this session.
 */
static void session_accumulate_sample(struct panthor_device *ptdev,
				      struct panthor_perf_session *session,
				      u8 *session_sample, u8 *perf_sample)
{
	const struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;
	struct panthor_perf_enable_masks *em = session->enabled_counters;
	struct panthor_perf_sample_block_iter iter;
	const u32 counters = perf_info->counters_per_block;

	sample_block_iter_init(perf_info, perf_sample, session_sample, &iter);

	while (sample_block_iter_next(&iter)) {
		DECLARE_BITMAP(enabled_ctrs, PANTHOR_PERF_EM_BITS);
		const struct panthor_perf_counter_block *src_blk = iter.src_blk;
		struct panthor_perf_counter_block *dst_blk = iter.dst_blk;

		bitmap_from_arr64(enabled_ctrs, em->mask[dst_blk->header.block_type],
				  PANTHOR_PERF_EM_BITS);
		bitmap_clear(enabled_ctrs, 0, PANTHOR_HEADER_COUNTERS);

		dst_blk->counters[PANTHOR_CTR_TIMESTAMP_HI] =
			src_blk->counters[PANTHOR_CTR_TIMESTAMP_HI];
		dst_blk->counters[PANTHOR_CTR_TIMESTAMP_LO] =
			src_blk->counters[PANTHOR_CTR_TIMESTAMP_LO];

		for (size_t ctr_idx = PANTHOR_HEADER_COUNTERS; ctr_idx < counters; ctr_idx++)
			if (test_bit(ctr_idx, enabled_ctrs))
				dst_blk->counters[ctr_idx] += src_blk->counters[ctr_idx];
	}
}

static void perf_fw_request_sample(struct panthor_perf *perf)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(perf->ptdev);
	bool sample;

	scoped_guard(spinlock_irqsave, &perf->pend_lock) {
		sample = !perf->fw_sample_requested;

		if (sample)
			perf->fw_sample_requested = true;
	}

	if (sample) {
		panthor_fw_toggle_reqs(glb_iface, req, ack, GLB_PERFCNT_SAMPLE);
		panthor_fw_ring_doorbell(perf->ptdev, CSF_GLB_DOORBELL_ID);
	}
}

/**
 * session_populate_sample - Populate a new userspace sample slot.
 * @ptdev: Panthor device.
 * @session: Perf session.
 * @session_sample: Start of the destination sample, including its header.
 * @perf_sample: Start of the intermediate sample, including its header.
 *
 * Copy the counter blocks and clear counters not requested by this session.
 * Update each block's PRFCNT_EN value to reflect this session's enable mask.
 * The sample header is filled when the sample is published.
 */
static void session_populate_sample(struct panthor_device *ptdev,
				    struct panthor_perf_session *session, u8 *session_sample,
				    u8 *perf_sample)
{
	const struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;
	struct panthor_perf_sample_block_iter iter;
	const size_t sample_header_size = perf_info->sample_header_size;
	const size_t data_size = perf_info->sample_size - sample_header_size;
	const u32 counters = perf_info->counters_per_block;

	memcpy(session_sample + sample_header_size, perf_sample + sample_header_size, data_size);

	sample_block_iter_init(perf_info, perf_sample, session_sample, &iter);

	while (sample_block_iter_next(&iter)) {
		DECLARE_BITMAP(em_diff, PANTHOR_PERF_EM_BITS);
		DECLARE_BITMAP(enabled_ctrs, PANTHOR_PERF_EM_BITS);
		struct panthor_perf_counter_block *blk = iter.dst_blk;
		enum drm_panthor_perf_block_type type = blk->header.block_type;
		unsigned long *blk_em = session->enabled_counters->mask[type];
		const u32 blk_prfcnt_en = blk->counters[PANTHOR_CTR_PRFCNT_EN];

		bitmap_zero(enabled_ctrs, PANTHOR_PERF_EM_BITS);
		expand_enable_mask(counters, blk_prfcnt_en, enabled_ctrs);
		bitmap_andnot(em_diff, enabled_ctrs, blk_em, PANTHOR_PERF_EM_BITS);
		bitmap_clear(em_diff, 0, PANTHOR_HEADER_COUNTERS);

		blk->counters[PANTHOR_CTR_PRFCNT_EN] = compress_enable_mask(counters, blk_em);

		for (size_t ctr_idx = PANTHOR_HEADER_COUNTERS; ctr_idx < counters; ctr_idx++) {
			if (test_bit(ctr_idx, em_diff))
				blk->counters[ctr_idx] = 0;
		}
	}
}

static void session_copy_sample(struct panthor_device *ptdev,
				struct panthor_perf_session *session, u32 flags)
{
	struct panthor_perf *perf = ptdev->perf;
	const size_t sample_size = ptdev->perf_info.sample_size;
	const u64 insert_idx = session->insert_idx;
	u8 *new_sample;

	scoped_guard(mutex, &session->state_lock) {
		if (flags & DRM_PANTHOR_PERF_SAMPLE_OVERFLOW)
			session->overflow = true;

		if (session_is_starting(session) ||
		    (!session_is_active(session) && !session_is_stopping(session)))
			return;
	}

	new_sample = session->samples + (insert_idx % session->ringbuf_slots) * sample_size;

	if (session->accum_idx != insert_idx) {
		session->sample_flags = flags;
		session_populate_sample(ptdev, session, new_sample, perf->sample);
		session->accum_idx = insert_idx;
	} else {
		session->sample_flags |= flags;
		session_accumulate_sample(ptdev, session, new_sample, perf->sample);
	}
}

static void session_publish_sample(struct panthor_perf *perf,
				   struct panthor_perf_session *session)
{
	const struct drm_panthor_perf_info *const info = &perf->ptdev->perf_info;
	u8 *sample = session->samples +
		(session->insert_idx % session->ringbuf_slots) * info->sample_size;
	struct drm_panthor_perf_sample_header *hdr = (typeof(hdr))sample;

	*hdr = (struct drm_panthor_perf_sample_header) {
		.user_data = session->user_data,
		.timestamp_start_ns = session->sample_start_ns,
		.timestamp_end_ns = perf->last_fw_sample_time,
		.flags = session->sample_flags,
	};

	session_write_insert_idx(session, session->insert_idx + 1);

	/* Order sample and insert-index writes before the eventfd notification. */
	wmb();

	eventfd_signal(session->eventfd);
}

static void session_emit_sample(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	guard(mutex)(&session->state_lock);

	if (session_is_starting(session)) {
		session_set_state(session, PANTHOR_PERF_SESSION_ACTIVE);
		goto update_session;
	}

	if (session_is_stopping(session)) {
		session_publish_sample(perf, session);
		goto handle_stopping;
	}

	if (!session_is_active(session))
		return;

	if (session->pending_sample) {
		session->pending_sample = false;
		session_publish_sample(perf, session);
		goto update_session;
	}

	return;

handle_stopping:
	if (session_is_stopping(session))
		session_set_state(session, PANTHOR_PERF_SESSION_STOPPED);

update_session:
	session->sample_flags = 0;
	session->sample_start_ns = perf->last_fw_sample_time;
	wake_up_all(&session->state_wq);
}

static void panthor_perf_sample_worker(struct work_struct *work)
{
	struct panthor_perf *perf = container_of(work, struct panthor_perf, sample_work);
	struct panthor_device *const ptdev = perf->ptdev;
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	const u32 extract_idx = READ_ONCE(glb_iface->input->perfcnt_extract);
	const u32 insert_idx = READ_ONCE(glb_iface->output->perfcnt_insert);
	const u32 ack = READ_ONCE(glb_iface->output->ack);
	const u32 req = READ_ONCE(glb_iface->input->req);
	const u32 events = (req ^ ack) & (GLB_PERFCNT_THRESHOLD | GLB_PERFCNT_OVERFLOW);
	const u32 flags = events & GLB_PERFCNT_OVERFLOW ? DRM_PANTHOR_PERF_SAMPLE_OVERFLOW : 0;

	/* Defer sample processing until the pending reset completes. */
	if (panthor_device_reset_is_pending(ptdev)) {
		queue_work(perf->sample_workq, &perf->sample_work);
		return;
	}

	/* A stale notification must not complete a userspace sample request. */
	if (extract_idx == insert_idx)
		goto ack_events;

	panthor_perf_handle_fw_samples(ptdev, extract_idx, insert_idx);
	WRITE_ONCE(glb_iface->input->perfcnt_extract, insert_idx);

	scoped_guard(mutex, &perf->session_lock) {
		struct panthor_perf_session *session;

		list_for_each_entry(session, &perf->session_list, sessions)
			session_copy_sample(ptdev, session, flags);

		list_for_each_entry(session, &perf->session_list, sessions)
			session_emit_sample(perf, session);
	}

	memset(perf->sample, 0, ptdev->perf_info.sample_size);

ack_events:
	if (events)
		panthor_fw_update_reqs(glb_iface, req, ack, events);
}

void panthor_perf_report_irq(struct panthor_device *ptdev, u32 status)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct panthor_perf *const perf = ptdev->perf;

	if (!(status & JOB_INT_GLOBAL_IF))
		return;

	if (!perf)
		return;

	const u32 ack = READ_ONCE(glb_iface->output->ack);
	const u32 req = READ_ONCE(glb_iface->input->req);
	const u32 events = (req ^ ack) & (GLB_PERFCNT_THRESHOLD | GLB_PERFCNT_OVERFLOW);
	const bool sample_acked = ((req & GLB_PERFCNT_SAMPLE) == (ack & GLB_PERFCNT_SAMPLE));
	const u32 extract_idx = READ_ONCE(glb_iface->input->perfcnt_extract);
	const u32 insert_idx = READ_ONCE(glb_iface->output->perfcnt_insert);
	const bool has_sample = sample_acked && insert_idx != extract_idx;

	if (sample_acked) {
		guard(spinlock_irqsave)(&perf->pend_lock);
		perf->fw_sample_requested = false;
	}

	if (events || has_sample)
		queue_work(perf->sample_workq, &perf->sample_work);
}

static int perf_enable(struct panthor_perf *perf)
{
	lockdep_assert_held(&perf->op_lock);

	guard(mutex)(&perf->session_lock);

	if (list_empty(&perf->session_list))
		return 0;

	panthor_perf_fw_write_config(perf);
	return panthor_perf_fw_start_sampling(perf->ptdev);
}

static int perf_disable(struct panthor_perf *perf)
{
	lockdep_assert_held(&perf->op_lock);

	guard(mutex)(&perf->session_lock);

	if (list_empty(&perf->session_list))
		return 0;

	return panthor_perf_fw_stop_sampling(perf->ptdev);
}

static void compute_enable_mask(struct panthor_perf *perf)
{
	lockdep_assert_held(&perf->session_lock);

	struct list_head *snode;
	struct panthor_perf_enable_masks em = {};

	list_for_each(snode, &perf->session_list)
	{
		struct panthor_perf_session *session =
			container_of(snode, typeof(*session), sessions);

		panthor_perf_em_add(&em, session->enabled_counters);
	}

	*perf->em = em;
}

static void perf_attach_session(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	lockdep_assert_held(&perf->session_lock);

	session_get(session);
	list_add_tail(&session->sessions, &perf->session_list);
	compute_enable_mask(perf);
}

static void perf_detach_session(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	lockdep_assert_held(&perf->session_lock);

	list_del_init(&session->sessions);
	compute_enable_mask(perf);
	session_put(session);
}

static int panthor_perf_session_add(struct panthor_perf *perf,
				    struct panthor_perf_session *session, u8 set)
{
	int ret = 0;

	guard(mutex)(&perf->op_lock);

	scoped_guard(mutex, &perf->session_lock) {
		if (!list_empty(&perf->session_list))
			return -EBUSY;

		perf->set_config = set;
	}

	ret = perf_disable(perf);
	if (ret)
		return ret;

	scoped_guard(mutex, &perf->session_lock)
		perf_attach_session(perf, session);

	ret = perf_enable(perf);
	if (ret)
		goto drop_session_ref;

	return ret;
drop_session_ref:
	scoped_guard(mutex, &perf->session_lock)
		perf_detach_session(perf, session);

	return ret;
}

static int panthor_perf_session_remove(struct panthor_perf *perf,
				       struct panthor_perf_session *session)
{
	guard(mutex)(&perf->op_lock);

	/*
	 * Even if the disable fails,  the session must be removed, so the error
	 * is ignored.
	 */
	perf_disable(perf);

	scoped_guard(mutex, &perf->session_lock)
		perf_detach_session(perf, session);

	return perf_enable(perf);
}

/**
 * panthor_perf_init - Initialize the performance counter subsystem.
 * @ptdev: Panthor device
 *
 * The performance counters require the FW interface to be available to setup the
 * sampling ringbuffers, so this must be called only after FW is initialized.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_init(struct panthor_device *ptdev)
{
	struct panthor_perf *perf;
	struct panthor_kernel_bo *bo;
	struct panthor_perf_enable_masks *em;
	u8 *sample;
	int ret;

	if (!ptdev)
		return -EINVAL;

	panthor_perf_info_init(ptdev);

	perf = drmm_kzalloc(&ptdev->base, sizeof(*perf), GFP_KERNEL);
	if (!perf)
		return -ENOMEM;

	ret = drmm_mutex_init(&ptdev->base, &perf->op_lock);
	if (ret)
		return ret;

	ret = drmm_mutex_init(&ptdev->base, &perf->session_lock);
	if (ret)
		return ret;

	spin_lock_init(&perf->pend_lock);

	ret = panthor_perf_setup_fw_buffer(ptdev, perf);
	if (ret) {
		drm_err(&ptdev->base,
			"Failed to setup FW ring buffer, disabling perf.");
		return -EOPNOTSUPP;
	}

	sample = drmm_kzalloc(&ptdev->base, ptdev->perf_info.sample_size, GFP_KERNEL);
	if (!sample)
		return -ENOMEM;

	em = drmm_kzalloc(&ptdev->base, sizeof(*em), GFP_KERNEL);
	if (!em)
		return -ENOMEM;

	bo = panthor_kernel_bo_create(ptdev, panthor_fw_vm(ptdev),
				      perf->fw_sample_size * perf_ringbuf_slots,
				      DRM_PANTHOR_BO_NO_MMAP,
				      DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC |
				      DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED,
				      PANTHOR_VM_KERNEL_AUTO_VA,
				      "perf_ringbuf");

	if (IS_ERR_OR_NULL(bo))
		return IS_ERR(bo) ? PTR_ERR(bo) : -ENOMEM;

	ret = panthor_kernel_bo_vmap(bo);
	if (ret)
		goto cleanup_bo;

	perf->sample_workq = alloc_workqueue("panthor_perf_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!perf->sample_workq) {
		ret = -ENOMEM;
		goto cleanup_vmap;
	}

	ret = drmm_add_action_or_reset(&ptdev->base, __drmm_workqueue_release,
				       perf->sample_workq);
	if (ret)
		goto cleanup_vmap;

	INIT_WORK(&perf->sample_work, panthor_perf_sample_worker);
	perf->rb = bo;
	perf->sample = sample;
	perf->sample_slots = perf_ringbuf_slots;
	perf->em = em;
	perf->last_fw_sample_time = ktime_get_raw_ns();

	INIT_LIST_HEAD(&perf->session_list);

	drm_info(&ptdev->base, "Performance counter subsystem initialized");

	perf->info = &ptdev->perf_info;
	perf->ptdev = ptdev;
	ptdev->perf = perf;

	return ret;

cleanup_vmap:
	panthor_kernel_bo_vunmap(bo);

cleanup_bo:
	panthor_kernel_bo_destroy(bo);

	return ret;
}

typedef bool (*session_state_cb)(struct panthor_perf_session *session);

/**
 * panthor_perf_request_sync - Request synchronous sample causing
 * @perf: Perf struct
 * @session: Target session
 * @cb: Callback for the expected state of the session at the end of a sample.
 *
 * Perform a synchronous sample for scenarios where the sample must complete for the user to
 * unambiguously know the state of the session. The two cases where this may be used are:
 * - starting a new session (initial samples): any data from before this sample must not show up
 *   in a new sampling session.
 * - stopping an existing session: the eventfd may already have been signalled, meaning the
 *   user may not find out when the session is stopped and continue waiting for more samples.
 *
 * Return: 0 on success, -ETIMEDOUT on wait failure
 */
static int panthor_perf_request_sync(struct panthor_perf *perf,
				     struct panthor_perf_session *session, session_state_cb cb)
{
	int ret;

	perf_fw_request_sample(perf);

	ret = wait_event_timeout(session->state_wq, cb(session),
				 msecs_to_jiffies(1000));

	return ret ? 0 : -ETIMEDOUT;
}

/**
 * panthor_perf_session_create - Create a user-visible session.
 *
 * @file: DRM file associated with the request.
 * @create_args: Creation arguments passed in via ioctl.
 * @sid: Session identifier output.
 *
 * Creates a new session associated with the session ID returned. When initialized, the
 * session must explicitly request sampling to start with a successive call to PERF_SESSION_START.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_create(struct drm_file *file,
				struct drm_panthor_perf_session_create *create_args,
				unsigned long *sid)
{
	struct panthor_file *pfile = file->driver_priv;
	struct panthor_device *ptdev = pfile->ptdev;
	struct panthor_perf_bo ringbuffer = {}, control = {};
	const size_t slots = create_args->sample_slots;
	struct panthor_perf_pool *pool = pfile->sessions;
	struct panthor_perf_session *session;
	struct panthor_perf_enable_masks *em;
	size_t user_sample_size;
	u32 session_id;
	int ret;

	if (!ptdev->perf)
		return -EOPNOTSUPP;

	if (!pool)
		return -EINVAL;

	if (!perfmon_capable())
		return -EACCES;

	session = kzalloc_obj(*session);
	if (ZERO_OR_NULL_PTR(session))
		return -ENOMEM;

	if (!is_power_of_2(slots)) {
		drm_err(&ptdev->base,
			"Number of slots is not power-of-2 aligned\n");
		ret = -EINVAL;
		goto cleanup_session;
	}

	user_sample_size = ptdev->perf_info.sample_size * slots;

	ret = panthor_perf_bo_get(file, create_args->ringbuf_handle,
				  user_sample_size, &ringbuffer);
	if (ret)
		goto cleanup_session;

	ret = panthor_perf_bo_get(file, create_args->control_handle, 0, &control);
	if (ret)
		goto cleanup_ringbuf;

	session->eventfd = eventfd_ctx_fdget(create_args->fd);
	if (IS_ERR(session->eventfd)) {
		drm_err(&ptdev->base, "Invalid eventfd %d!\n", create_args->fd);
		ret = PTR_ERR_OR_ZERO(session->eventfd) ?: -EINVAL;
		goto cleanup_control;
	}

	em = panthor_perf_create_em(create_args);
	if (IS_ERR_OR_NULL(em)) {
		ret = -ENOMEM;
		goto cleanup_eventfd;
	}

	kref_init(&session->ref);
	INIT_LIST_HEAD(&session->sessions);
	init_waitqueue_head(&session->state_wq);
	mutex_init(&session->state_lock);
	session->control = control.vaddr;
	*session->control = (struct drm_panthor_perf_ringbuf_control) { 0 };
	session->samples = ringbuffer.vaddr;
	session->enabled_counters = em;
	session->ring_buf = ringbuffer;
	session->ringbuf_slots = slots;
	session->control_buf = control;
	session->accum_idx = U32_MAX;

	ret = pm_runtime_resume_and_get(ptdev->base.dev);
	if (ret)
		goto cleanup_obj;

	ret = panthor_perf_session_add(ptdev->perf, session, create_args->block_set);
	if (ret)
		goto cleanup_pm;

	ret = xa_alloc_cyclic(&pfile->sessions->xa, &session_id, session,
			pfile->sessions->session_range,
			&pfile->sessions->next, GFP_KERNEL);
	if (ret < 0) {
		drm_err(&ptdev->base, "File session limit exceeded.\n");
		ret = -EBUSY;
		goto cleanup_tracked;
	}

	*sid = session_id;
	return 0;

cleanup_tracked:
	panthor_perf_session_remove(pfile->ptdev->perf, session);

cleanup_pm:
	pm_runtime_put(ptdev->base.dev);

cleanup_obj:
	session_put(session);
	return ret;

cleanup_eventfd:
	eventfd_ctx_put(session->eventfd);

cleanup_control:
	panthor_perf_bo_put(&control);

cleanup_ringbuf:
	panthor_perf_bo_put(&ringbuffer);

cleanup_session:
	kfree(session);

	return ret;
}

static int session_try_stop(struct panthor_perf_session *session, u64 user_data)
{
	guard(mutex)(&session->state_lock);

	if (!session_is_active(session))
		return 0;

	if (session_is_starting(session))
		return -EBUSY;

	if (session->pending_sample)
		return -EBUSY;

	if (!session_has_space(session, 1))
		return -EBUSY;

	if (session_is_stopping(session))
		return 1;

	session_set_state(session, PANTHOR_PERF_SESSION_STOPPING);
	session->sample_start_ns = ktime_get_raw_ns();
	session->user_data = user_data;

	return 1;
}

static bool session_is_stopped_locked(struct panthor_perf_session *session)
{
	guard(mutex)(&session->state_lock);

	return session_is_stopped(session);
}

static int session_stop(struct panthor_perf *perf, struct panthor_perf_session *session,
			u64 user_data)
{
	int ret;
	ret = session_try_stop(session, user_data);
	if (ret <= 0)
		return ret;

	return panthor_perf_request_sync(perf, session, session_is_stopped_locked);
}

static int session_try_start(struct panthor_perf_session *session, u64 user_data)
{
	guard(mutex)(&session->state_lock);

	if (session_is_active(session))
		return 0;

	if (session_is_stopping(session))
		return -EBUSY;

	if (session_is_active(session) || session_is_starting(session))
		return 0;

	session_set_state(session, PANTHOR_PERF_SESSION_STARTING);
	session->overflow = false;
	session->sample_start_ns = ktime_get_raw_ns();
	session->user_data = user_data;

	return 1;
}

static bool session_is_active_locked(struct panthor_perf_session *session)
{
	guard(mutex)(&session->state_lock);

	return session_is_active(session);
}

static int session_start(struct panthor_perf *perf, struct panthor_perf_session *session,
			 u64 user_data)
{
	int ret;

	ret = session_try_start(session, user_data);
	if (ret <= 0)
		return ret;

	return panthor_perf_request_sync(perf, session, session_is_active_locked);
}

static int session_try_sample(struct panthor_perf_session *session, u64 user_data)
{
	guard(mutex)(&session->state_lock);

	if (!session_is_active(session))
		return -EINVAL;

	if (session_is_stopping(session))
		return -EBUSY;

	if (session_has_overflow(session))
		return -EACCES;

	if (session->pending_sample)
		return -EBUSY;

	/*
	 * Must have at least two slots remaining in the ringbuffer to sample: one for
	 * the current sample, and one for a stop sample, since a stop command should
	 * always be acknowledged by taking a final sample and stopping the session.
	 */
	if (!session_has_space(session, 2))
		return -ENOSPC;

	session->user_data = user_data;
	session->sample_start_ns = ktime_get_raw_ns();
	session->pending_sample = true;

	return 1;
}

static int session_sample(struct panthor_perf *perf, struct panthor_perf_session *session,
			  u64 user_data)
{
	int ret;

	ret = session_try_sample(session, user_data);
	if (ret <= 0)
		return ret;

	perf_fw_request_sample(perf);

	return 0;
}

static int session_try_destroy(struct panthor_perf_session *session)
{
	guard(mutex)(&session->state_lock);

	if (session_is_active(session))
		return -EINVAL;

	if (session_is_starting(session) || session_is_stopping(session))
		return -EBUSY;

	return 0;
}

/**
 * panthor_perf_session_destroy - Destroy the session associated with @handle.
 * @pfile: Open panthor file.
 * @handle: Session identifier.
 *
 * Only stopped sessions can be destroyed. Other session states return an error.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_destroy(struct panthor_file *pfile, u32 handle)
{
	struct panthor_perf_session *session;
	struct panthor_perf *perf = pfile->ptdev->perf;
	int ret;

	if (!perf)
		return -EOPNOTSUPP;

	scoped_guard(mutex, &perf->op_lock) {
		session = xa_load(&pfile->sessions->xa, handle);
		if (!session)
			return -EINVAL;

		ret = session_try_destroy(session);
		if (ret)
			return ret;

		xa_erase(&pfile->sessions->xa, handle);
		ret = perf_disable(perf);

		scoped_guard(mutex, &perf->session_lock)
			perf_detach_session(perf, session);
	}

	session_put(session);
	pm_runtime_put_sync(perf->ptdev->base.dev);

	return ret;
}

/**
 * panthor_perf_session_start - Start sampling on a stopped session.
 * @pfile: Open panthor file.
 * @handle: Session identifier for the desired session.
 * @user_data: Opaque value passed in from userspace.
 *
 * A session counts as stopped when it is created or when it is explicitly stopped after being
 * started. Starting an active session is treated as a no-op.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_start(struct panthor_file *pfile, u32 handle, u64 user_data)
{
	struct panthor_perf_session *session;
	struct panthor_perf *perf = pfile->ptdev->perf;
	int err;

	if (!perf)
		return -EOPNOTSUPP;

	guard(mutex)(&perf->op_lock);

	session = session_from_handle(pfile->sessions, handle);
	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_start(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_session_stop - Stop sampling on an active session.
 * @pfile: Open panthor file.
 * @handle: Session identifier for the desired session.
 * @user_data: An opaque value passed in from userspace.
 *
 * A session counts as active when it has been explicitly started via the PERF_SESSION_START
 * ioctl. Stopping a stopped session is treated as a no-op.
 *
 * To ensure data is not lost when sampling is stopping, there must always be at least one slot
 * available for the final automatic sample, and the stop command will be rejected if there is not.
 *
 * The @user_data will always be associated with the final sample.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_stop(struct panthor_file *pfile, unsigned int handle, u64 user_data)
{
	struct panthor_perf_session *session;
	struct panthor_perf *perf = pfile->ptdev->perf;
	int err;

	if (!perf)
		return -EOPNOTSUPP;

	guard(mutex)(&perf->op_lock);

	session = session_from_handle(pfile->sessions, handle);
	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_stop(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_session_sample - Request a sample on a manual sampling session.
 * @pfile: Open panthor file.
 * @handle: Session identifier for the desired session.
 * @user_data: An opaque value passed in from userspace.
 *
 * Only an active manual sampler is permitted to request samples directly. Failing to meet either
 * of these conditions will cause the sampling request to be rejected. Requesting a manual sample
 * with a full ringbuffer will see the request being rejected.
 *
 * The @user_data will always be unambiguously associated one-to-one with the resultant sample.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_sample(struct panthor_file *pfile, u32 handle, u64 user_data)
{
	struct panthor_perf_session *session;
	struct panthor_perf *perf = pfile->ptdev->perf;
	int err;

	if (!perf)
		return -EOPNOTSUPP;

	guard(mutex)(&perf->op_lock);

	session = session_from_handle(pfile->sessions, handle);
	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_sample(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_unplug - Terminate the performance counter subsystem.
 * @ptdev: Panthor device.
 *
 * Release the performance counter hardware resources. The control structure is
 * released with the DRM device.
 */
void panthor_perf_unplug(struct panthor_device *ptdev)
{
	int ret;
	struct panthor_perf *perf = ptdev->perf;

	if (!perf)
		return;

	scoped_guard(mutex, &perf->op_lock)
		ret = perf_disable(perf);
	if (ret)
		drm_err(&perf->ptdev->base, "Failed to disable performance counter sampling!");

	disable_work_sync(&perf->sample_work);

	panthor_kernel_bo_vunmap(perf->rb);
	panthor_kernel_bo_destroy(perf->rb);

	ptdev->perf = NULL;
}

int panthor_perf_pool_create(struct panthor_file *pfile)
{
	struct panthor_perf_pool *pool;

	pool = kzalloc_obj(*pool);
	if (!pool)
		return -ENOMEM;

	xa_init_flags(&pool->xa, XA_FLAGS_ALLOC);

	pool->session_range.min = 0;
	pool->session_range.max = MAX_SESSIONS_PER_FILE - 1;

	pfile->sessions = pool;

	return 0;
}

void panthor_perf_pool_destroy(struct panthor_file *pfile)
{
	struct panthor_perf_pool *pool = pfile->sessions;
	unsigned long handle;
	struct panthor_perf_session *session;

	if (IS_ERR_OR_NULL(pool))
		return;

	if (!pfile->ptdev->perf)
		return;

	xa_for_each(&pool->xa, handle, session) {
		xa_erase(&pool->xa, handle);
		panthor_perf_session_remove(pfile->ptdev->perf, session);
		session_put(session);
		pm_runtime_put_sync(pfile->ptdev->base.dev);
	}

	xa_destroy(&pool->xa);
	kfree(pool);

	pfile->sessions = NULL;
}
