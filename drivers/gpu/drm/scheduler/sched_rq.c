#include <linux/rbtree.h>

#include <drm/drm_print.h>
#include <drm/gpu_scheduler.h>

static __always_inline bool drm_sched_entity_compare_before(struct rb_node *a,
							    const struct rb_node *b)
{
	struct drm_sched_entity *ent_a =  rb_entry((a), struct drm_sched_entity, rb_tree_node);
	struct drm_sched_entity *ent_b =  rb_entry((b), struct drm_sched_entity, rb_tree_node);

	return ktime_before(ent_a->oldest_job_waiting, ent_b->oldest_job_waiting);
}

static void __drm_sched_rq_remove_tree_locked(struct drm_sched_entity *entity,
					      struct drm_sched_rq *rq)
{
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	rb_erase_cached(&entity->rb_tree_node, &rq->rb_tree_root);
	RB_CLEAR_NODE(&entity->rb_tree_node);
}

static void __drm_sched_rq_add_tree_locked(struct drm_sched_entity *entity,
					   struct drm_sched_rq *rq,
					   ktime_t ts)
{
	/*
	 * Both locks need to be grabbed, one to protect from entity->rq change
	 * for entity from within concurrent drm_sched_entity_select_rq and the
	 * other to update the rb tree structure.
	 */
	lockdep_assert_held(&entity->lock);
	lockdep_assert_held(&rq->lock);

	entity->oldest_job_waiting = ts;
	rb_add_cached(&entity->rb_tree_node, &rq->rb_tree_root,
		      drm_sched_entity_compare_before);
}

/**
 * drm_sched_rq_init - initialize a given run queue struct
 *
 * @sched: scheduler instance to associate with this run queue
 * @rq: scheduler run queue
 *
 * Initializes a scheduler runqueue.
 */
void drm_sched_rq_init(struct drm_gpu_scheduler *sched,
		       struct drm_sched_rq *rq)
{
	spin_lock_init(&rq->lock);
	INIT_LIST_HEAD(&rq->entities);
	rq->rb_tree_root = RB_ROOT_CACHED;
	rq->sched = sched;
}

/**
 * drm_sched_rq_add_entity - add an entity
 *
 * @rq: scheduler run queue
 * @entity: scheduler entity
 * @ts: submission timestamp
 *
 * Adds a scheduler entity to the run queue.
 *
 * Returns a DRM scheduler pre-selected to handle this entity.
 */
struct drm_gpu_scheduler *
drm_sched_rq_add_entity(struct drm_sched_rq *rq,
			struct drm_sched_entity *entity,
			ktime_t ts)
{
	struct drm_gpu_scheduler *sched;

	if (entity->stopped) {
		DRM_ERROR("Trying to push to a killed entity\n");
		return NULL;
	}

	spin_lock(&entity->lock);
	spin_lock(&rq->lock);

	sched = rq->sched;

	if (!list_empty(&entity->list)) {
		atomic_inc(sched->score);
		list_add_tail(&entity->list, &rq->entities);
	}

	if (!RB_EMPTY_NODE(&entity->rb_tree_node))
		__drm_sched_rq_remove_tree_locked(entity, rq);
	__drm_sched_rq_add_tree_locked(entity, rq, ts);

	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);

	return sched;
}

/**
 * drm_sched_rq_remove_entity - remove an entity
 *
 * @rq: scheduler run queue
 * @entity: scheduler entity
 *
 * Removes a scheduler entity from the run queue.
 */
void drm_sched_rq_remove_entity(struct drm_sched_rq *rq,
				struct drm_sched_entity *entity)
{
	lockdep_assert_held(&entity->lock);

	if (list_empty(&entity->list))
		return;

	spin_lock(&rq->lock);

	atomic_dec(rq->sched->score);
	list_del_init(&entity->list);

	if (!RB_EMPTY_NODE(&entity->rb_tree_node))
		__drm_sched_rq_remove_tree_locked(entity, rq);

	spin_unlock(&rq->lock);
}

void drm_sched_rq_pop_entity(struct drm_sched_rq *rq,
			     struct drm_sched_entity *entity)
{
	struct drm_sched_job *next_job;

	spin_lock(&entity->lock);
	spin_lock(&rq->lock);
	__drm_sched_rq_remove_tree_locked(entity, rq);
	next_job = to_drm_sched_job(spsc_queue_peek(&entity->job_queue));
	if (next_job) {
		ktime_t ts;

		ts = drm_sched_entity_get_job_deadline(entity, next_job);
		__drm_sched_rq_add_tree_locked(entity, rq, ts);
	}
	spin_unlock(&rq->lock);
	spin_unlock(&entity->lock);
}

/**
 * drm_sched_rq_select_entity - Select an entity which provides a job to run
 *
 * @sched: the gpu scheduler
 * @rq: scheduler run queue to check.
 *
 * Find oldest waiting ready entity.
 *
 * Return an entity if one is found; return an error-pointer (!NULL) if an
 * entity was ready, but the scheduler had insufficient credits to accommodate
 * its job; return NULL, if no ready entity was found.
 */
struct drm_sched_entity *
drm_sched_rq_select_entity(struct drm_gpu_scheduler *sched,
			   struct drm_sched_rq *rq)
{
	struct drm_sched_entity *entity = NULL;
	struct rb_node *rb;

	spin_lock(&rq->lock);
	for (rb = rb_first_cached(&rq->rb_tree_root); rb; rb = rb_next(rb)) {
		entity = rb_entry(rb, struct drm_sched_entity, rb_tree_node);
		if (drm_sched_entity_is_ready(entity))
			break;
		else
			entity = NULL;
	}
	spin_unlock(&rq->lock);

	if (!entity)
		return NULL;

	/*
	 * If scheduler cannot take more jobs signal the caller to not consider
	 * lower priority queues.
	 */
	if (!drm_sched_can_queue(sched, entity))
		return ERR_PTR(-ENOSPC);

	reinit_completion(&entity->entity_idle);

	return entity;
}
