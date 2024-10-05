// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/union_find.h>

static void test_union_and_find(struct kunit *test)
{
	struct uf_node node1, node2, node3;
	struct uf_node *root1, *root2, *root3;
	bool merged;

	/* Initialize the nodes */
	uf_node_init(&node1);
	uf_node_init(&node2);
	uf_node_init(&node3);

	/* Check the initial parent and rank */
	KUNIT_ASSERT_PTR_EQ(test, uf_find(&node1), &node1);
	KUNIT_ASSERT_PTR_EQ(test, uf_find(&node2), &node2);
	KUNIT_ASSERT_PTR_EQ(test, uf_find(&node3), &node3);
	KUNIT_ASSERT_EQ(test, node1.rank, 0);
	KUNIT_ASSERT_EQ(test, node2.rank, 0);
	KUNIT_ASSERT_EQ(test, node3.rank, 0);

	/* Union node1 and node2 */
	merged = uf_union(&node1, &node2);
	KUNIT_ASSERT_TRUE(test, merged);

	/* Assert that one of the nodes is now the parent of the other */
	root1 = uf_find(&node1);
	root2 = uf_find(&node2);
	KUNIT_ASSERT_PTR_EQ(test, root1, root2);

	/* Check rank after the first union */
	if (root1 == &node1) {
		KUNIT_ASSERT_EQ(test, node1.rank, 1);
		KUNIT_ASSERT_EQ(test, node2.rank, 0);
	} else {
		KUNIT_ASSERT_EQ(test, node1.rank, 0);
		KUNIT_ASSERT_EQ(test, node2.rank, 1);
	}

	/* Attempt to union node1 and node2 again and check for false return */
	merged = uf_union(&node1, &node2);
	KUNIT_ASSERT_FALSE(test, merged);

	/* Union node3 with the result of the previous union (node1 and node2) */
	uf_union(&node1, &node3);

	/* Assert that all nodes have the same root */
	root3 = uf_find(&node3);
	KUNIT_ASSERT_PTR_EQ(test, root1, root3);

	/* Check rank after the second union */
	KUNIT_ASSERT_EQ(test, root1->rank, 1);
	KUNIT_ASSERT_EQ(test, node3.rank, 0);
}

static struct kunit_case union_find_test_cases[] = {
	KUNIT_CASE(test_union_and_find),
	{}
};

static struct kunit_suite union_find_test_suite = {
	.name = "union_find_test_suite",
	.test_cases = union_find_test_cases,
};

kunit_test_suites(&union_find_test_suite);

MODULE_AUTHOR("Kuan-Wei Chiu <visitorckw@gmail.com>");
MODULE_DESCRIPTION("Union-find KUnit test suite");
MODULE_LICENSE("GPL");
