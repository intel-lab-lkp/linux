#include <test_progs.h>

#include "uptr_kv_store.h"
#include "test_uptr_kv_store_common.h"
#include "test_uptr_kv_store.skel.h"
#include "test_uptr_kv_store_v1.skel.h"

static void test_uptr_kv_store_basic(void)
{
	int err, i, pid, zero = 0, *int_val_p, max_int_entries;
	struct test_uptr_kv_store *skel;
	struct kv_store *kvs = NULL;

	skel = test_uptr_kv_store__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel_open_and_load"))
		return;

	skel->bss->target_pid = -1;
	err = test_uptr_kv_store__attach(skel);
	if (!ASSERT_OK(err, "skel_attach"))
		return;

	kvs = kv_store_init(getpid(), skel->maps.data_map, "/sys/fs/bpf/kv_store_data_map", NULL);
	if (!ASSERT_OK_PTR(kvs, "kv_store_init"))
		return;

	max_int_entries = KVS_MAX_VAL_ENTRIES;

	err = kv_store_set(kvs, 0, &zero, KVS_MAX_VAL_SIZE + 1);
	ASSERT_EQ(err, -E2BIG, "kv_store_set(kvs, 0, &zero, 4097)");

	err = kv_store_set(kvs, max_int_entries, &zero, sizeof(int));
	ASSERT_EQ(err, -ENOENT, "kv_store_set(kvs, 1024, &zero, 4)");

	for (i = 0; i < max_int_entries; i++) {
		int_val_p = kv_store_get(kvs, i);
		if (!ASSERT_ERR_PTR(int_val_p, "kv_store_get(kvs, i)"))
			goto out;

		err = kv_store_set(kvs, i, &i, sizeof(i));
		if (!ASSERT_OK(err, "kv_store_set(kvs, i)"))
			goto out;
	}

	pid = sys_gettid();
	skel->bss->target_pid = pid;
	for (i = 0; i < max_int_entries; i++) {
		skel->bss->test_key = i;
		skel->bss->test_op = KVS_INT_GET;
		sys_gettid();
		ASSERT_EQ(skel->bss->test_int_val, i, "bpf:value[i]");

		skel->bss->test_int_val += 1;
		skel->bss->test_op = KVS_INT_SET;
		sys_gettid();
		skel->bss->test_int_val = 0;
	}
	skel->bss->target_pid = -1;

	for (i = 0; i < max_int_entries; i++) {
		int_val_p = kv_store_get(kvs, i);
		if (!ASSERT_OK_PTR(int_val_p, "kv_store_get(kvs, i)"))
			goto out;

		ASSERT_EQ(*int_val_p, i + 1, "userspace:value[i]");
	}

out:
	kv_store_close(kvs);
}

static void test_uptr_kv_store_update(void)
{
	struct test_struct val2 = {.a = 1, .b = 2};
	struct kv_pair kvp_array[5] = {
		{.key = 0, .val = NULL, .size = sizeof(int)},
		{.key = 1, .val = NULL, .size = sizeof(int)},
		{.key = 2, .val = &val2, .size = sizeof(struct test_struct)},
		{.key = 3, .val = NULL, .size = sizeof(struct test_struct)},
		{.key = 4, .val = NULL, .size = sizeof(int)},
	};
	struct test_struct_v1 val2_v1 = {.a = 3, .b = 4, .c = 5};
	int val4 = 1234;
	struct kv_pair kvp_array_v1[5] = {
		{.key = 0, .val = NULL, .size = sizeof(int)},
		{.key = 2, .val = &val2_v1, .size = sizeof(struct test_struct_v1)},
		{.key = 4, .val = &val4, .size = sizeof(int)},
		{.key = 6, .val = NULL, .size = sizeof(struct test_struct_v1)},
		{.key = 8, .val = NULL, .size = sizeof(int)},
	};
	struct test_uptr_kv_store_v1 *skel_v1;
	struct test_uptr_kv_store *skel;
	struct kv_pairs *kvp, *kvp_v1;
	struct kv_store *kvs = NULL;
	int err, pid, val;
	void *val_p;

	kvp = malloc(sizeof(struct kv_pairs) + sizeof(struct kv_pair) * 5);
	if (!ASSERT_OK_PTR(kvp, "malloc kvp"))
		goto out;
	kvp->array_cnt = 5;
	memcpy(&kvp->array, &kvp_array, sizeof(struct kv_pair) * 5);

	kvp_v1 = malloc(sizeof(struct kv_pairs) + sizeof(struct kv_pair) * 5);
	if (!ASSERT_OK_PTR(kvp_v1, "malloc kvp_v1"))
		goto out;
	kvp_v1->array_cnt = 5;
	memcpy(&kvp_v1->array, &kvp_array_v1, sizeof(struct kv_pair) * 5);

	/* Rollout the initial version */
	skel = test_uptr_kv_store__open_and_load();
	if (!ASSERT_OK_PTR(skel, "skel_open_and_load"))
		return;

	pid = sys_gettid();
	kvs = kv_store_init(pid, skel->maps.data_map, "/sys/fs/bpf/kv_store_data_map", kvp);
	if (!ASSERT_OK_PTR(kvs, "kv_store_init"))
		goto out;

	skel->bss->target_pid = -1;
	err = test_uptr_kv_store__attach(skel);
	if (!ASSERT_OK(err, "skel_attach"))
		return;

	/* Check if the KV store is initialized correctly */
	val_p = kv_store_get(kvs, 0);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 0)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 0, "value[0]");

	val_p = kv_store_get(kvs, 1);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 1)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 0, "value[1]");

	val_p = kv_store_get(kvs, 2);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 2)"))
		goto out;
	ASSERT_EQ(((struct test_struct *)val_p)->a, 1, "value[2].a");
	ASSERT_EQ(((struct test_struct *)val_p)->b, 2, "value[2].b");

	val_p = kv_store_get(kvs, 3);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 3)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 0, "value[3]");

	val_p = kv_store_get(kvs, 4);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 4)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 0, "value[4] = 0");

	/* Make sure bpf prog see the same thing */
	skel->bss->target_pid = pid;
	skel->bss->test_key = 2;
	skel->bss->test_op = KVS_STRUCT_GET;
	sys_gettid();
	ASSERT_EQ(skel->bss->test_struct_val.a, 1, "bpf:value[2].a");
	ASSERT_EQ(skel->bss->test_struct_val.b, 2, "bpf:value[2].b");
	skel->bss->target_pid = -1;

	/* Change some key-value pairs */
	val = 1;
	kv_store_set(kvs, 0, &val, sizeof(val));

	/* Reuse the KV store and rollout v1 program */
	skel_v1 = test_uptr_kv_store_v1__open();
	if (!ASSERT_OK_PTR(skel, "skel_open v1"))
		goto out;

	kv_store_reuse(kvs, skel_v1->maps.data_map);

	kv_store_update(kvs, kvp_v1);

	err = test_uptr_kv_store_v1__load(skel_v1);
	if (!ASSERT_OK(err, "skel_load v1"))
		goto out;

	skel_v1->bss->target_pid = -1;
	err = test_uptr_kv_store_v1__attach(skel_v1);
	if (!ASSERT_OK(err, "skel_attach v1"))
		goto out;

	/* Check if the KV store is updated correctly */
	/* value[0] already exists and should not be zero initialized again */
	val_p = kv_store_get(kvs, 0);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 0)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 1, "value[0]");

	/* value[1] was deleted */
	val_p = kv_store_get(kvs, 1);
	ASSERT_ERR_PTR(val_p, "kv_store_get(kvs, 1)");

	/* value[2] was deleted and a new value[2] of a different type was set */
	val_p = kv_store_get(kvs, 2);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 2)"))
		goto out;
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->a, 3, "value[2].a");
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->b, 4, "value[2].b");
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->c, 5, "value[2].c");

	/* value[3] was be deleted */
	val_p = kv_store_get(kvs, 3);
	ASSERT_ERR_PTR(val_p, "kv_store_get(kvs, 3)");

	/* value[4] was updated to a new integer value */
	val_p = kv_store_get(kvs, 4);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 4)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 1234, "value[4]");

	/* value[5] was never set */
	val_p = kv_store_get(kvs, 5);
	ASSERT_ERR_PTR(val_p, "kv_store_get(kvs, 5)");

	/* value[6] of struct test_struct_v1 type was newly set */
	val_p = kv_store_get(kvs, 6);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 6)"))
		goto out;
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->a, 0, "value[6].a");
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->b, 0, "value[6].b");
	ASSERT_EQ(((struct test_struct_v1 *)val_p)->c, 0, "value[6].c");

	/* value[7] was never set */
	val_p = kv_store_get(kvs, 7);
	ASSERT_ERR_PTR(val_p, "kv_store_get(kvs, 7)");

	/* value[8] of int type was newly set */
	val_p = kv_store_get(kvs, 8);
	if (!ASSERT_OK_PTR(val_p, "kv_store_get(kvs, 8)"))
		goto out;
	ASSERT_EQ(*(int *)val_p, 0, "value[8]");

	/* Make sure bpf prog see the same thing */
	skel_v1->bss->target_pid = pid;
	skel_v1->bss->test_key = 2;
	skel_v1->bss->test_op = KVS_STRUCT_GET;
	sys_gettid();
	ASSERT_EQ(skel_v1->bss->test_struct_val.a, 3, "bpf:value[2].a");
	ASSERT_EQ(skel_v1->bss->test_struct_val.b, 4, "bpf:value[2].b");
	ASSERT_EQ(skel_v1->bss->test_struct_val.c, 5, "bpf:value[2].c");
	skel_v1->bss->target_pid = -1;

out:
	if (kvp)
		free(kvp);
	if (kvp_v1)
		free(kvp_v1);
	if (kvs)
		kv_store_close(kvs);
}

void test_uptr_kv_store(void)
{
	if (test__start_subtest("uptr_kv_store_basic"))
		test_uptr_kv_store_basic();
	if (test__start_subtest("uptr_kv_store_update"))
		test_uptr_kv_store_update();
}
