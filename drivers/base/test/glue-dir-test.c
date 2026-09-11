// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the class glue-directory index: a class device whose
 * parent is not itself a class device is placed under a per-parent "glue"
 * directory named after the class, which get_device_parent() finds
 * through a per-class index keyed by the parent kobject.  Every case
 * drives that lookup through plain device registration.
 */

#include <kunit/resource.h>
#include <kunit/test.h>

#include <linux/device.h>
#include <linux/kernfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

static void glue_dev_release(struct device *dev)
{
	kfree(dev);
}

static void glue_dev_unregister(void *data)
{
	device_unregister(data);
}

static void glue_root_unregister(void *data)
{
	root_device_unregister(data);
}

static void glue_class_destroy(void *data)
{
	class_destroy(data);
}

/* Does a child named @name exist under @parent? */
static bool glue_child_visible(struct kobject *parent, const char *name)
{
	struct kernfs_node *kn = kernfs_find_and_get(parent->sd, name);
	bool found = kn;

	kernfs_put(kn);
	return found;
}

/* Freed by glue_dev_release() when the last reference drops. */
static struct device *glue_dev_alloc(struct kunit *test,
				     struct device *parent,
				     const struct class *class,
				     const char *name)
{
	struct device *dev;
	int ret;

	dev = kzalloc_obj(*dev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	device_initialize(dev);
	dev->parent = parent;
	dev->class = class;
	dev->release = glue_dev_release;

	ret = dev_set_name(dev, "%s", name);
	if (ret)
		put_device(dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return dev;
}

/* Register a class device; unregistered again by a deferred kunit action. */
static struct device *glue_dev_add(struct kunit *test, struct device *parent,
				   const struct class *class,
				   const char *name)
{
	struct device *dev = glue_dev_alloc(test, parent, class, name);
	int ret;

	ret = device_add(dev);
	if (ret)
		put_device(dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							glue_dev_unregister,
							dev), 0);
	return dev;
}

static struct class *glue_class(struct kunit *test, const char *name)
{
	struct class *class = class_create(name);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, class);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							glue_class_destroy,
							class), 0);
	return class;
}

static struct device *glue_root(struct kunit *test, const char *name)
{
	struct device *root = root_device_register(name);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, root);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							glue_root_unregister,
							root), 0);
	return root;
}

/* Siblings share one glue dir: the second lookup has to hit the first. */
static void glue_test_reuse(struct kunit *test)
{
	struct device *root, *dev_a, *dev_b;
	struct kobject *glue;
	struct class *class;

	class = glue_class(test, "glue_kunit_reuse");
	root = glue_root(test, "glue_kunit_reuse_root");

	dev_a = glue_dev_add(test, root, class, "reuseA");
	dev_b = glue_dev_add(test, root, class, "reuseB");

	glue = dev_a->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, glue);
	KUNIT_EXPECT_PTR_NE(test, glue, &root->kobj);
	KUNIT_EXPECT_PTR_EQ(test, glue, dev_b->kobj.parent);
	KUNIT_EXPECT_TRUE(test, glue_child_visible(glue, "reuseA"));
	KUNIT_EXPECT_TRUE(test, glue_child_visible(glue, "reuseB"));
}

/* Two classes below one parent: each consults only its own class's index. */
static void glue_test_cross_class(struct kunit *test)
{
	struct class *class_a, *class_b;
	struct device *root, *dev_a, *dev_b;

	class_a = glue_class(test, "glue_kunit_xclass_a");
	class_b = glue_class(test, "glue_kunit_xclass_b");
	root = glue_root(test, "glue_kunit_xclass_root");

	dev_a = glue_dev_add(test, root, class_a, "xclassA");
	dev_b = glue_dev_add(test, root, class_b, "xclassB");

	KUNIT_ASSERT_NOT_NULL(test, dev_a->kobj.parent);
	KUNIT_EXPECT_PTR_NE(test, dev_a->kobj.parent, dev_b->kobj.parent);
}

/* Reap retires the index entry: the next add must not find the freed dir. */
static void glue_test_reap_recreate(struct kunit *test)
{
	struct device *root, *dev;
	struct class *class;

	class = glue_class(test, "glue_kunit_reap");
	root = glue_root(test, "glue_kunit_reap_root");

	dev = glue_dev_add(test, root, class, "reap0");
	KUNIT_EXPECT_TRUE(test, glue_child_visible(&root->kobj,
						   "glue_kunit_reap"));

	/* last child gone: the glue dir goes with it ... */
	kunit_release_action(test, glue_dev_unregister, dev);
	KUNIT_EXPECT_FALSE(test, glue_child_visible(&root->kobj,
						    "glue_kunit_reap"));

	/* ... and a further registration recreates it */
	dev = glue_dev_add(test, root, class, "reap1");
	KUNIT_ASSERT_NOT_NULL(test, dev->kobj.parent);
	KUNIT_EXPECT_TRUE(test, glue_child_visible(dev->kobj.parent, "reap1"));
}

static ssize_t glue_kunit_collide_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return sysfs_emit(buf, "\n");
}
static DEVICE_ATTR_RO(glue_kunit_collide);

/*
 * A same-named child that is not a glue dir must never be taken for one:
 * the colliding add fails on the duplicate sysfs create and leaves
 * nothing stale, so the same registration succeeds once the file is gone.
 */
static void glue_test_name_collision(struct kunit *test)
{
	struct device *root, *dev;
	struct kernfs_node *kn;
	struct class *class;

	class = glue_class(test, "glue_kunit_collide");
	root = glue_root(test, "glue_kunit_collide_root");

	KUNIT_ASSERT_EQ(test,
			device_create_file(root,
					   &dev_attr_glue_kunit_collide), 0);

	dev = glue_dev_alloc(test, root, class, "collide0");
	KUNIT_ASSERT_EQ(test, device_add(dev), -EEXIST);
	put_device(dev);

	/* the same-named child is still the attribute file */
	kn = kernfs_find_and_get(root->kobj.sd, "glue_kunit_collide");
	KUNIT_ASSERT_NOT_NULL(test, kn);
	KUNIT_EXPECT_EQ(test, kernfs_type(kn), KERNFS_FILE);
	kernfs_put(kn);

	device_remove_file(root, &dev_attr_glue_kunit_collide);
	dev = glue_dev_add(test, root, class, "collide0");

	/* and it lands in a glue dir of the class's name, not on the root */
	KUNIT_ASSERT_NOT_NULL(test, dev->kobj.parent);
	KUNIT_EXPECT_PTR_NE(test, dev->kobj.parent, &root->kobj);
	KUNIT_EXPECT_STREQ(test, kobject_name(dev->kobj.parent), class->name);
	KUNIT_EXPECT_PTR_EQ(test, dev->kobj.parent->parent, &root->kobj);
}

/*
 * A parentless class device is keyed on the shared "virtual" kobject
 * rather than on a device, so two of them still share one glue dir.
 */
static void glue_test_virtual_parent(struct kunit *test)
{
	struct device *dev_a, *dev_b;
	struct kobject *glue;
	struct class *class;

	class = glue_class(test, "glue_kunit_virtual");

	dev_a = glue_dev_add(test, NULL, class, "virtA");
	dev_b = glue_dev_add(test, NULL, class, "virtB");

	glue = dev_a->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, glue);
	KUNIT_EXPECT_PTR_EQ(test, glue, dev_b->kobj.parent);
	KUNIT_ASSERT_NOT_NULL(test, glue->parent);
	KUNIT_EXPECT_STREQ(test, kobject_name(glue->parent), "virtual");
}

/*
 * Every class's parentless devices share the one "virtual" key; two
 * classes below it must still land in dirs of their own class's name.
 */
static void glue_test_virtual_cross_class(struct kunit *test)
{
	struct class *class_a, *class_b;
	struct device *dev_a, *dev_b;
	struct kobject *glue_a, *glue_b;

	class_a = glue_class(test, "glue_kunit_virtxc_a");
	class_b = glue_class(test, "glue_kunit_virtxc_b");

	dev_a = glue_dev_add(test, NULL, class_a, "virtxcA");
	dev_b = glue_dev_add(test, NULL, class_b, "virtxcB");

	glue_a = dev_a->kobj.parent;
	glue_b = dev_b->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, glue_a);
	KUNIT_ASSERT_NOT_NULL(test, glue_b);

	KUNIT_EXPECT_PTR_NE(test, glue_a, glue_b);
	KUNIT_EXPECT_STREQ(test, kobject_name(glue_a), class_a->name);
	KUNIT_EXPECT_STREQ(test, kobject_name(glue_b), class_b->name);

	/* both dirs hang off the one shared key */
	KUNIT_ASSERT_NOT_NULL(test, glue_a->parent);
	KUNIT_EXPECT_PTR_EQ(test, glue_a->parent, glue_b->parent);
	KUNIT_EXPECT_STREQ(test, kobject_name(glue_a->parent), "virtual");
}

#define GLUE_MANY_PARENTS 256

/*
 * With hundreds of parents indexed, every lookup must resolve the dir
 * anchored at its own parent -- the case that falsifies the key comparison.
 */
static void glue_test_many_parents(struct kunit *test)
{
	struct device **roots, **devs;
	struct class *class;
	char name[32];
	int i;

	class = glue_class(test, "glue_kunit_many");

	roots = kunit_kcalloc(test, GLUE_MANY_PARENTS, sizeof(*roots),
			      GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, roots);
	devs = kunit_kcalloc(test, GLUE_MANY_PARENTS, sizeof(*devs),
			     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, devs);

	for (i = 0; i < GLUE_MANY_PARENTS; i++) {
		snprintf(name, sizeof(name), "glue_kunit_many_root%d", i);
		roots[i] = glue_root(test, name);
		snprintf(name, sizeof(name), "many%d", i);
		devs[i] = glue_dev_add(test, roots[i], class, name);
	}

	/* Distinctness via anchoring: a kobject has one parent. */
	for (i = 0; i < GLUE_MANY_PARENTS; i++) {
		struct kobject *glue = devs[i]->kobj.parent;

		KUNIT_ASSERT_NOT_NULL(test, glue);
		KUNIT_EXPECT_PTR_EQ(test, glue->parent, &roots[i]->kobj);
	}
}

/*
 * device_move() looks up against the new parent and must leave the old
 * parent's entry behind as a valid hit: the old glue dir is never reaped
 * (base behaviour), so a later device under the old parent reuses it.
 */
static void glue_test_device_move(struct kunit *test)
{
	struct device *root_a, *root_b, *dev, *dev_a2;
	struct kobject *old_glue, *new_glue;
	struct class *class;

	class = glue_class(test, "glue_kunit_move");
	root_a = glue_root(test, "glue_kunit_move_rootA");
	root_b = glue_root(test, "glue_kunit_move_rootB");

	dev = glue_dev_add(test, root_a, class, "move0");
	old_glue = dev->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, old_glue);

	KUNIT_ASSERT_EQ(test, device_move(dev, root_b, DPM_ORDER_NONE), 0);

	new_glue = dev->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, new_glue);
	KUNIT_EXPECT_PTR_NE(test, new_glue, old_glue);
	KUNIT_EXPECT_PTR_EQ(test, new_glue->parent, &root_b->kobj);
	KUNIT_EXPECT_TRUE(test, glue_child_visible(new_glue, "move0"));

	/* nothing reaps the old dir on the success path: it is still there */
	KUNIT_EXPECT_TRUE(test, glue_child_visible(&root_a->kobj,
						   "glue_kunit_move"));

	/* and it must still be the hit for the old parent */
	dev_a2 = glue_dev_add(test, root_a, class, "move1");
	KUNIT_EXPECT_PTR_EQ(test, dev_a2->kobj.parent, old_glue);
}

/*
 * Removing one of two siblings must not retire the shared glue dir:
 * the survivor keeps it visible, and a further sibling still reuses
 * it -- a spurious unindex would fail that add on a duplicate create.
 */
static void glue_test_no_reap(struct kunit *test)
{
	struct device *root, *dev_a, *dev_b, *dev_c;
	struct kobject *glue;
	struct class *class;

	class = glue_class(test, "glue_kunit_noreap");
	root = glue_root(test, "glue_kunit_noreap_root");

	dev_a = glue_dev_add(test, root, class, "noreapA");
	dev_b = glue_dev_add(test, root, class, "noreapB");
	glue = dev_b->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, glue);

	kunit_release_action(test, glue_dev_unregister, dev_a);

	KUNIT_EXPECT_TRUE(test, glue_child_visible(&root->kobj,
						   "glue_kunit_noreap"));
	KUNIT_EXPECT_PTR_EQ(test, dev_b->kobj.parent, glue);
	KUNIT_EXPECT_TRUE(test, glue_child_visible(glue, "noreapB"));

	dev_c = glue_dev_add(test, root, class, "noreapC");
	KUNIT_EXPECT_PTR_EQ(test, dev_c->kobj.parent, glue);
}

/*
 * Static so that unregistering under a live device frees only the
 * driver-core generation, not the struct class the device points at.
 */
static const struct class glue_gone_class_a = {
	.name = "glue_kunit_gone_a",
};

static const struct class glue_gone_class_b = {
	.name = "glue_kunit_gone_b",
};

static void glue_static_class_unregister(void *data)
{
	class_unregister(data);
}

/*
 * Unregistering a class under a live device is tolerated API misuse: the
 * device's glue dir can no longer be reaped and is deliberately leaked
 * (class_dir, kernfs node, and parent reference), here as in the base
 * tree; kmemleak reports all three on every run.
 *
 * The case pins the index's placement: a dead generation's tree dies
 * with its subsys_private, so the successor class starts on an empty
 * tree and cannot resolve the leaked dir.  A global index would put the
 * dead entry back in its search path.
 */
static void glue_test_class_gone(struct kunit *test)
{
	struct device *root, *dev_a, *dev_b;
	struct kobject *glue_a;

	KUNIT_ASSERT_EQ(test, class_register(&glue_gone_class_a), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  glue_static_class_unregister,
						  (void *)&glue_gone_class_a),
			0);
	root = glue_root(test, "glue_kunit_gone_root");

	dev_a = glue_dev_add(test, root, &glue_gone_class_a, "goneA");
	glue_a = dev_a->kobj.parent;
	KUNIT_ASSERT_NOT_NULL(test, glue_a);

	/* the misuse: the class goes away under a live device */
	kunit_release_action(test, glue_static_class_unregister,
			     (void *)&glue_gone_class_a);

	KUNIT_ASSERT_EQ(test, class_register(&glue_gone_class_b), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test,
						  glue_static_class_unregister,
						  (void *)&glue_gone_class_b),
			0);

	dev_b = glue_dev_add(test, root, &glue_gone_class_b, "goneB");
	KUNIT_ASSERT_NOT_NULL(test, dev_b->kobj.parent);
	KUNIT_EXPECT_PTR_NE(test, dev_b->kobj.parent, glue_a);
	KUNIT_EXPECT_PTR_NE(test, dev_b->kobj.parent, &root->kobj);
	KUNIT_EXPECT_STREQ(test, kobject_name(dev_b->kobj.parent),
			   glue_gone_class_b.name);
	KUNIT_EXPECT_TRUE(test, glue_child_visible(dev_b->kobj.parent,
						   "goneB"));
}

static struct kunit_case glue_dir_tests[] = {
	KUNIT_CASE(glue_test_reuse),
	KUNIT_CASE(glue_test_cross_class),
	KUNIT_CASE(glue_test_reap_recreate),
	KUNIT_CASE(glue_test_name_collision),
	KUNIT_CASE(glue_test_virtual_parent),
	KUNIT_CASE(glue_test_virtual_cross_class),
	KUNIT_CASE(glue_test_many_parents),
	KUNIT_CASE(glue_test_device_move),
	KUNIT_CASE(glue_test_no_reap),
	KUNIT_CASE(glue_test_class_gone),
	{}
};

static struct kunit_suite glue_dir_test_suite = {
	.name = "glue_dir",
	.test_cases = glue_dir_tests,
};

kunit_test_suite(glue_dir_test_suite);

MODULE_DESCRIPTION("KUnit tests for class glue directories");
MODULE_LICENSE("GPL");
