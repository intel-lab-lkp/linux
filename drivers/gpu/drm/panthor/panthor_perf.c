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
#include "panthor_gpu.h"
#include "panthor_gpu_regs.h"
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

struct panthor_perf_bo {
	struct drm_gem_object *obj;
	void *vaddr;
};

struct panthor_perf_session {
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

	/**
	 * @op_lock: Serializes session operations. Acquire before looking up session
	 *           handles and before a session's state_lock.
	 */
	struct mutex op_lock;
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
	int ret = 0;

	if (!ptdev)
		return -EINVAL;

	panthor_perf_info_init(ptdev);

	perf = drmm_kzalloc(&ptdev->base, sizeof(*perf), GFP_KERNEL);
	if (!perf)
		return -ENOMEM;

	ret = drmm_mutex_init(&ptdev->base, &perf->op_lock);
	if (ret)
		return ret;

	drm_info(&ptdev->base, "Performance counter subsystem initialized");

	perf->info = &ptdev->perf_info;
	ptdev->perf = perf;

	return ret;
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
	mutex_init(&session->state_lock);
	session->control = control.vaddr;
	*session->control = (struct drm_panthor_perf_ringbuf_control) { 0 };
	session->samples = ringbuffer.vaddr;
	session->enabled_counters = em;
	session->ring_buf = ringbuffer;
	session->ringbuf_slots = slots;
	session->control_buf = control;
	session->accum_idx = U32_MAX;

	ret = xa_alloc_cyclic(&pfile->sessions->xa, &session_id, session,
			      pfile->sessions->session_range,
			      &pfile->sessions->next, GFP_KERNEL);
	if (ret < 0) {
		drm_err(&ptdev->base, "File session limit exceeded.\n");
		ret = -EBUSY;
		goto cleanup_obj;
	}

	*sid = session_id;
	return 0;

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

	if (session_is_stopping(session))
		return 1;

	session_set_state(session, PANTHOR_PERF_SESSION_STOPPING);
	session->user_data = user_data;

	return 1;
}

static int session_stop(struct panthor_perf *perf, struct panthor_perf_session *session,
			u64 user_data)
{
	int ret;

	ret = session_try_stop(session, user_data);
	if (ret <= 0)
		return ret;

	/* TODO Calls to the FW interface will go here in later patches. */
	return 0;
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
	session->user_data = user_data;

	return 1;
}

static int session_start(struct panthor_perf *perf, struct panthor_perf_session *session,
			 u64 user_data)
{
	int ret;

	ret = session_try_start(session, user_data);
	if (ret <= 0)
		return ret;

	/* TODO Calls to the FW interface will go here in later patches. */
	return 0;
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

	session->user_data = user_data;

	return 1;
}

static int session_sample(struct panthor_perf *perf, struct panthor_perf_session *session,
			  u64 user_data)
{

	int ret;

	ret = session_try_sample(session, user_data);
	if (ret <= 0)
		return ret;

	/* TODO Calls to the FW interface will go here in later patches. */
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
	}

	session_put(session);

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
	struct panthor_perf *perf = ptdev->perf;

	if (!perf)
		return;

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

	xa_for_each(&pool->xa, handle, session) {
		xa_erase(&pool->xa, handle);
		session_put(session);
	}

	xa_destroy(&pool->xa);
	kfree(pool);

	pfile->sessions = NULL;
}
