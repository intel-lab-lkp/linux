// SPDX-License-Identifier: GPL-2.0

#include <linux/security.h>
#include <linux/integrity.h>
#include <linux/ctype.h>
#include <keys/asymmetric-type.h>
#include <keys/asymmetric-subtype.h>
#include <keys/system_keyring.h>
#include <keys/user-type.h>
#include <crypto/pkcs7.h>
#include <kunit/static_stub.h>
#include "clavis.h"

static struct key *clavis_keyring;
static struct asymmetric_key_id *clavis_boot_akid;
static struct asymmetric_setup_kid clavis_setup_akid;
static bool clavis_enforced;

static int pkcs7_preparse_content(void *ctx, const void *data, size_t len, size_t asn1hdrlen)
{
	struct key_preparsed_payload *prep = ctx;
	const void *saved_prep_data;
	size_t saved_prep_datalen;
	char *desc;
	int ret, i;

	/* key_acl_free_preparse will free this */
	desc = kmemdup(data, len, GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	/* Copy the user supplied contents and remove any white space. */
	for (i = 0; i < len; i++) {
		desc[i] = tolower(desc[i]);
		if (isspace(desc[i]))
			desc[i] = 0;
	}

	prep->description = desc;
	saved_prep_data = prep->data;
	saved_prep_datalen = prep->datalen;
	prep->data = desc;
	prep->datalen = len;
	ret = user_preparse(prep);
	prep->data = saved_prep_data;
	prep->datalen = saved_prep_datalen;
	return ret;
}

int (* const pkcs7_preparse_content_fn_ptr)(void *ctx, const void *data, size_t len,
					    size_t asn1hdrlen) = pkcs7_preparse_content;

static void key_acl_free_preparse(struct key_preparsed_payload *prep)
{
	kfree(prep->description);
	user_free_preparse(prep);
}

static struct key *clavis_keyring_get(void)
{
	KUNIT_STATIC_STUB_REDIRECT(clavis_keyring_get);
	return clavis_keyring;
}

struct key * (* const clavis_keyring_get_fn_ptr)(void) = clavis_keyring_get;

static bool clavis_acl_enforced(void)
{
	KUNIT_STATIC_STUB_REDIRECT(clavis_acl_enforced);
	return clavis_enforced;
}

bool (* const clavis_acl_enforced_fn_ptr)(void) = clavis_acl_enforced;

static int key_acl_preparse(struct key_preparsed_payload *prep)
{
	KUNIT_STATIC_STUB_REDIRECT(key_acl_preparse, prep);

	/*
	 * Only allow the description to be set via the pkcs7 data contents.
	 * The exception to this rule is if the entry was builtin, it will have
	 * the original_description set.  Since we don't have access to the key
	 * within the preparse step to determine if the entity is builtin, let
	 * it through now and this will be checked in the instantiate step.
	 */
	if (prep->orig_description)
		return 0;

	return verify_pkcs7_signature(NULL, 0, prep->data, prep->datalen, clavis_keyring_get(),
				      VERIFYING_CLAVIS_SIGNATURE, pkcs7_preparse_content,
				      prep);
}

int (* const key_acl_preparse_fn_ptr)(struct key_preparsed_payload *prep) = key_acl_preparse;

static int key_acl_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
	/*
	 * The orig_description may only be used for builtin entities.  All
	 * other entries must have been validated through the pkcs7 signature
	 * within the preparse stage.
	 */
	if (prep->orig_description && !(key->flags & (1 << KEY_FLAG_BUILTIN)))
		return -EINVAL;

	key->perm = KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH |
		    KEY_USR_VIEW;
	set_bit(KEY_FLAG_KEEP, &key->flags);
	return generic_key_instantiate(key, prep);
}

static void key_acl_destroy(struct key *key)
{
	/* It should not be possible to get here */
	pr_info("destroy clavis_key_acl denied\n");
}

static void key_acl_revoke(struct key *key)
{
	/* It should not be possible to get here */
	pr_info("revoke clavis_key_acl denied\n");
}

static int key_acl_update(struct key *key, struct key_preparsed_payload *prep)
{
	return -EPERM;
}

static int key_acl_vet_description(const char *desc)
{
	int i, desc_len;
	s16 ktype;

	if (!desc)
		goto invalid;

	desc_len = sizeof(desc);

	/*
	 * clavis_acl format:
	 *    xx:yyyy...
	 *
	 *    xx     - Single byte of the key type
	 *    :      - Ascii colon
	 *    yyyy.. - Even number of hexadecimal characters representing the keyid
	 */

	/* The min clavis acl is 7 characters. */
	if (desc_len < 7)
		goto invalid;

	/* Check the first byte is a valid key type. */
	if (sscanf(desc, "%2hx", &ktype) != 1)
		goto invalid;

	if (ktype >= VERIFYING_CLAVIS_SIGNATURE)
		goto invalid;

	/* Check that there is a colon following the key type */
	if (desc[2] != ':')
		goto invalid;

	/* Move past the colon. */
	desc += 3;

	for (i = 0; *desc && i < CLAVIS_ASCII_KID_MAX; desc++, i++) {
		/* Check if lowercase hex number */
		if (!isxdigit(*desc) || isupper(*desc))
			goto invalid;
	}

	/* Check if the has is greater than CLAVIS_ASCII_KID_MAX. */
	if (*desc)
		goto invalid;

	/* Check for even number of hex characters. */
	if (i == 0 || i & 1)
		goto invalid;

	return 0;

invalid:
	pr_err("Unparsable clavis key id: %s\n", desc);
	return -EINVAL;
}

static struct key_type clavis_key_acl = {
	.name			= "clavis_key_acl",
	.preparse		= key_acl_preparse,
	.free_preparse		= key_acl_free_preparse,
	.instantiate		= key_acl_instantiate,
	.update			= key_acl_update,
	.revoke			= key_acl_revoke,
	.destroy		= key_acl_destroy,
	.vet_description	= key_acl_vet_description,
	.read			= user_read,
};

static int restrict_link_for_clavis(struct key *dest_keyring, const struct key_type *type,
				    const union key_payload *payload, struct key *restrict_key)
{
	/*
	 * Allow a single asymmetric key into this keyring. This key is used as the
	 * root of trust for anything added afterwards.
	 */
	if (type == &key_type_asymmetric && dest_keyring == clavis_keyring &&
	    !clavis_acl_enforced()) {
		clavis_enforced = true;
		return 0;
	}

	if (type == &clavis_key_acl)
		return 0;

	return -EOPNOTSUPP;
}

static struct asymmetric_key_id *clavis_parse_boot_param(char *kid, struct asymmetric_key_id *akid,
							 int akid_max_len)
{
	int error, hex_len;

	if (!kid)
		return 0;

	hex_len = strlen(kid) / 2;

	if (hex_len > akid_max_len)
		return 0;

	akid->len = hex_len;
	error = hex2bin(akid->data, kid, akid->len);

	if (error < 0) {
		pr_err("Unparsable clavis key id\n");
		return 0;
	}

	return akid;
}

struct asymmetric_key_id *
	(* const parse_boot_param_fn_ptr)(char *kid, struct asymmetric_key_id *akid,
					  int akid_max_len) = clavis_parse_boot_param;

static int __init clavis_param(char *kid)
{
	clavis_boot_akid = clavis_parse_boot_param(kid, &clavis_setup_akid.id,
						   ARRAY_SIZE(clavis_setup_akid.data));

	return 1;
}

__setup("clavis=", clavis_param);

static struct key *clavis_keyring_alloc(const char *desc, struct key_restriction *restriction)
{
	struct key *keyring;

	keyring = keyring_alloc(desc, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, current_cred(),
				KEY_POS_VIEW | KEY_POS_READ | KEY_POS_SEARCH | KEY_POS_WRITE |
				KEY_USR_VIEW | KEY_USR_READ | KEY_USR_SEARCH | KEY_USR_WRITE,
				KEY_ALLOC_NOT_IN_QUOTA | KEY_ALLOC_SET_KEEP,
				restriction, NULL);
	return keyring;
}

struct key *
	(* const keyring_alloc_fn_ptr)(const char *desc, struct key_restriction *restriction) =
				       clavis_keyring_alloc;

static struct key_restriction *clavis_restriction_alloc(key_restrict_link_func_t check_func)
{
	struct key_restriction *restriction;

	restriction = kzalloc(sizeof(*restriction), GFP_KERNEL);

	if (restriction)
		restriction->check = check_func;

	return restriction;
}

struct key_restriction *
	(* const restriction_alloc_fn_ptr)(key_restrict_link_func_t
					   check_func) = clavis_restriction_alloc;

static void clavis_add_acl(const char *const *skid_list, struct key *keyring)
{
	const char *const *acl;
	key_ref_t key;

	for (acl = skid_list; *acl; acl++) {
		key = key_create(make_key_ref(keyring, true),
				 "clavis_key_acl",
				  *acl,
				  NULL,
				  0,
				  KEY_POS_SEARCH | KEY_POS_VIEW | KEY_USR_SEARCH | KEY_USR_VIEW,
				  KEY_ALLOC_NOT_IN_QUOTA | KEY_ALLOC_BUILT_IN |
				  KEY_ALLOC_BYPASS_RESTRICTION);
		if (IS_ERR(key)) {
			if (PTR_ERR(key) == -EEXIST)
				pr_info("Duplicate clavis_key_acl %s\n", *acl);
			else
				pr_info("Problem with clavis_key_acl %s: %pe\n", *acl, key);
		} else {
			pr_info("Added clavis_key_acl %s\n", *acl);
		}
	}
}

void (* const clavis_add_acl_fn_ptr)(const char *const *skid_list,
				     struct key *keyring) = clavis_add_acl;

int __init clavis_keyring_init(void)
{
	struct key_restriction *restriction;

	if (register_key_type(&clavis_key_acl) < 0)
		panic("Can't allocate clavis key type\n");

	restriction = clavis_restriction_alloc(restrict_link_for_clavis);
	if (!restriction)
		panic("Can't allocate clavis keyring restriction\n");

	clavis_keyring = clavis_keyring_alloc(".clavis", restriction);
	if (IS_ERR(clavis_keyring))
		panic("Can't allocate clavis keyring\n");

	clavis_add_acl(clavis_module_acl, clavis_keyring);
	clavis_add_acl(clavis_builtin_acl_list, clavis_keyring);

	return 0;
}

void __init late_init_clavis_setup(void)
{
	struct asymmetric_setup_kid efi_keyid;
	struct asymmetric_key_id *keyid = &efi_keyid.id;
	int error;

	error = clavis_efi_param(keyid, ARRAY_SIZE(efi_keyid.data));

	if (error && !clavis_boot_akid)
		return;

	if (error)
		keyid = clavis_boot_akid;

	system_key_link(clavis_keyring, clavis_boot_akid);
}

int clavis_sig_verify(const struct key *key, const struct public_key_signature *sig)
{
	const struct asymmetric_key_ids *kids = asymmetric_key_ids(key);
	const struct asymmetric_key_subtype *subtype;
	const struct asymmetric_key_id *newkid;
	char *buf_ptr, *ptr;
	key_ref_t ref;
	int i, buf_len;

	if (!clavis_acl_enforced())
		return 0;
	if (key->type != &key_type_asymmetric)
		return -EKEYREJECTED;
	subtype = asymmetric_key_subtype(key);
	if (!subtype || !key->payload.data[0])
		return -EKEYREJECTED;
	if (!subtype->verify_signature)
		return -EKEYREJECTED;

	/* Allow sig validation when not using a system keyring */
	if (!test_bit(PKS_USAGE_SET, &sig->usage_flags))
		return 0;

	/* The previous sig validation is enough to get on the clavis keyring */
	if (sig->usage == VERIFYING_CLAVIS_SIGNATURE)
		return 0;

	if (test_bit(PKS_REVOCATION_PASS, &sig->usage_flags))
		return 0;

	for (i = 0, buf_len = 0; i < 3; i++) {
		if (kids->id[i]) {
			newkid = (struct asymmetric_key_id *)kids->id[i];
			if (newkid->len > buf_len)
				buf_len = newkid->len;
		}
	}

	if (!buf_len)
		return -EKEYREJECTED;

	/* Allocate enough space for the conversion to ascii plus the header. */
	buf_ptr = kmalloc(buf_len * 2 + 4, GFP_KERNEL | __GFP_ZERO);

	if (!buf_ptr)
		return -ENOMEM;

	for (i = 0; i < 3; i++) {
		if (kids->id[i]) {
			newkid = (struct asymmetric_key_id *)kids->id[i];
			if (!newkid->len)
				continue;

			ptr = buf_ptr;
			ptr = bin2hex(ptr, &sig->usage, 1);
			*ptr++ = ':';
			ptr = bin2hex(ptr, newkid->data, newkid->len);
			*ptr = 0;
			ref = keyring_search(make_key_ref(clavis_keyring_get(), true),
					     &clavis_key_acl, buf_ptr, false);

			if (!IS_ERR(ref))
				break;
		}
	}

	kfree(buf_ptr);

	if (IS_ERR(ref))
		return -EKEYREJECTED;

	return 0;
}
