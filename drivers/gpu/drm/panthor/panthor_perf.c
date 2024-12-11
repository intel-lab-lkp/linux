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
#include "panthor_gpu.h"
#include "panthor_perf.h"
#include "panthor_regs.h"

/**
 * PANTHOR_PERF_EM_BITS - Number of bits in a user-facing enable mask. This must correspond
 *                        to the maximum number of counters available for selection on the newest
 *                        Mali GPUs (128 as of the Mali-Gx15).
 */
#define PANTHOR_PERF_EM_BITS (BITS_PER_TYPE(u64) * 2)

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

static size_t session_get_max_sample_size(const struct drm_panthor_perf_info *const info)
{
	const size_t block_size = get_annotated_block_size(info->counters_per_block);
	const size_t block_nr = info->cshw_blocks + info->csg_blocks + info->fw_blocks +
		info->tiler_blocks + info->memsys_blocks + info->shader_blocks;

	return sizeof(struct drm_panthor_perf_sample_header) + (block_size * block_nr);
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

	drm_info(&ptdev->base, "Performance counter subsystem initialized");

	ptdev->perf = perf;

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
		goto cleanup_em;

	kref_init(&session->ref);

	return session_id;

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
	if (!test_bit(PANTHOR_PERF_SESSION_ACTIVE, session->state))
		return 0;

	const u32 extract_idx = session_read_extract_idx(session);
	const u32 insert_idx = session_read_insert_idx(session);

	/* Must have at least one slot remaining in the ringbuffer to sample. */
	if (WARN_ON_ONCE(!CIRC_SPACE_TO_END(insert_idx, extract_idx, session->ringbuf_slots)))
		return -EBUSY;

	session->user_data = user_data;

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

	/* TODO Calls to the FW interface will go here in later patches. */
	return 0;
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

	/* TODO Calls to the FW interface will go here in later patches. */
	return 0;
}

static int session_destroy(struct panthor_perf *perf, struct panthor_perf_session *session)
{
	session_put(session);

	return 0;
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

	devm_kfree(ptdev->base.dev, ptdev->perf);

	ptdev->perf = NULL;
}
