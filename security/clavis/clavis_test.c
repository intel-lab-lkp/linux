// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <kunit/static_stub.h>
#include <linux/init.h>
#include <linux/key-type.h>
#include <keys/asymmetric-type.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/module_signature.h>
#include <keys/system_keyring.h>
#include <linux/cred.h>
#include "clavis.h"
#include <generated/clavis/x509.h>
#include <generated/clavis/acl.h>
#include <generated/clavis/signed_data.h>

static struct key *machine_keyring;
static struct key *clavis_keyring;
static bool clavis_enforced;

const char *const clavis_builtin_test_acl_list[] = {
	"01:02b4e19c7efc4512ae4911d9e7e6c3c9",
	"05:b6c202e7710544a885e425387cd344f6",
	"04:ca5b4645541c4e828ef460806f9a61bc",
	NULL
};

static int clavis_suite_init(struct kunit_suite *suite)
{
	kunit_info(suite, "Initializing Clavis Suite\n");

	machine_keyring = keyring_alloc(".machine_kunit",
					GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, current_cred(),
					(KEY_POS_ALL & ~KEY_POS_SETATTR) |
					KEY_USR_VIEW | KEY_USR_READ |
					KEY_USR_SEARCH,
					KEY_ALLOC_NOT_IN_QUOTA,
					NULL, NULL);

	x509_load_certificate_list(ctv_00_x509, ctv_00_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_01_x509, ctv_01_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_02_x509, ctv_02_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_03_x509, ctv_03_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_04_x509, ctv_04_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_05_x509, ctv_05_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_ca0_x509, ctv_ca0_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_ca1_x509, ctv_ca1_x509_len, machine_keyring);
	x509_load_certificate_list(ctv_rev_x509, ctv_rev_x509_len, machine_keyring);

	/*
	 * Note, this will leave the machine with one additional revocation in the
	 * blacklist keyring when done.
	 */
	add_key_to_revocation_list(ctv_rev_x509, ctv_rev_x509_len);

	return 0;
}

static void clavis_suite_exit(struct kunit_suite *suite)
{
	key_put(machine_keyring);
}

static int restrict_link_for_clavis_test(struct key *dest_keyring, const struct key_type *type,
					 const union key_payload *payload, struct key *restrict_key)
{
	struct key_type *clavis;
	int rval = 0;

	if (type == &key_type_asymmetric && dest_keyring == clavis_keyring && !clavis_enforced) {
		clavis_enforced = true;
		return 0;
	}

	clavis = key_type_lookup("clavis_key_acl");
	if (type != clavis)
		rval = -EOPNOTSUPP;

	if (clavis != ERR_PTR(-ENOKEY))
		key_type_put(clavis);

	return rval;
}

static int clavis_test_keyring_init(struct kunit *test)
{
	struct key_restriction *restriction;

	restriction = restriction_alloc_fn_ptr(restrict_link_for_clavis_test);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, restriction);
	clavis_keyring = keyring_alloc_fn_ptr(".clavis_test", restriction);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, clavis_keyring);
	KUNIT_EXPECT_EQ(test, clavis_keyring->perm, KEY_POS_VIEW | KEY_POS_READ | KEY_POS_SEARCH |
			KEY_POS_WRITE | KEY_USR_VIEW | KEY_USR_READ | KEY_USR_SEARCH |
			KEY_USR_WRITE);
	clavis_enforced = false;
	return 0;
}

static void clavis_test_keyring_exit(struct kunit *test)
{
	key_put(clavis_keyring);
	clavis_keyring = NULL;
	clavis_enforced = false;
}

static void builtin_acl_tests(struct kunit *test)
{
	key_ref_t key;
	const char *const *desc;

	clavis_add_acl_fn_ptr(clavis_builtin_test_acl_list, clavis_keyring);

	for (desc = clavis_builtin_test_acl_list; *desc; desc++) {
		key = keyring_search(make_key_ref(clavis_keyring, true),
				     key_type_lookup("clavis_key_acl"),
				     *desc,
				     true);
		KUNIT_EXPECT_FALSE(test, (IS_ERR(key)));
		KUNIT_EXPECT_EQ(test, strcmp(key_ref_to_ptr(key)->description, *desc), 0);
		KUNIT_EXPECT_EQ(test,
				keyctl_update_key(key_ref_to_ptr(key)->serial, NULL, 0),
				-EACCES);
		KUNIT_EXPECT_EQ(test, key_ref_to_ptr(key)->perm, KEY_POS_SEARCH | KEY_POS_VIEW |
				KEY_USR_SEARCH | KEY_USR_VIEW);
		key_ref_put(key);
	}
}

static void register_key_type_tests(struct kunit *test)
{
	struct key_type *clavis;

	clavis = key_type_lookup("clavis_key_acl");
	KUNIT_EXPECT_PTR_NE(test, clavis, ERR_PTR(-ENOKEY));
	if (clavis != ERR_PTR(-ENOKEY))
		key_type_put(clavis);

	clavis = key_type_lookup("bogus");
	KUNIT_EXPECT_PTR_EQ(test, clavis, ERR_PTR(-ENOKEY));
	if (clavis != ERR_PTR(-ENOKEY))
		key_type_put(clavis);
}

static void clavis_parse_boot_param_tests(struct kunit *test)
{
	char *huge = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01";
	struct asymmetric_setup_kid ask;
	struct asymmetric_key_id *kid;

	KUNIT_EXPECT_PTR_EQ(test, parse_boot_param_fn_ptr(NULL, &ask.id, ARRAY_SIZE(ask.data)),
			    NULL);
	KUNIT_EXPECT_PTR_EQ(test, parse_boot_param_fn_ptr(huge, &ask.id, ARRAY_SIZE(ask.data)),
			    NULL);
	KUNIT_EXPECT_PTR_EQ(test, parse_boot_param_fn_ptr("0x1000", &ask.id, ARRAY_SIZE(ask.data)),
			    NULL);
	KUNIT_EXPECT_PTR_EQ(test, parse_boot_param_fn_ptr("nothex", &ask.id, ARRAY_SIZE(ask.data)),
			    NULL);
	kid = parse_boot_param_fn_ptr("01234567", &ask.id, ARRAY_SIZE(ask.data));

	KUNIT_EXPECT_EQ(test, kid->len, 4);
	KUNIT_EXPECT_EQ(test, kid->data[0], 0x01);
	KUNIT_EXPECT_EQ(test, kid->data[1], 0x23);
	KUNIT_EXPECT_EQ(test, kid->data[2], 0x45);
	KUNIT_EXPECT_EQ(test, kid->data[3], 0x67);
}

static inline bool vet_description_test(struct key *keyring, const char *desc)
{
	key_ref_t key;

	key = key_create(make_key_ref(keyring, true),
			 "clavis_key_acl",
			  desc,
			  NULL,
			  0,
			  KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH | KEY_USR_VIEW,
			  KEY_ALLOC_BUILT_IN);

	if (IS_ERR(key))
		return false;

	return true;
}

static void key_acl_vet_description_tests(struct kunit *test)
{
	char *huge = "01:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01";
	char *large = "01:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	char buf[64];
	int i;

	KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, "00:nothex"));
	KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, "0:1234"));
	KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, "01:123"));
	KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, "X0:123"));
	KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, huge));
	KUNIT_EXPECT_TRUE(test, vet_description_test(clavis_keyring, large));

	for (i = 0; i < 255; i++) {
		snprintf(buf, sizeof(buf), "%2hx:01234567", i);
		if (i < VERIFYING_CLAVIS_SIGNATURE)
			KUNIT_EXPECT_TRUE(test, vet_description_test(clavis_keyring, buf));
		else
			KUNIT_EXPECT_FALSE(test, vet_description_test(clavis_keyring, buf));
	}
}

static void key_usage_tests(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, VERIFYING_CLAVIS_SIGNATURE + 1, NR__KEY_BEING_USED_FOR);
}

static int key_acl_preparse_kunit(struct key_preparsed_payload *prep)
{
	if (prep->orig_description)
		return 0;

	return verify_pkcs7_signature(NULL, 0, prep->data, prep->datalen, clavis_keyring,
				      VERIFYING_CLAVIS_SIGNATURE, pkcs7_preparse_content_fn_ptr,
				      prep);
}

static void signed_acl_tests(struct kunit *test)
{
	int i;
	const unsigned char *ca0_acl_pkcs7[] = { ctv_ca0_00_pkcs7, ctv_ca0_01_pkcs7,
						 ctv_ca0_02_pkcs7, ctv_ca0_03_pkcs7,
						 ctv_ca0_04_pkcs7, ctv_ca0_05_pkcs7
					       };

	const u32 ca0_acl_pkcs7_buf_size[] = { ctv_ca0_00_pkcs7_len, ctv_ca0_01_pkcs7_len,
					       ctv_ca0_02_pkcs7_len, ctv_ca0_03_pkcs7_len,
					       ctv_ca0_04_pkcs7_len, ctv_ca0_05_pkcs7_len
					     };

	const unsigned char *ca1_acl_pkcs7[] = { ctv_ca1_00_pkcs7, ctv_ca1_01_pkcs7,
						 ctv_ca1_02_pkcs7, ctv_ca1_03_pkcs7,
						 ctv_ca1_04_pkcs7, ctv_ca1_05_pkcs7
					       };

	const u32 ca1_acl_pkcs7_buf_size[] = { ctv_ca1_00_pkcs7_len, ctv_ca1_01_pkcs7_len,
					       ctv_ca1_02_pkcs7_len, ctv_ca1_03_pkcs7_len,
					       ctv_ca1_04_pkcs7_len, ctv_ca1_05_pkcs7_len
					     };

	char const *acl_list[] = { ctv_00_x509_acl, ctv_01_x509_acl, ctv_02_x509_acl,
				   ctv_03_x509_acl, ctv_04_x509_acl, ctv_05_x509_acl };

	key_ref_t key;

	KUNIT_EXPECT_EQ(test,
			x509_load_certificate_list(ctv_ca0_x509, ctv_ca0_x509_len, clavis_keyring),
			0);

	clavis_enforced = true;

	for (i = 0; i < 6; i++) {
		key = key_create(make_key_ref(clavis_keyring, true),
				 "clavis_key_acl",
				 NULL,
				 ca0_acl_pkcs7[i],
				 ca0_acl_pkcs7_buf_size[i],
				 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH | KEY_USR_VIEW,
				 KEY_ALLOC_BUILT_IN);

		KUNIT_EXPECT_TRUE(test, IS_ERR(key));

		key = keyring_search(make_key_ref(clavis_keyring, true),
				     key_type_lookup("clavis_key_acl"),
				     acl_list[i],
				     true);

		KUNIT_EXPECT_TRUE(test, IS_ERR(key));
	}

	kunit_activate_static_stub(test, key_acl_preparse_fn_ptr, key_acl_preparse_kunit);

	for (i = 0; i < 6; i++) {
		key = key_create(make_key_ref(clavis_keyring, true),
				 "clavis_key_acl",
				 NULL,
				 ca0_acl_pkcs7[i],
				 ca0_acl_pkcs7_buf_size[i],
				 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH | KEY_USR_VIEW,
				 KEY_ALLOC_BUILT_IN);

		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, key);

		key = keyring_search(make_key_ref(clavis_keyring, true),
				     key_type_lookup("clavis_key_acl"),
				     acl_list[i],
				     true);

		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, key);
	}

	for (i = 0; i < 6; i++) {
		key = key_create(make_key_ref(clavis_keyring, true),
				 "clavis_key_acl",
				 NULL,
				 ca1_acl_pkcs7[i],
				 ca1_acl_pkcs7_buf_size[i],
				 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH | KEY_USR_VIEW,
				 KEY_ALLOC_BUILT_IN);

		KUNIT_EXPECT_TRUE(test, IS_ERR(key));
	}

	kunit_deactivate_static_stub(test, key_acl_preparse_fn_ptr);
}

static struct key *clavis_key_get(void)
{
	return clavis_keyring;
}

static bool clavis_acl_enforced(void)
{
	return clavis_enforced;
}

static void module_invalid_signed_tests(struct kunit *test)
{
	const void *mod = ctv_module_ca0_signed;
	struct module_signature ms;
	key_ref_t key;
	size_t sig_len, mod_len;

	kunit_activate_static_stub(test, clavis_keyring_get_fn_ptr, clavis_key_get);
	kunit_activate_static_stub(test, clavis_acl_enforced_fn_ptr, clavis_acl_enforced);

	/* Remove the module signature appended information at the end. */
	mod_len = ctv_module_ca0_signed_len - 28;
	KUNIT_EXPECT_GT(test, mod_len, sizeof(ms));
	memcpy(&ms, mod + (mod_len - sizeof(ms)), sizeof(ms));
	sig_len = be32_to_cpu(ms.sig_len);
	mod_len -= sig_len + sizeof(ms);

	/*
	 * Enforcement has not been set yet, therefore the verification passes
	 * without an ACL. The module signing key is in the machine_kunit
	 * keyring.
	 */
	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), 0);

	/* Load the clavis CA0 in the clavis keyring. */
	KUNIT_EXPECT_EQ(test,
			x509_load_certificate_list(ctv_ca0_x509, ctv_ca0_x509_len, clavis_keyring),
			0);

	clavis_enforced = true;

	/* Enforcement has been enabled without an ACL set. */
	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), -EKEYREJECTED);

	/* Add the module ACL. */
	key = key_create(make_key_ref(clavis_keyring, true),
			 "clavis_key_acl",
			 NULL,
			 ctv_ca0_00_pkcs7,
			 ctv_ca0_00_pkcs7_len,
			 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH |
			 KEY_USR_VIEW, KEY_ALLOC_BUILT_IN);

	KUNIT_EXPECT_FALSE(test, IS_ERR(key));

	/* This module was not signed by the module ACL in the clavis keyring. */
	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), -EKEYREJECTED);

	kunit_deactivate_static_stub(test, clavis_keyring_get_fn_ptr);
	kunit_deactivate_static_stub(test, clavis_acl_enforced_fn_ptr);
}

static void module_signed_tests(struct kunit *test)
{
	const void *mod = ctv_module_00_signed;
	struct module_signature ms;
	key_ref_t key;
	size_t sig_len, mod_len;

	kunit_activate_static_stub(test, clavis_keyring_get_fn_ptr, clavis_key_get);
	kunit_activate_static_stub(test, clavis_acl_enforced_fn_ptr, clavis_acl_enforced);

	/* Remove the module signature appended information at the end. */
	mod_len = ctv_module_00_signed_len - 28;
	KUNIT_EXPECT_GT(test, mod_len, sizeof(ms));
	memcpy(&ms, mod + (mod_len - sizeof(ms)), sizeof(ms));
	sig_len = be32_to_cpu(ms.sig_len);
	mod_len -= sig_len + sizeof(ms);

	/*
	 * Enforcement has not been set yet, therefore the verification passes
	 * without an ACL.
	 */
	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), 0);

	/* Load the clavis CA0 in the clavis keyring. */
	KUNIT_EXPECT_EQ(test,
			x509_load_certificate_list(ctv_ca0_x509, ctv_ca0_x509_len, clavis_keyring),
			0);

	clavis_enforced = true;

	/* Enforcement has been enabled without an ACL set. */
	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), -EKEYREJECTED);

	/* Add the module ACL. */
	key = key_create(make_key_ref(clavis_keyring, true),
			 "clavis_key_acl",
			 NULL,
			 ctv_ca0_00_pkcs7,
			 ctv_ca0_00_pkcs7_len,
			 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH |
			 KEY_USR_VIEW, KEY_ALLOC_BUILT_IN);

	KUNIT_EXPECT_FALSE(test, IS_ERR(key));

	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), 0);

	kunit_deactivate_static_stub(test, clavis_keyring_get_fn_ptr);
	kunit_deactivate_static_stub(test, clavis_acl_enforced_fn_ptr);
}

static void module_revocation_tests(struct kunit *test)
{
	/*
	 * When this test starts, the cert used to sign the module is both in
	 * the machine_kunit keyring and the blacklist keyring.  Also the
	 * clavis_kunit is not in enforcement mode. This will test the
	 * PKS_REVOCATION_PASS.
	 */

	const void *mod = ctv_module_rev_signed;
	struct module_signature ms;
	size_t sig_len, mod_len;

	kunit_activate_static_stub(test, clavis_keyring_get_fn_ptr, clavis_key_get);
	kunit_activate_static_stub(test, clavis_acl_enforced_fn_ptr, clavis_acl_enforced);

	/* Remove the module signature appended information at the end. */
	mod_len = ctv_module_rev_signed_len - 28;
	KUNIT_EXPECT_GT(test, mod_len, sizeof(ms));
	memcpy(&ms, mod + (mod_len - sizeof(ms)), sizeof(ms));
	sig_len = be32_to_cpu(ms.sig_len);
	mod_len -= sig_len + sizeof(ms);

	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), -EKEYREJECTED);

	clavis_enforced = true;

	KUNIT_EXPECT_EQ(test, verify_pkcs7_signature(mod, mod_len,
						     mod + mod_len, sig_len,
						     machine_keyring,
						     VERIFYING_MODULE_SIGNATURE,
						     NULL, NULL), -EKEYREJECTED);

	kunit_deactivate_static_stub(test, clavis_keyring_get_fn_ptr);
	kunit_deactivate_static_stub(test, clavis_acl_enforced_fn_ptr);
}

static void unspecified_signed_tests(struct kunit *test)
{
	key_ref_t key;

	kunit_activate_static_stub(test, clavis_keyring_get_fn_ptr, clavis_key_get);
	kunit_activate_static_stub(test, clavis_acl_enforced_fn_ptr, clavis_acl_enforced);

	KUNIT_EXPECT_EQ(test,
			verify_pkcs7_signature(NULL, 0, ctv_data_05_signed, ctv_data_05_signed_len,
					       machine_keyring,
					       VERIFYING_UNSPECIFIED_SIGNATURE, NULL, NULL),
			0);

	/* Load the clavis CA0 in the clavis keyring. */
	KUNIT_EXPECT_EQ(test,
			x509_load_certificate_list(ctv_ca0_x509, ctv_ca0_x509_len, clavis_keyring),
			0);

	clavis_enforced = true;

	KUNIT_EXPECT_EQ(test,
			verify_pkcs7_signature(NULL, 0, ctv_data_05_signed, ctv_data_05_signed_len,
					       machine_keyring,
					       VERIFYING_UNSPECIFIED_SIGNATURE, NULL, NULL),
			-EKEYREJECTED);

	/* Add the unspecified ACL. */
	key = key_create(make_key_ref(clavis_keyring, true),
			 "clavis_key_acl",
			 NULL,
			 ctv_ca0_05_pkcs7,
			 ctv_ca0_05_pkcs7_len,
			 KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH |
			 KEY_USR_VIEW, KEY_ALLOC_BUILT_IN);

	KUNIT_EXPECT_FALSE(test, IS_ERR(key));

	KUNIT_EXPECT_EQ(test,
			verify_pkcs7_signature(NULL, 0, ctv_data_05_signed, ctv_data_05_signed_len,
					       machine_keyring,
					       VERIFYING_UNSPECIFIED_SIGNATURE, NULL, NULL),
			0);

	KUNIT_EXPECT_EQ(test,
			verify_pkcs7_signature(NULL, 0, ctv_data_05_signed, ctv_data_05_signed_len,
					       machine_keyring,
					       VERIFYING_MODULE_SIGNATURE, NULL, NULL),
			-EKEYREJECTED);

	kunit_deactivate_static_stub(test, clavis_keyring_get_fn_ptr);
	kunit_deactivate_static_stub(test, clavis_acl_enforced_fn_ptr);
}

static struct kunit_case clavis_test_cases[] = {
	KUNIT_CASE(builtin_acl_tests),
	KUNIT_CASE(register_key_type_tests),
	KUNIT_CASE(clavis_parse_boot_param_tests),
	KUNIT_CASE(key_acl_vet_description_tests),
	KUNIT_CASE(key_usage_tests),
	KUNIT_CASE(signed_acl_tests),
	KUNIT_CASE(module_signed_tests),
	KUNIT_CASE(module_invalid_signed_tests),
	KUNIT_CASE(module_revocation_tests),
	KUNIT_CASE(unspecified_signed_tests),
	{}
};

static struct kunit_suite clavis_test_suite = {
	.name = "clavis",
	.suite_init = clavis_suite_init,
	.suite_exit = clavis_suite_exit,
	.init = clavis_test_keyring_init,
	.exit = clavis_test_keyring_exit,
	.test_cases = clavis_test_cases,
};

kunit_test_suites(&clavis_test_suite);
