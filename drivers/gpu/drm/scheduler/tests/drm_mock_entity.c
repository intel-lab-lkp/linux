
#include "drm_sched_tests.h"

struct drm_mock_sched_entity *
drm_mock_sched_entity_new(struct kunit *test,
			  enum drm_sched_priority priority,
			  struct drm_mock_scheduler *sched)
{
	struct drm_sched_mock_entity *entity;
	int ret;

	entity = kunit_kmalloc(test, sizeof(*entity), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, entity);

	ret = drm_sched_entity_init(&entity->base,
				    priority,
				    &sched->base, 1,
				    NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	entity->test = test;

	return entity;
}

void drm_mock_sched_entity_free(struct drm_mock_sched_entity *entity)
{
	drm_sched_entity_fini(&entity->base);
}
