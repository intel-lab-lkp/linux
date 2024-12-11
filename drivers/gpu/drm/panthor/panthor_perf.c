// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2023 Collabora Ltd */
/* Copyright 2024 Arm ltd. */

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/panthor_drm.h>

#include <linux/circ_buf.h>
#include <linux/iosys-map.h>
#include <linux/pm_runtime.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_gem.h"
#include "panthor_gpu.h"
#include "panthor_mmu.h"
#include "panthor_perf.h"
#include "panthor_regs.h"

/**
 * PANTHOR_PERF_EM_BITS - Number of bits in a user-facing enable mask. This must correspond
 *                        to the maximum number of counters available for selection on the newest
 *                        Mali GPUs (128 as of the Mali-Gx15).
 */
#define PANTHOR_PERF_EM_BITS (BITS_PER_TYPE(u64) * 2)

/**
 * PANTHOR_PERF_FW_RINGBUF_SLOTS - Number of slots allocated for individual samples when configuring
 *                                 the performance counter ring buffer to firmware. This can be
 *                                 used to reduce memory consumption on low memory systems.
 */
#define PANTHOR_PERF_FW_RINGBUF_SLOTS (32)

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
 * enum panthor_perf_session_state - Session state bits.
 */
enum panthor_perf_session_state {
	/** @PANTHOR_PERF_SESSION_ACTIVE: The session is active and can be used for sampling. */
	PANTHOR_PERF_SESSION_ACTIVE = 0,

	/**
	 * @PANTHOR_PERF_SESSION_OVERFLOW: The session encountered an overflow in one of the
	 *                                 counters during the last sampling period. This flag
	 *                                 gets propagated as part of samples emitted for this
	 *                                 session, to ensure the userspace client can gracefully
	 *                                 handle this data corruption.
	 */
	PANTHOR_PERF_SESSION_OVERFLOW,

	/** @PANTHOR_PERF_SESSION_MAX: Bits needed to represent the state. Must be last.*/
	PANTHOR_PERF_SESSION_MAX,
};

struct panthor_perf_enable_masks {
	/**
	 * @link: List node used to keep track of the enable masks aggregated by the sampler.
	 */
	struct list_head link;

	/** @refs: Number of references taken out on an instantiated enable mask. */
	struct kref refs;

	/**
	 * @mask: Array of bitmasks indicating the counters userspace requested, where
	 *        one bit represents a single counter. Used to build the firmware configuration
	 *        and ensure that userspace clients obtain only the counters they requested.
	 */
	DECLARE_BITMAP(mask, PANTHOR_PERF_EM_BITS)[DRM_PANTHOR_PERF_BLOCK_MAX];
};

struct panthor_perf_counter_block {
	struct drm_panthor_perf_block_header header;
	u64 counters[];
};

struct panthor_perf_session {
	DECLARE_BITMAP(state, PANTHOR_PERF_SESSION_MAX);

	/**
	 * @user_sample_size: The size of a single sample as exposed to userspace. For the sake of
	 *                    simplicity, the current implementation exposes the same structure
	 *                    as provided by firmware, after annotating the sample and the blocks,
	 *                    and zero-extending the counters themselves (to account for in-kernel
	 *                    accumulation).
	 *
	 *                    This may also allow further memory-optimizations of compressing the
	 *                    sample to provide only requested blocks, if deemed to be worth the
	 *                    additional complexity.
	 */
	size_t user_sample_size;

	/**
	 * @sample_freq_ns: Period between subsequent sample requests. Zero indicates that
	 *                  userspace will be responsible for requesting samples.
	 */
	u64 sample_freq_ns;

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

	/** @ring_buf: BO for the userspace ringbuffer. */
	struct drm_gem_object *ring_buf;

	/**
	 * @control_buf: BO for the insert and extract indices.
	 */
	struct drm_gem_object *control_buf;

	/**
	 * @extract_idx: The extract index is used by userspace to indicate the position of the
	 *               consumer in the ringbuffer.
	 */
	u32 *extract_idx;

	/**
	 * @insert_idx: The insert index is used by the kernel to indicate the position of the
	 *              latest sample exposed to userspace.
	 */
	u32 *insert_idx;

	/** @samples: The mapping of the @ring_buf into the kernel's VA space. */
	u8 *samples;

	/**
	 * @waiting: The list node used by the sampler to track the sessions waiting for a sample.
	 */
	struct list_head waiting;

	/**
	 * @pfile: The panthor file which was used to create a session, used for the postclose
	 *         handling and to prevent a misconfigured userspace from closing unrelated
	 *         sessions.
	 */
	struct panthor_file *pfile;

	/**
	 * @ref: Session reference count. The sample delivery to userspace is asynchronous, meaning
	 *       the lifetime of the session must extend at least until the sample is exposed to
	 *       userspace.
	 */
	struct kref ref;
};

struct panthor_perf_buffer_descriptor {
	/**
	 * @block_size: The size of a single block in the FW ring buffer, equal to
	 *              sizeof(u32) * counters_per_block.
	 */
	size_t block_size;

	/**
	 * @buffer_size: The total size of the buffer, equal to (#hardware blocks +
	 *               #firmware blocks) * block_size.
	 */
	size_t buffer_size;

	/**
	 * @available_blocks: Bitmask indicating the blocks supported by the hardware and firmware
	 *                    combination. Note that this can also include blocks that will not
	 *                    be exposed to the user.
	 */
	DECLARE_BITMAP(available_blocks, DRM_PANTHOR_PERF_BLOCK_MAX);
	struct {
		/** @offset: Starting offset of a block of type @type in the FW ringbuffer. */
		size_t offset;

		/** @type: Type of the blocks between @blocks[i].offset and @blocks[i+1].offset. */
		enum drm_panthor_perf_block_type type;

		/** @block_count: Number of blocks of the given @type, starting at @offset. */
		size_t block_count;
	} blocks[DRM_PANTHOR_PERF_BLOCK_MAX];
};


/**
 * struct panthor_perf_sampler - Interface to de-multiplex firmware interaction and handle
 *                               global interactions.
 */
struct panthor_perf_sampler {
	/** @sample_requested: A sample has been requested. */
	bool sample_requested;

	/**
	 * @last_ack: Temporarily storing the last GLB_ACK status. Without storing this data,
	 *            we do not know whether a toggle bit has been handled.
	 */
	u32 last_ack;

	/**
	 * @enabled_clients: The number of clients concurrently requesting samples. To ensure that
	 *                   one client cannot deny samples to another, we must ensure that clients
	 *                   are effectively reference counted.
	 */
	atomic_t enabled_clients;

	/**
	 * @sample_handled: Synchronization point between the interrupt bottom half and the
	 *                  main sampler interface. Must be re-armed solely on a new request
	 *                  coming to the sampler.
	 */
	struct completion sample_handled;

	/** @rb: Kernel BO in the FW AS containing the sample ringbuffer. */
	struct panthor_kernel_bo *rb;

	/**
	 * @sample_size: The size of a single sample in the FW ringbuffer. This is computed using
	 *               the hardware configuration according to the architecture specification,
	 *               and cross-validated against the sample size reported by FW to ensure
	 *               a consistent view of the buffer size.
	 */
	size_t sample_size;

	/**
	 * @sample_slots: Number of slots for samples in the FW ringbuffer. Could be static,
	 *		  but may be useful to customize for low-memory devices.
	 */
	size_t sample_slots;

	/**
	 * @config_lock: Lock serializing changes to the global counter configuration, including
	 *               requested counter set and the counters themselves.
	 */
	struct mutex config_lock;

	/**
	 * @ems: List of enable maps of the active sessions. When removing a session, the number
	 *       of requested counters may decrease, and the union of enable masks from the multiple
	 *       sessions does not provide sufficient information to reconstruct the previous
	 *       enable mask.
	 */
	struct list_head ems;

	/** @em: Combined enable mask for all of the active sessions. */
	struct panthor_perf_enable_masks *em;

	/**
	 * @desc: Buffer descriptor for a sample in the FW ringbuffer. Note that this buffer
	 *        at current time does some interesting things with the zeroth block type. On
	 *        newer FW revisions, the first counter block of the sample is the METADATA block,
	 *        which contains a single value indicating the reason the sample was taken (if
	 *        any). This block must not be exposed to userspace, as userspace does not
	 *        have sufficient context to interpret it. As such, this block type is not
	 *        added to the uAPI, but we still use it in the kernel.
	 */
	struct panthor_perf_buffer_descriptor desc;

	/**
	 * @sample: Pointer to an upscaled and annotated sample that may be emitted to userspace.
	 *          This is used both as an intermediate buffer to do the zero-extension of the
	 *          32-bit counters to 64-bits and as a storage buffer in case the sampler
	 *          requests an additional sample that was not requested by any of the top-level
	 *          sessions (for instance, when changing the enable masks).
	 */
	u8 *sample;

	/** @sampler_lock: Lock used to guard the list of sessions requesting samples. */
	struct mutex sampler_lock;

	/** @sampler_list: List of sessions requesting samples. */
	struct list_head sampler_list;

	/** @set_config: The set that will be configured onto the hardware. */
	u8 set_config;

	/**
	 * @ptdev: Backpointer to the Panthor device, needed to ring the global doorbell and
	 *         interface with FW.
	 */
	struct panthor_device *ptdev;
};

struct panthor_perf {
	/**
	 * @block_set: The global counter set configured onto the HW.
	 */
	u8 block_set;

	/** @next_session: The ID of the next session. */
	u32 next_session;

	/** @session_range: The number of sessions supported at a time. */
	struct xa_limit session_range;

	/**
	 * @sessions: Global map of sessions, accessed by their ID.
	 */
	struct xarray sessions;

	/** @sampler: FW control interface. */
	struct panthor_perf_sampler sampler;
};

/**
 * PANTHOR_PERF_COUNTERS_PER_BLOCK - On CSF architectures pre-11.x, the number of counters
 * per block was hardcoded to be 64. Arch 11.0 onwards supports the PRFCNT_FEATURES GPU register,
 * which indicates the same information.
 */
#define PANTHOR_PERF_COUNTERS_PER_BLOCK (64)

void panthor_perf_info_init(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	if (PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features))
		perf_info->flags |= DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 11)
		perf_info->counters_per_block = PANTHOR_PERF_COUNTERS_PER_BLOCK;

	perf_info->sample_header_size = sizeof(struct drm_panthor_perf_sample_header);
	perf_info->block_header_size = sizeof(struct drm_panthor_perf_block_header);

	if (GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size)) {
		perf_info->fw_blocks = 1;
		perf_info->csg_blocks = glb_iface->control->group_num;
	}

	perf_info->cshw_blocks = 1;
	perf_info->tiler_blocks = 1;
	perf_info->memsys_blocks = hweight64(ptdev->gpu_info.l2_present);
	perf_info->shader_blocks = hweight64(ptdev->gpu_info.shader_present);
}

static struct panthor_perf_enable_masks *panthor_perf_em_new(void)
{
	struct panthor_perf_enable_masks *em = kmalloc(sizeof(*em), GFP_KERNEL);

	if (!em)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&em->link);

	kref_init(&em->refs);

	return em;
}

static struct panthor_perf_enable_masks *panthor_perf_create_em(struct drm_panthor_perf_cmd_setup
		*setup_args)
{
	struct panthor_perf_enable_masks *em = panthor_perf_em_new();

	if (IS_ERR_OR_NULL(em))
		return em;

	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_FW],
			setup_args->fw_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_CSG],
			setup_args->csg_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_CSHW],
			setup_args->cshw_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_TILER],
			setup_args->tiler_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_MEMSYS],
			setup_args->memsys_enable_mask, PANTHOR_PERF_EM_BITS);
	bitmap_from_arr64(em->mask[DRM_PANTHOR_PERF_BLOCK_SHADER],
			setup_args->shader_enable_mask, PANTHOR_PERF_EM_BITS);

	return em;
}

static void panthor_perf_em_add(struct panthor_perf_enable_masks *dst_em,
		const struct panthor_perf_enable_masks *const src_em)
{
	size_t i = 0;

	for (i = DRM_PANTHOR_PERF_BLOCK_FW; i <= DRM_PANTHOR_PERF_BLOCK_LAST; i++)
		bitmap_or(dst_em->mask[i], dst_em->mask[i], src_em->mask[i], PANTHOR_PERF_EM_BITS);
}

static void panthor_perf_em_zero(struct panthor_perf_enable_masks *em)
{
	size_t i = 0;

	for (i = DRM_PANTHOR_PERF_BLOCK_FW; i <= DRM_PANTHOR_PERF_BLOCK_LAST; i++)
		bitmap_zero(em->mask[i], PANTHOR_PERF_EM_BITS);
}

static void panthor_perf_destroy_em_kref(struct kref *em_kref)
{
	struct panthor_perf_enable_masks *em = container_of(em_kref, typeof(*em), refs);

	if (!list_empty(&em->link))
		return;

	kfree(em);
}

static size_t get_annotated_block_size(size_t counters_per_block)
{
	return struct_size_t(struct panthor_perf_counter_block, counters, counters_per_block);
}

static u32 session_read_extract_idx(struct panthor_perf_session *session)
{
	/* Userspace will update their own extract index to indicate that a sample is consumed
	 * from the ringbuffer, and we must ensure we read the latest value.
	 */
	return smp_load_acquire(session->extract_idx);
}

static void session_write_insert_idx(struct panthor_perf_session *session, u32 idx)
{
	/* Userspace needs the insert index to know where to look for the sample. */
	smp_store_release(session->insert_idx, idx);
}

static u32 session_read_insert_idx(struct panthor_perf_session *session)
{
	return *session->insert_idx;
}

static void session_get(struct panthor_perf_session *session)
{
	kref_get(&session->ref);
}

static void session_free(struct kref *ref)
{
	struct panthor_perf_session *session = container_of(ref, typeof(*session), ref);

	if (session->samples) {
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(session->samples);

		drm_gem_vunmap_unlocked(session->ring_buf, &map);
		drm_gem_object_put(session->ring_buf);
	}

	if (session->insert_idx && session->extract_idx) {
		struct iosys_map map = IOSYS_MAP_INIT_VADDR(session->extract_idx);

		drm_gem_vunmap_unlocked(session->control_buf, &map);
		drm_gem_object_put(session->control_buf);
	}

	kref_put(&session->enabled_counters->refs, panthor_perf_destroy_em_kref);
	eventfd_ctx_put(session->eventfd);

	devm_kfree(session->pfile->ptdev->base.dev, session);
}

static void session_put(struct panthor_perf_session *session)
{
	kref_put(&session->ref, session_free);
}

/**
 * session_find - Find a session associated with the given session ID and
 *                panthor_file.
 * @pfile: Panthor file.
 * @perf: Panthor perf.
 * @sid: Session ID.
 *
 * The reference count of a valid session is increased to ensure it does not disappear
 * in the window between the XA lock being dropped and the internal session functions
 * being called.
 *
 * Return: valid session pointer or an ERR_PTR.
 */
static struct panthor_perf_session *session_find(struct panthor_file *pfile,
		struct panthor_perf *perf, u32 sid)
{
	struct panthor_perf_session *session;

	if (!perf)
		return ERR_PTR(-EINVAL);

	xa_lock(&perf->sessions);
	session = xa_load(&perf->sessions, sid);

	if (!session || xa_is_err(session)) {
		xa_unlock(&perf->sessions);
		return ERR_PTR(-EBADF);
	}

	if (session->pfile != pfile) {
		xa_unlock(&perf->sessions);
		return ERR_PTR(-EINVAL);
	}

	session_get(session);
	xa_unlock(&perf->sessions);

	return session;
}

static u32 compress_enable_mask(unsigned long *const src)
{
	size_t i;
	u32 result = 0;
	unsigned long clump;

	for_each_set_clump8(i, clump, src, PANTHOR_PERF_EM_BITS) {
		const unsigned long shift = div_u64(i, 4);

		result |= !!(clump & GENMASK(3, 0)) << shift;
		result |= !!(clump & GENMASK(7, 4)) << (shift + 1);
	}

	return result;
}

static void expand_enable_mask(u32 em, unsigned long *const dst)
{
	size_t i;
	DECLARE_BITMAP(emb, BITS_PER_TYPE(u32));

	bitmap_from_arr32(emb, &em, BITS_PER_TYPE(u32));

	for_each_set_bit(i, emb, BITS_PER_TYPE(u32))
		bitmap_set(dst, i * 4, 4);
}

/**
 * panthor_perf_block_data - Identify the block index and type based on the offset.
 *
 * @desc:   FW buffer descriptor.
 * @offset: The current offset being examined.
 * @idx:    Pointer to an output index.
 * @type:   Pointer to an output block type.
 *
 * To disambiguate different types of blocks as well as different blocks of the same type,
 * the offset into the FW ringbuffer is used to uniquely identify the block being considered.
 *
 * In the future, this is a good time to identify whether a block will be empty,
 * allowing us to short-circuit its processing after emitting header information.
 */
static void panthor_perf_block_data(struct panthor_perf_buffer_descriptor *const desc,
		size_t offset, u32 *idx, enum drm_panthor_perf_block_type *type)
{
	unsigned long id;

	for_each_set_bit(id, desc->available_blocks, DRM_PANTHOR_PERF_BLOCK_LAST) {
		const size_t block_start = desc->blocks[id].offset;
		const size_t block_count = desc->blocks[id].block_count;
		const size_t block_end = desc->blocks[id].offset +
			desc->block_size * block_count;

		if (!block_count)
			continue;

		if ((offset >= block_start) && (offset < block_end)) {
			*type = desc->blocks[id].type;
			*idx = div_u64(offset - desc->blocks[id].offset, desc->block_size);

			return;
		}
	}
}

static size_t session_get_max_sample_size(const struct drm_panthor_perf_info *const info)
{
	const size_t block_size = get_annotated_block_size(info->counters_per_block);
	const size_t block_nr = info->cshw_blocks + info->csg_blocks + info->fw_blocks +
		info->tiler_blocks + info->memsys_blocks + info->shader_blocks;

	return sizeof(struct drm_panthor_perf_sample_header) + (block_size * block_nr);
}

static u32 panthor_perf_handle_sample(struct panthor_device *ptdev, u32 extract_idx, u32 insert_idx)
{
	struct panthor_perf *perf = ptdev->perf;
	struct panthor_perf_sampler *sampler = &ptdev->perf->sampler;
	const size_t ann_block_size =
		get_annotated_block_size(ptdev->perf_info.counters_per_block);
	u32 i;

	for (i = extract_idx; i != insert_idx; i = (i + 1) % sampler->sample_slots) {
		u8 *fw_sample = (u8 *)sampler->rb->kmap + i * sampler->sample_size;

		for (size_t fw_off = 0, ann_off = sizeof(struct drm_panthor_perf_sample_header);
				fw_off < sampler->desc.buffer_size;
				fw_off += sampler->desc.block_size)

		{
			u32 idx;
			enum drm_panthor_perf_block_type type;
			DECLARE_BITMAP(expanded_em, PANTHOR_PERF_EM_BITS);
			struct panthor_perf_counter_block *blk =
				(typeof(blk))(perf->sampler.sample + ann_off);
			const u32 prfcnt_en = blk->counters[PANTHOR_CTR_PRFCNT_EN];

			panthor_perf_block_data(&sampler->desc, fw_off, &idx, &type);

			/**
			 * TODO Data from the metadata block must be used to populate the
			 * block state information.
			 */
			if (type == DRM_PANTHOR_PERF_BLOCK_METADATA)
				continue;

			expand_enable_mask(prfcnt_en, expanded_em);

			blk->header = (struct drm_panthor_perf_block_header) {
				.clock = 0,
				.block_idx = idx,
				.block_type = type,
				.block_states = DRM_PANTHOR_PERF_BLOCK_STATE_UNKNOWN
			};
			bitmap_to_arr64(blk->header.enable_mask, expanded_em, PANTHOR_PERF_EM_BITS);

			u32 *block = (u32 *)(fw_sample + fw_off);

			/*
			 * The four header counters must be treated differently, because they are
			 * not additive. For the fourth, the assignment does not matter, as it
			 * is reserved and should be zero.
			 */
			blk->counters[PANTHOR_CTR_TIMESTAMP_LO] = block[PANTHOR_CTR_TIMESTAMP_LO];
			blk->counters[PANTHOR_CTR_TIMESTAMP_HI] = block[PANTHOR_CTR_TIMESTAMP_HI];
			blk->counters[PANTHOR_CTR_PRFCNT_EN] = block[PANTHOR_CTR_PRFCNT_EN];

			for (size_t k = PANTHOR_HEADER_COUNTERS;
					k < ptdev->perf_info.counters_per_block;
					k++)
				blk->counters[k] += block[k];

			ann_off += ann_block_size;
		}
	}

	return i;
}

static size_t panthor_perf_get_fw_reported_size(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);

	size_t fw_size = GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size);
	size_t hw_size = GLB_PERFCNT_HW_SIZE(glb_iface->control->perfcnt_size);
	size_t md_size = PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features);

	return md_size + fw_size + hw_size;
}

#define PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, typ, blk_count, offset) \
	({ \
		(desc)->blocks[(typ)].type = (typ); \
		(desc)->blocks[(typ)].offset = (offset); \
		(desc)->blocks[(typ)].block_count = (blk_count);  \
		if ((blk_count))                                    \
			set_bit((typ), (desc)->available_blocks); \
		(offset) + ((desc)->block_size) * (blk_count); \
	 })

static int panthor_perf_setup_fw_buffer_desc(struct panthor_device *ptdev,
		struct panthor_perf_sampler *sampler)
{
	const struct drm_panthor_perf_info *const info = &ptdev->perf_info;
	const size_t block_size = info->counters_per_block * sizeof(u32);
	struct panthor_perf_buffer_descriptor *desc = &sampler->desc;
	const size_t fw_sample_size = panthor_perf_get_fw_reported_size(ptdev);
	size_t offset = 0;

	desc->block_size = block_size;

	for (enum drm_panthor_perf_block_type type = 0; type < DRM_PANTHOR_PERF_BLOCK_MAX; type++) {
		switch (type) {
		case DRM_PANTHOR_PERF_BLOCK_METADATA:
			if (info->flags & DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT)
				offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc,
					DRM_PANTHOR_PERF_BLOCK_METADATA, 1, offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_FW:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->fw_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_CSG:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->csg_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_CSHW:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->cshw_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_TILER:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->tiler_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_MEMSYS:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->memsys_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_SHADER:
			offset = PANTHOR_PERF_SET_BLOCK_DESC_DATA(desc, type, info->shader_blocks,
					offset);
			break;
		case DRM_PANTHOR_PERF_BLOCK_MAX:
			drm_WARN_ON_ONCE(&ptdev->base,
					"DRM_PANTHOR_PERF_BLOCK_MAX should be unreachable!");
			break;
		}
	}

	/* Computed size is not the same as the reported size, so we should not proceed in
	 * initializing the sampling session.
	 */
	if (offset != fw_sample_size)
		return -EINVAL;

	desc->buffer_size = offset;

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
	gpu_write(ptdev, CSF_DOORBELL(CSF_GLB_DOORBELL_ID), 1);
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

	panthor_fw_update_reqs(glb_iface, req, GLB_PERFCNT_ENABLE, GLB_PERFCNT_ENABLE);
	gpu_write(ptdev, CSF_DOORBELL(CSF_GLB_DOORBELL_ID), 1);
	ret = panthor_fw_glb_wait_acks(ptdev, GLB_PERFCNT_ENABLE, &acked, 100);
	if (ret)
		drm_warn(&ptdev->base, "Could not enable performance counters");

	return ret;
}

static void panthor_perf_fw_write_em(struct panthor_perf_sampler *sampler,
		struct panthor_perf_enable_masks *em)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(sampler->ptdev);
	u32 perfcnt_config;

	glb_iface->input->perfcnt_csf_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_CSHW]);
	glb_iface->input->perfcnt_shader_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_SHADER]);
	glb_iface->input->perfcnt_mmu_l2_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_MEMSYS]);
	glb_iface->input->perfcnt_tiler_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_TILER]);
	glb_iface->input->perfcnt_fw_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_FW]);
	glb_iface->input->perfcnt_csg_enable =
		compress_enable_mask(em->mask[DRM_PANTHOR_PERF_BLOCK_CSG]);

	perfcnt_config = GLB_PRFCNT_CONFIG_SIZE(PANTHOR_PERF_FW_RINGBUF_SLOTS);
	perfcnt_config |= GLB_PRFCNT_CONFIG_SET(sampler->set_config);
	glb_iface->input->perfcnt_config = perfcnt_config;

	/**
	 * The spec mandates that the host zero the PRFCNT_EXTRACT register before an enable
	 * operation, and each (re-)enable will require an enable-disable pair to program
	 * the new changes onto the FW interface.
	 */
	WRITE_ONCE(glb_iface->input->perfcnt_extract, 0);
}

static void session_populate_sample_header(struct panthor_perf_session *session,
		struct drm_panthor_perf_sample_header *hdr)
{
	hdr->block_set = 0;
	hdr->user_data = session->user_data;
	hdr->timestamp_start_ns = session->sample_start_ns;
	/**
	 * TODO This should be changed to use the GPU clocks and the TIMESTAMP register,
	 * when support is added.
	 */
	hdr->timestamp_end_ns = ktime_get_raw_ns();
}

/**
 * session_patch_sample - Update the PRFCNT_EN header counter and the counters exposed to the
 *                        userspace client to only contain requested counters.
 *
 * @ptdev: Panthor device
 * @session: Perf session
 * @sample: Starting offset of the sample in the userspace mapping.
 *
 * The hardware supports counter selection at the granularity of 1 bit per 4 counters, and there
 * is a single global FW frontend to program the counter requests from multiple sessions. This may
 * lead to a large disparity between the requested and provided counters for an individual client.
 * To remove this cross-talk, we patch out the counters that have not been requested by this
 * session and update the PRFCNT_EN, the header counter containing a bitmask of enabled counters,
 * accordingly.
 */
static void session_patch_sample(struct panthor_device *ptdev,
		struct panthor_perf_session *session, u8 *sample)
{
	const struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	const size_t block_size = get_annotated_block_size(perf_info->counters_per_block);
	const size_t sample_size = session_get_max_sample_size(perf_info);

	for (size_t i = 0; i < sample_size; i += block_size) {
		size_t ctr_idx;
		DECLARE_BITMAP(em_diff, PANTHOR_PERF_EM_BITS);
		struct panthor_perf_counter_block *blk = (typeof(blk))(sample + block_size);
		enum drm_panthor_perf_block_type type = blk->header.block_type;
		unsigned long *blk_em = session->enabled_counters->mask[type];

		bitmap_from_arr64(em_diff, blk->header.enable_mask, PANTHOR_PERF_EM_BITS);

		bitmap_andnot(em_diff, em_diff, blk_em, PANTHOR_PERF_EM_BITS);

		for_each_set_bit(ctr_idx, em_diff, PANTHOR_PERF_EM_BITS)
			blk->counters[ctr_idx] = 0;

		bitmap_to_arr64(blk->header.enable_mask, blk_em, PANTHOR_PERF_EM_BITS);
	}
}

static int session_copy_sample(struct panthor_device *ptdev,
		struct panthor_perf_session *session)
{
	struct panthor_perf *perf = ptdev->perf;
	const size_t sample_size = session_get_max_sample_size(&ptdev->perf_info);
	const u32 insert_idx = session_read_insert_idx(session);
	const u32 extract_idx = session_read_extract_idx(session);
	u8 *new_sample;

	if (!CIRC_SPACE_TO_END(insert_idx, extract_idx, session->ringbuf_slots))
		return -ENOSPC;

	new_sample = session->samples + extract_idx * sample_size;

	memcpy(new_sample, perf->sampler.sample, sample_size);

	session_populate_sample_header(session,
			(struct drm_panthor_perf_sample_header *)new_sample);

	session_patch_sample(ptdev, session, new_sample +
			sizeof(struct drm_panthor_perf_sample_header));

	session_write_insert_idx(session, (insert_idx + 1) % session->ringbuf_slots);

	/* Since we are about to notify userspace, we must ensure that all changes to memory
	 * are visible.
	 */
	wmb();

	eventfd_signal(session->eventfd);

	return 0;
}

#define PERFCNT_IRQS (GLB_PERFCNT_OVERFLOW | GLB_PERFCNT_SAMPLE | GLB_PERFCNT_THRESHOLD)

void panthor_perf_report_irq(struct panthor_device *ptdev, u32 status)
{
	struct panthor_perf *const perf = ptdev->perf;
	struct panthor_perf_sampler *sampler;
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);

	if (!(status & JOB_INT_GLOBAL_IF))
		return;

	if (!perf)
		return;

	sampler = &perf->sampler;

	/* TODO This needs locking. */
	const u32 ack = READ_ONCE(glb_iface->output->ack);
	const u32 fw_events = sampler->last_ack ^ ack;

	sampler->last_ack = ack;

	if (!(fw_events & PERFCNT_IRQS))
		return;

	/* TODO Fix up the error handling for overflow. */
	if (fw_events & GLB_PERFCNT_OVERFLOW)
		return;

	if (fw_events & (GLB_PERFCNT_SAMPLE | GLB_PERFCNT_THRESHOLD)) {
		const u32 extract_idx = READ_ONCE(glb_iface->input->perfcnt_extract);
		const u32 insert_idx = READ_ONCE(glb_iface->output->perfcnt_insert);

		WRITE_ONCE(glb_iface->input->perfcnt_extract,
				panthor_perf_handle_sample(ptdev, extract_idx, insert_idx));
	}

	scoped_guard(mutex, &sampler->sampler_lock)
	{
		struct list_head *pos, *temp;

		list_for_each_safe(pos, temp, &sampler->sampler_list) {
			struct panthor_perf_session *session = list_entry(pos,
					struct panthor_perf_session, waiting);

			session_copy_sample(ptdev, session);
			list_del_init(pos);

			session_put(session);
		}
	}

	memset(sampler->sample, 0, session_get_max_sample_size(&ptdev->perf_info));
	sampler->sample_requested = false;
	complete(&sampler->sample_handled);
}


static int panthor_perf_sampler_init(struct panthor_perf_sampler *sampler,
		struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct panthor_kernel_bo *bo;
	u8 *sample;
	int ret;

	ret = panthor_perf_setup_fw_buffer_desc(ptdev, sampler);
	if (ret) {
		drm_err(&ptdev->base,
				"Failed to setup descriptor for FW ring buffer, err = %d", ret);
		return ret;
	}

	bo = panthor_kernel_bo_create(ptdev, panthor_fw_vm(ptdev),
			sampler->desc.buffer_size * PANTHOR_PERF_FW_RINGBUF_SLOTS,
			DRM_PANTHOR_BO_NO_MMAP,
			DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC | DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED,
			PANTHOR_VM_KERNEL_AUTO_VA);

	if (IS_ERR_OR_NULL(bo))
		return IS_ERR(bo) ? PTR_ERR(bo) : -ENOMEM;

	ret = panthor_kernel_bo_vmap(bo);
	if (ret)
		goto cleanup_bo;

	sample = devm_kzalloc(ptdev->base.dev,
			session_get_max_sample_size(&ptdev->perf_info), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(sample)) {
		ret = -ENOMEM;
		goto cleanup_vmap;
	}

	glb_iface->input->perfcnt_as = panthor_vm_as(panthor_fw_vm(ptdev));
	glb_iface->input->perfcnt_base = panthor_kernel_bo_gpuva(bo);
	glb_iface->input->perfcnt_extract = 0;
	glb_iface->input->perfcnt_csg_select = GENMASK(glb_iface->control->group_num, 0);

	sampler->rb = bo;
	sampler->sample = sample;
	sampler->sample_slots = PANTHOR_PERF_FW_RINGBUF_SLOTS;

	sampler->em = panthor_perf_em_new();

	mutex_init(&sampler->sampler_lock);
	mutex_init(&sampler->config_lock);
	INIT_LIST_HEAD(&sampler->sampler_list);
	INIT_LIST_HEAD(&sampler->ems);
	init_completion(&sampler->sample_handled);

	sampler->ptdev = ptdev;

	return 0;

cleanup_vmap:
	panthor_kernel_bo_vunmap(bo);

cleanup_bo:
	panthor_kernel_bo_destroy(bo);

	return ret;
}

static void panthor_perf_sampler_term(struct panthor_perf_sampler *sampler)
{
	int ret;

	if (sampler->sample_requested)
		wait_for_completion_killable(&sampler->sample_handled);

	panthor_perf_fw_write_em(sampler, &(struct panthor_perf_enable_masks) {});

	ret = panthor_perf_fw_stop_sampling(sampler->ptdev);
	if (ret)
		drm_warn_once(&sampler->ptdev->base, "Sampler termination failed, ret = %d", ret);

	devm_kfree(sampler->ptdev->base.dev, sampler->sample);

	panthor_kernel_bo_destroy(sampler->rb);
}

static int panthor_perf_sampler_add(struct panthor_perf_sampler *sampler,
		struct panthor_perf_enable_masks *const new_em,
		u8 set)
{
	int ret = 0;

	guard(mutex)(&sampler->config_lock);

	/* Early check for whether a new set can be configured. */
	if (!atomic_read(&sampler->enabled_clients))
		sampler->set_config = set;
	else
		if (sampler->set_config != set)
			return -EBUSY;

	kref_get(&new_em->refs);
	list_add_tail(&sampler->ems, &new_em->link);

	panthor_perf_em_add(sampler->em, new_em);
	pm_runtime_get_sync(sampler->ptdev->base.dev);

	if (atomic_read(&sampler->enabled_clients)) {
		ret = panthor_perf_fw_stop_sampling(sampler->ptdev);
		if (ret)
			return ret;
	}

	panthor_perf_fw_write_em(sampler, sampler->em);

	ret = panthor_perf_fw_start_sampling(sampler->ptdev);
	if (ret)
		return ret;

	atomic_inc(&sampler->enabled_clients);

	return 0;
}

static int panthor_perf_sampler_remove(struct panthor_perf_sampler *sampler,
		struct panthor_perf_enable_masks *session_em)
{
	int ret;
	struct list_head *em_node;

	guard(mutex)(&sampler->config_lock);

	list_del_init(&session_em->link);
	kref_put(&session_em->refs, panthor_perf_destroy_em_kref);

	panthor_perf_em_zero(sampler->em);
	list_for_each(em_node, &sampler->ems)
	{
		struct panthor_perf_enable_masks *curr_em =
			container_of(em_node, typeof(*curr_em), link);

		panthor_perf_em_add(sampler->em, curr_em);
	}

	ret = panthor_perf_fw_stop_sampling(sampler->ptdev);
	if (ret)
		return ret;

	atomic_dec(&sampler->enabled_clients);
	pm_runtime_put_sync(sampler->ptdev->base.dev);

	panthor_perf_fw_write_em(sampler, sampler->em);

	if (atomic_read(&sampler->enabled_clients))
		return panthor_perf_fw_start_sampling(sampler->ptdev);
	return 0;
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
	int ret;

	if (!ptdev)
		return -EINVAL;

	perf = devm_kzalloc(ptdev->base.dev, sizeof(*perf), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(perf))
		return -ENOMEM;

	xa_init_flags(&perf->sessions, XA_FLAGS_ALLOC);

	/* Currently, we only support a single session at a time. */
	perf->session_range = (struct xa_limit) {
		.min = 0,
		.max = 1,
	};

	ret = panthor_perf_sampler_init(&perf->sampler, ptdev);
	if (ret)
		goto cleanup_perf;

	drm_info(&ptdev->base, "Performance counter subsystem initialized");

	ptdev->perf = perf;

	return ret;

cleanup_perf:
	devm_kfree(ptdev->base.dev, perf);

	return ret;
}


static void panthor_perf_fw_request_sample(struct panthor_perf_sampler *sampler)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(sampler->ptdev);

	panthor_fw_toggle_reqs(glb_iface, req, ack, GLB_PERFCNT_SAMPLE);
	gpu_write(sampler->ptdev, CSF_DOORBELL(CSF_GLB_DOORBELL_ID), 1);
}

/**
 * panthor_perf_sampler_request_clearing - Request a clearing sample.
 * @sampler: Panthor sampler
 *
 * Perform a synchronous sample that gets immediately discarded. This sets a baseline at the point
 * of time a new session is started, to avoid having counters from before the session.
 *
 */
static int panthor_perf_sampler_request_clearing(struct panthor_perf_sampler *sampler)
{
	scoped_guard(mutex, &sampler->sampler_lock) {
		if (!sampler->sample_requested) {
			panthor_perf_fw_request_sample(sampler);
			sampler->sample_requested = true;
		}
	}

	return wait_for_completion_timeout(&sampler->sample_handled,
			msecs_to_jiffies(1000));
}

/**
 * panthor_perf_sampler_request_sample - Request a counter sample for the userspace client.
 * @sampler: Panthor sampler
 * @session: Target session
 *
 * A session that has already requested a sample cannot request another one until the previous
 * sample has been delivered.
 *
 * Return:
 * * %0       - The sample has been requested successfully.
 * * %-EBUSY  - The target session has already requested a sample and has not received it yet.
 */
static int panthor_perf_sampler_request_sample(struct panthor_perf_sampler *sampler,
		struct panthor_perf_session *session)
{
	struct list_head *head;

	reinit_completion(&sampler->sample_handled);

	guard(mutex)(&sampler->sampler_lock);

	/*
	 * If a previous sample has not been handled yet, the session cannot request another
	 * sample. If this happens too often, the requested sample rate is too high.
	 */
	list_for_each(head, &sampler->sampler_list) {
		struct panthor_perf_session *cur_session = list_entry(head,
				typeof(*cur_session), waiting);

		if (session == cur_session)
			return -EBUSY;
	}

	if (list_empty(&sampler->sampler_list) && !sampler->sample_requested)
		panthor_perf_fw_request_sample(sampler);

	sampler->sample_requested = true;
	list_add_tail(&session->waiting, &sampler->sampler_list);
	session_get(session);

	return 0;
}

static int session_validate_set(u8 set)
{
	if (set > DRM_PANTHOR_PERF_SET_TERTIARY)
		return -EINVAL;

	if (set == DRM_PANTHOR_PERF_SET_PRIMARY)
		return 0;

	if (set > DRM_PANTHOR_PERF_SET_PRIMARY)
		return capable(CAP_PERFMON) ? 0 : -EACCES;

	return -EINVAL;
}

/**
 * panthor_perf_session_setup - Create a user-visible session.
 *
 * @ptdev: Handle to the panthor device.
 * @perf: Handle to the perf control structure.
 * @setup_args: Setup arguments passed in via ioctl.
 * @pfile: Panthor file associated with the request.
 *
 * Creates a new session associated with the session ID returned. When initialized, the
 * session must explicitly request sampling to start with a successive call to PERF_CONTROL.START.
 *
 * Return: non-negative session identifier on success or negative error code on failure.
 */
int panthor_perf_session_setup(struct panthor_device *ptdev, struct panthor_perf *perf,
		struct drm_panthor_perf_cmd_setup *setup_args,
		struct panthor_file *pfile)
{
	struct panthor_perf_session *session;
	struct drm_gem_object *ringbuffer;
	struct drm_gem_object *control;
	const size_t slots = setup_args->sample_slots;
	struct panthor_perf_enable_masks *em;
	struct iosys_map rb_map, ctrl_map;
	size_t user_sample_size;
	int session_id;
	int ret;

	ret = session_validate_set(setup_args->block_set);
	if (ret)
		return ret;

	session = devm_kzalloc(ptdev->base.dev, sizeof(*session), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(session))
		return -ENOMEM;

	ringbuffer = drm_gem_object_lookup(pfile->drm_file, setup_args->ringbuf_handle);
	if (!ringbuffer) {
		ret = -EINVAL;
		goto cleanup_session;
	}

	control = drm_gem_object_lookup(pfile->drm_file, setup_args->control_handle);
	if (!control) {
		ret = -EINVAL;
		goto cleanup_ringbuf;
	}

	user_sample_size = session_get_max_sample_size(&ptdev->perf_info) * slots;

	if (ringbuffer->size != PFN_ALIGN(user_sample_size)) {
		ret = -ENOMEM;
		goto cleanup_control;
	}

	ret = drm_gem_vmap_unlocked(ringbuffer, &rb_map);
	if (ret)
		goto cleanup_control;


	ret = drm_gem_vmap_unlocked(control, &ctrl_map);
	if (ret)
		goto cleanup_ring_map;

	session->eventfd = eventfd_ctx_fdget(setup_args->fd);
	if (IS_ERR_OR_NULL(session->eventfd)) {
		ret = PTR_ERR_OR_ZERO(session->eventfd) ?: -EINVAL;
		goto cleanup_control_map;
	}

	em = panthor_perf_create_em(setup_args);
	if (IS_ERR_OR_NULL(em)) {
		ret = -ENOMEM;
		goto cleanup_eventfd;
	}

	ret = panthor_perf_sampler_add(&perf->sampler, em, setup_args->block_set);
	if (ret)
		goto cleanup_em;

	INIT_LIST_HEAD(&session->waiting);

	session->extract_idx = ctrl_map.vaddr;
	*session->extract_idx = 0;
	session->insert_idx = session->extract_idx + 1;
	*session->insert_idx = 0;

	session->samples = rb_map.vaddr;

	/* TODO This will need validation when we support periodic sampling sessions */
	if (setup_args->sample_freq_ns) {
		ret = -EOPNOTSUPP;
		goto cleanup_em;
	}

	session->sample_freq_ns = setup_args->sample_freq_ns;
	session->user_sample_size = user_sample_size;
	session->enabled_counters = em;
	session->ring_buf = ringbuffer;
	session->control_buf = control;
	session->pfile = pfile;

	ret = xa_alloc_cyclic(&perf->sessions, &session_id, session, perf->session_range,
			&perf->next_session, GFP_KERNEL);
	if (ret < 0)
		goto cleanup_sampler_add;

	kref_init(&session->ref);

	return session_id;

cleanup_sampler_add:
	panthor_perf_sampler_remove(&perf->sampler, em);

cleanup_em:
	kref_put(&em->refs, panthor_perf_destroy_em_kref);

cleanup_eventfd:
	eventfd_ctx_put(session->eventfd);

cleanup_control_map:
	drm_gem_vunmap_unlocked(control, &ctrl_map);

cleanup_ring_map:
	drm_gem_vunmap_unlocked(ringbuffer, &rb_map);

cleanup_control:
	drm_gem_object_put(control);

cleanup_ringbuf:
	drm_gem_object_put(ringbuffer);

cleanup_session:
	devm_kfree(ptdev->base.dev, session);

	return ret;
}

static int session_stop(struct panthor_perf *perf, struct panthor_perf_session *session,
		u64 user_data)
{
	int ret;

	if (!test_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state))
		return 0;

	const u32 extract_idx = session_read_extract_idx(session);
	const u32 insert_idx = session_read_insert_idx(session);

	/* Must have at least one slot remaining in the ringbuffer to sample. */
	if (WARN_ON_ONCE(!CIRC_SPACE_TO_END(insert_idx, extract_idx, session->ringbuf_slots)))
		return -EBUSY;

	session->user_data = user_data;

	ret = panthor_perf_sampler_request_sample(&perf->sampler, session);
	if (ret)
		return ret;

	clear_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state);

	/* TODO Calls to the FW interface will go here in later patches. */
	return 0;
}

static int session_start(struct panthor_perf *perf, struct panthor_perf_session *session,
		u64 user_data)
{
	if (test_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state))
		return 0;

	set_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state);

	/*
	 * For manual sampling sessions, a start command does not correspond to a sample,
	 * and so the user data gets discarded.
	 */
	if (session->sample_freq_ns)
		session->user_data = user_data;

	return panthor_perf_sampler_request_clearing(&perf->sampler);
}

static int session_sample(struct panthor_perf *perf, struct panthor_perf_session *session,
		u64 user_data)
{
	if (!test_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state))
		return -EACCES;

	const u32 extract_idx = session_read_extract_idx(session);
	const u32 insert_idx = session_read_insert_idx(session);

	/* Manual sampling for periodic sessions is forbidden. */
	if (session->sample_freq_ns)
		return -EINVAL;

	/*
	 * Must have at least two slots remaining in the ringbuffer to sample: one for
	 * the current sample, and one for a stop sample, since a stop command should
	 * always be acknowledged by taking a final sample and stopping the session.
	 */
	if (CIRC_SPACE_TO_END(insert_idx, extract_idx, session->ringbuf_slots) < 2)
		return -EBUSY;

	session->sample_start_ns = ktime_get_raw_ns();
	session->user_data = user_data;

	return panthor_perf_sampler_request_sample(&perf->sampler, session);
}

static int session_destroy(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	int ret = panthor_perf_sampler_remove(&perf->sampler, session->enabled_counters);

	session_put(session);

	return ret;
}

static int session_teardown(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	if (test_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state))
		return -EINVAL;

	if (!list_empty(&session->waiting))
		return -EBUSY;

	return session_destroy(perf, session);
}

/**
 * panthor_perf_session_teardown - Teardown the session associated with the @sid.
 * @pfile: Open panthor file.
 * @perf: Handle to the perf control structure.
 * @sid: Session identifier.
 *
 * Destroys a stopped session where the last sample has been explicitly consumed
 * or discarded. Active sessions will be ignored.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_teardown(struct panthor_file *pfile, struct panthor_perf *perf, u32 sid)
{
	int err;
	struct panthor_perf_session *session;

	xa_lock(&perf->sessions);
	session = __xa_store(&perf->sessions, sid, NULL, GFP_KERNEL);

	if (xa_is_err(session)) {
		err = xa_err(session);
		goto restore;
	}

	if (session->pfile != pfile) {
		err = -EINVAL;
		goto restore;
	}

	session_get(session);
	xa_unlock(&perf->sessions);

	err = session_teardown(perf, session);

	session_put(session);

	return err;

restore:
	__xa_store(&perf->sessions, sid, session, GFP_KERNEL);
	xa_unlock(&perf->sessions);

	return err;
}

/**
 * panthor_perf_session_start - Start sampling on a stopped session.
 * @pfile: Open panthor file.
 * @perf: Handle to the panthor perf control structure.
 * @sid: Session identifier for the desired session.
 * @user_data: An opaque value passed in from userspace.
 *
 * A session counts as stopped when it is created or when it is explicitly stopped after being
 * started. Starting an active session is treated as a no-op.
 *
 * The @user_data parameter will be associated with all subsequent samples for a periodic
 * sampling session and will be ignored for manual sampling ones in favor of the user data
 * passed in the PERF_CONTROL.SAMPLE ioctl call.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_start(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data)
{
	struct panthor_perf_session *session = session_find(pfile, perf, sid);
	int err;

	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_start(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_session_stop - Stop sampling on an active session.
 * @pfile: Open panthor file.
 * @perf: Handle to the panthor perf control structure.
 * @sid: Session identifier for the desired session.
 * @user_data: An opaque value passed in from userspace.
 *
 * A session counts as active when it has been explicitly started via the PERF_CONTROL.START
 * ioctl. Stopping a stopped session is treated as a no-op.
 *
 * To ensure data is not lost when sampling is stopping, there must always be at least one slot
 * available for the final automatic sample, and the stop command will be rejected if there is not.
 *
 * The @user_data will always be associated with the final sample.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_session_stop(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data)
{
	struct panthor_perf_session *session = session_find(pfile, perf, sid);
	int err;

	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_stop(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_session_sample - Request a sample on a manual sampling session.
 * @pfile: Open panthor file.
 * @perf: Handle to the panthor perf control structure.
 * @sid: Session identifier for the desired session.
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
int panthor_perf_session_sample(struct panthor_file *pfile, struct panthor_perf *perf,
		u32 sid, u64 user_data)
{
	struct panthor_perf_session *session = session_find(pfile, perf, sid);
	int err;

	if (IS_ERR_OR_NULL(session))
		return IS_ERR(session) ? PTR_ERR(session) : -EINVAL;

	err = session_sample(perf, session, user_data);

	session_put(session);

	return err;
}

/**
 * panthor_perf_session_destroy - Destroy a sampling session associated with the @pfile.
 * @perf: Handle to the panthor perf control structure.
 * @pfile: The file being closed.
 *
 * Must be called when the corresponding userspace process is destroyed and cannot close its
 * own sessions. As such, we offer no guarantees about data delivery.
 */
void panthor_perf_session_destroy(struct panthor_file *pfile, struct panthor_perf *perf)
{
	unsigned long sid;
	struct panthor_perf_session *session;

	xa_for_each(&perf->sessions, sid, session)
	{
		if (session->pfile == pfile) {
			session_destroy(perf, session);
			xa_erase(&perf->sessions, sid);
		}
	}
}

/**
 * panthor_perf_unplug - Terminate the performance counter subsystem.
 * @ptdev: Panthor device.
 *
 * This function will terminate the performance counter control structures and any remaining
 * sessions, after waiting for any pending interrupts.
 */
void panthor_perf_unplug(struct panthor_device *ptdev)
{
	struct panthor_perf *perf = ptdev->perf;

	if (!perf)
		return;

	if (!xa_empty(&perf->sessions)) {
		unsigned long sid;
		struct panthor_perf_session *session;

		drm_err(&ptdev->base,
				"Performance counter sessions active when unplugging the driver!");

		xa_for_each(&perf->sessions, sid, session)
			session_destroy(perf, session);
	}

	xa_destroy(&perf->sessions);

	panthor_perf_sampler_term(&perf->sampler);

	devm_kfree(ptdev->base.dev, ptdev->perf);

	ptdev->perf = NULL;
}
