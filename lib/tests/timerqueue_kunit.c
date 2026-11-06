// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the timerqueue in lib/timerqueue.c, both the cached
 * rbtree variant and the linked variant used by hrtimers.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/timerqueue.h>

#define TQ_STRESS_NODES	256

static void timerqueue_empty_test(struct kunit *test)
{
	struct timerqueue_head head;
	struct timerqueue_node node;

	timerqueue_init_head(&head);
	timerqueue_init(&node);

	KUNIT_EXPECT_NULL(test, timerqueue_getnext(&head));
	KUNIT_EXPECT_FALSE(test, timerqueue_node_queued(&node));
	KUNIT_EXPECT_NULL(test, timerqueue_iterate_next(NULL));
}

static void timerqueue_add_del_test(struct kunit *test)
{
	struct timerqueue_head head;
	struct timerqueue_node a, b, c;

	timerqueue_init_head(&head);
	timerqueue_init(&a);
	timerqueue_init(&b);
	timerqueue_init(&c);
	a.expires = 20;
	b.expires = 10;
	c.expires = 30;

	/* timerqueue_add() returns true when the new node is the first. */
	KUNIT_EXPECT_TRUE(test, timerqueue_add(&head, &a));
	KUNIT_EXPECT_TRUE(test, timerqueue_node_queued(&a));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &a);

	KUNIT_EXPECT_TRUE(test, timerqueue_add(&head, &b));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &b);

	KUNIT_EXPECT_FALSE(test, timerqueue_add(&head, &c));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &b);

	/* Iteration is in expiry order. */
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_iterate_next(&b), &a);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_iterate_next(&a), &c);
	KUNIT_EXPECT_NULL(test, timerqueue_iterate_next(&c));

	/* timerqueue_del() returns true while the queue is not empty. */
	KUNIT_EXPECT_TRUE(test, timerqueue_del(&head, &b));
	KUNIT_EXPECT_FALSE(test, timerqueue_node_queued(&b));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &a);

	/* Deleting a node that is not the first leaves the first alone. */
	KUNIT_EXPECT_TRUE(test, timerqueue_del(&head, &c));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &a);
	KUNIT_EXPECT_NULL(test, timerqueue_iterate_next(&a));

	KUNIT_EXPECT_FALSE(test, timerqueue_del(&head, &a));
	KUNIT_EXPECT_NULL(test, timerqueue_getnext(&head));

	/* A deleted node can be queued again. */
	KUNIT_EXPECT_TRUE(test, timerqueue_add(&head, &b));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &b);
	KUNIT_EXPECT_FALSE(test, timerqueue_del(&head, &b));
}

static void timerqueue_equal_expiry_test(struct kunit *test)
{
	struct timerqueue_head head;
	struct timerqueue_node n[3];
	int i;

	timerqueue_init_head(&head);
	for (i = 0; i < ARRAY_SIZE(n); i++) {
		timerqueue_init(&n[i]);
		n[i].expires = 100;
	}

	/*
	 * Nodes with the same expiry time stay in the order they were added,
	 * and only the first one added reports itself as the new first.
	 */
	KUNIT_EXPECT_TRUE(test, timerqueue_add(&head, &n[0]));
	KUNIT_EXPECT_FALSE(test, timerqueue_add(&head, &n[1]));
	KUNIT_EXPECT_FALSE(test, timerqueue_add(&head, &n[2]));

	KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), &n[0]);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_iterate_next(&n[0]), &n[1]);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_iterate_next(&n[1]), &n[2]);

	for (i = 0; i < ARRAY_SIZE(n); i++)
		timerqueue_del(&head, &n[i]);
	KUNIT_EXPECT_NULL(test, timerqueue_getnext(&head));
}

/* Check that the queue holds exactly @count nodes, in expiry order. */
static void timerqueue_check_order(struct kunit *test,
				   struct timerqueue_head *head,
				   unsigned int count)
{
	struct timerqueue_node *node, *prev = NULL;
	unsigned int seen = 0;

	for (node = timerqueue_getnext(head); node;
	     node = timerqueue_iterate_next(node)) {
		if (prev)
			KUNIT_EXPECT_LE(test, prev->expires, node->expires);
		prev = node;
		seen++;
	}
	KUNIT_EXPECT_EQ(test, seen, count);
}

static void timerqueue_stress_test(struct kunit *test)
{
	struct timerqueue_head head;
	struct timerqueue_node *nodes;
	struct rnd_state rnd;
	unsigned int i, left;

	nodes = kunit_kcalloc(test, TQ_STRESS_NODES, sizeof(*nodes), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nodes);

	prandom_seed_state(&rnd, 42);
	timerqueue_init_head(&head);

	for (i = 0; i < TQ_STRESS_NODES; i++) {
		struct timerqueue_node *first = timerqueue_getnext(&head);
		bool expect_first;

		timerqueue_init(&nodes[i]);
		/* A small range, so that some expiry times are equal. */
		nodes[i].expires = prandom_u32_state(&rnd) % 64;
		expect_first = !first || nodes[i].expires < first->expires;
		KUNIT_EXPECT_EQ(test, timerqueue_add(&head, &nodes[i]),
				expect_first);
	}
	timerqueue_check_order(test, &head, TQ_STRESS_NODES);

	/* Remove the odd nodes, then drain the queue from the front. */
	left = TQ_STRESS_NODES;
	for (i = 1; i < TQ_STRESS_NODES; i += 2) {
		left--;
		KUNIT_EXPECT_EQ(test, timerqueue_del(&head, &nodes[i]),
				left != 0);
	}
	timerqueue_check_order(test, &head, left);

	while (left) {
		struct timerqueue_node *first = timerqueue_getnext(&head);
		struct timerqueue_node *next;

		KUNIT_ASSERT_NOT_NULL(test, first);
		next = timerqueue_iterate_next(first);
		left--;
		KUNIT_EXPECT_EQ(test, timerqueue_del(&head, first), left != 0);
		KUNIT_EXPECT_PTR_EQ(test, timerqueue_getnext(&head), next);
	}
	KUNIT_EXPECT_NULL(test, timerqueue_getnext(&head));
}

static void timerqueue_linked_test(struct kunit *test)
{
	struct timerqueue_linked_head head;
	struct timerqueue_linked_node a, b, c;

	timerqueue_linked_init_head(&head);
	timerqueue_linked_init(&a);
	timerqueue_linked_init(&b);
	timerqueue_linked_init(&c);
	a.expires = 20;
	b.expires = 10;
	c.expires = 30;

	KUNIT_EXPECT_NULL(test, timerqueue_linked_first(&head));
	KUNIT_EXPECT_FALSE(test, timerqueue_linked_node_queued(&a));

	KUNIT_EXPECT_TRUE(test, timerqueue_linked_add(&head, &a));
	KUNIT_EXPECT_TRUE(test, timerqueue_linked_node_queued(&a));
	KUNIT_EXPECT_NULL(test, timerqueue_linked_prev(&a));
	KUNIT_EXPECT_NULL(test, timerqueue_linked_next(&a));

	KUNIT_EXPECT_TRUE(test, timerqueue_linked_add(&head, &b));
	KUNIT_EXPECT_FALSE(test, timerqueue_linked_add(&head, &c));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_first(&head), &b);

	/* The prev/next links follow expiry order in both directions. */
	KUNIT_EXPECT_NULL(test, timerqueue_linked_prev(&b));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_next(&b), &a);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_prev(&a), &b);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_next(&a), &c);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_prev(&c), &a);
	KUNIT_EXPECT_NULL(test, timerqueue_linked_next(&c));

	/* Deleting from the middle relinks the neighbours. */
	KUNIT_EXPECT_TRUE(test, timerqueue_linked_del(&head, &a));
	KUNIT_EXPECT_FALSE(test, timerqueue_linked_node_queued(&a));
	KUNIT_EXPECT_NULL(test, timerqueue_linked_prev(&a));
	KUNIT_EXPECT_NULL(test, timerqueue_linked_next(&a));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_next(&b), &c);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_prev(&c), &b);

	/* Deleting the first node moves the head to the next one. */
	KUNIT_EXPECT_TRUE(test, timerqueue_linked_del(&head, &b));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_first(&head), &c);
	KUNIT_EXPECT_NULL(test, timerqueue_linked_prev(&c));

	/* Re-adding a deleted node with a later expiry links it last. */
	a.expires = 40;
	KUNIT_EXPECT_FALSE(test, timerqueue_linked_add(&head, &a));
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_next(&c), &a);
	KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_prev(&a), &c);

	KUNIT_EXPECT_TRUE(test, timerqueue_linked_del(&head, &c));
	KUNIT_EXPECT_FALSE(test, timerqueue_linked_del(&head, &a));
	KUNIT_EXPECT_NULL(test, timerqueue_linked_first(&head));
}

static void timerqueue_linked_stress_test(struct kunit *test)
{
	struct timerqueue_linked_head head;
	struct timerqueue_linked_node *nodes, *node, *prev;
	struct rnd_state rnd;
	unsigned int i, seen;

	nodes = kunit_kcalloc(test, TQ_STRESS_NODES, sizeof(*nodes), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, nodes);

	prandom_seed_state(&rnd, 7);
	timerqueue_linked_init_head(&head);

	for (i = 0; i < TQ_STRESS_NODES; i++) {
		struct timerqueue_linked_node *first;
		bool expect_first;

		first = timerqueue_linked_first(&head);
		timerqueue_linked_init(&nodes[i]);
		nodes[i].expires = prandom_u32_state(&rnd) % 64;
		expect_first = !first || nodes[i].expires < first->expires;
		KUNIT_EXPECT_EQ(test, timerqueue_linked_add(&head, &nodes[i]),
				expect_first);
	}

	/* Remove every third node to exercise relinking. */
	for (i = 0; i < TQ_STRESS_NODES; i += 3)
		timerqueue_linked_del(&head, &nodes[i]);

	/* Walk forward checking order and that each prev link points back. */
	seen = 0;
	prev = NULL;
	for (node = timerqueue_linked_first(&head); node;
	     node = timerqueue_linked_next(node)) {
		KUNIT_EXPECT_PTR_EQ(test, timerqueue_linked_prev(node), prev);
		if (prev)
			KUNIT_EXPECT_LE(test, prev->expires, node->expires);
		prev = node;
		seen++;
	}
	KUNIT_EXPECT_EQ(test, seen, TQ_STRESS_NODES - (TQ_STRESS_NODES + 2) / 3);

	for (i = 0; i < TQ_STRESS_NODES; i++)
		if (timerqueue_linked_node_queued(&nodes[i]))
			timerqueue_linked_del(&head, &nodes[i]);
	KUNIT_EXPECT_NULL(test, timerqueue_linked_first(&head));
}

static struct kunit_case timerqueue_test_cases[] = {
	KUNIT_CASE(timerqueue_empty_test),
	KUNIT_CASE(timerqueue_add_del_test),
	KUNIT_CASE(timerqueue_equal_expiry_test),
	KUNIT_CASE(timerqueue_stress_test),
	KUNIT_CASE(timerqueue_linked_test),
	KUNIT_CASE(timerqueue_linked_stress_test),
	{}
};

static struct kunit_suite timerqueue_test_suite = {
	.name = "timerqueue",
	.test_cases = timerqueue_test_cases,
};

kunit_test_suite(timerqueue_test_suite);

MODULE_DESCRIPTION("KUnit tests for the timerqueue");
MODULE_LICENSE("GPL");
