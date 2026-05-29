// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for keyring operations.
 */

#include <keys/user-type.h>
#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/key.h>
#include <linux/key-type.h>
#include <linux/keyctl.h>
#include <linux/module.h>
#include <linux/uidgid.h>

static void keyring_test_key_put(void *data)
{
	key_put(data);
}

static struct key *test_keyring_alloc(struct kunit *test, const char *desc,
				      unsigned long flags)
{
	struct key *keyring;

	keyring = keyring_alloc(desc, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID,
				current_cred(), KEY_POS_ALL | KEY_USR_ALL,
				KEY_ALLOC_NOT_IN_QUOTA | flags, NULL, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, keyring);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							keyring_test_key_put,
							keyring), 0);

	return keyring;
}

static struct key *test_user_key_alloc(struct kunit *test, const char *desc,
				       struct key *keyring,
				       unsigned long flags)
{
	static const char payload[] = "payload";
	struct key *key;
	int ret;

	key = key_alloc(&key_type_user, desc, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID,
			current_cred(), KEY_POS_ALL | KEY_USR_ALL,
			KEY_ALLOC_NOT_IN_QUOTA | flags, NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, key);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
							keyring_test_key_put,
							key), 0);

	ret = key_instantiate_and_link(key, payload, sizeof(payload),
				       keyring, NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return key;
}

static void keyring_move_user_key(struct kunit *test)
{
	struct key *from, *to, *key;
	int ret;

	from = test_keyring_alloc(test, "move-from", 0);
	to = test_keyring_alloc(test, "move-to", 0);
	key = test_user_key_alloc(test, "move-key", from, 0);

	ret = key_move(key, from, to, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = key_move(key, to, from, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static void keyring_move_keep_key_fails(struct kunit *test)
{
	struct key *from, *to, *key;
	int ret;

	from = test_keyring_alloc(test, "keep-from", KEY_ALLOC_SET_KEEP);
	to = test_keyring_alloc(test, "keep-to", 0);
	key = test_user_key_alloc(test, "keep-key", from, 0);

	KUNIT_ASSERT_TRUE(test, test_bit(KEY_FLAG_KEEP, &from->flags));
	KUNIT_ASSERT_TRUE(test, test_bit(KEY_FLAG_KEEP, &key->flags));

	ret = key_move(key, from, to, 0);
	KUNIT_EXPECT_EQ(test, ret, -EPERM);

	ret = key_move(key, to, from, 0);
	KUNIT_EXPECT_EQ(test, ret, -ENOENT);
}

static void keyring_move_keep_same_keyring(struct kunit *test)
{
	struct key *keyring, *key;
	int ret;

	keyring = test_keyring_alloc(test, "keep-same", KEY_ALLOC_SET_KEEP);
	key = test_user_key_alloc(test, "keep-same-key", keyring, 0);

	ret = key_move(key, keyring, keyring, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

static struct kunit_case keyring_test_cases[] = {
	KUNIT_CASE(keyring_move_user_key),
	KUNIT_CASE(keyring_move_keep_key_fails),
	KUNIT_CASE(keyring_move_keep_same_keyring),
	{}
};

static struct kunit_suite keyring_test_suite = {
	.name = "keyring",
	.test_cases = keyring_test_cases,
};

kunit_test_suite(keyring_test_suite);

MODULE_LICENSE("GPL");
