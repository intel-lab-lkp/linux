// SPDX-License-Identifier: GPL-2.0

#include <linux/security.h>
#include <linux/integrity.h>
#include <linux/ctype.h>
#include <keys/asymmetric-type.h>
#include <keys/asymmetric-subtype.h>
#include <keys/system_keyring.h>
#include <keys/user-type.h>
#include <crypto/pkcs7.h>
#include "clavis.h"

static struct key *clavis_keyring;
static struct asymmetric_key_id *setup_keyid;

#define MAX_ASCII_KID 64
#define MAX_BIN_KID   32

static struct {
	struct asymmetric_key_id id;
	unsigned char data[MAX_BIN_KID];
} setup_key;

static int pkcs7_preparse_content(void *ctx, const void *data, size_t len,
				  size_t asn1hdrlen)
{
	struct key_preparsed_payload *prep = ctx;
	const void *saved_prep_data;
	size_t saved_prep_datalen;
	const char *p;
	char *desc;
	int ret, i;

	/* key_acl_free_preparse will free this */
	desc = kmalloc(len, GFP_KERNEL);

	if (!desc)
		return -ENOMEM;
	memcpy(desc, data, len);

	/* remove any white space */
	for (i = 0, p = desc; i < len; i++, p++) {
		if (isspace(*p))
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

static void key_acl_free_preparse(struct key_preparsed_payload *prep)
{
	kfree(prep->description);
	user_free_preparse(prep);
}

static int key_acl_preparse(struct key_preparsed_payload *prep)
{
	/* Only allow the description to be set via the pkcs7 data contents */
	if (prep->orig_description)
		return -EINVAL;

	return verify_pkcs7_signature(NULL, 0, prep->data, prep->datalen, clavis_keyring,
				      VERIFYING_CLAVIS_SIGNATURE, pkcs7_preparse_content,
				      prep);
}

static int key_acl_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
	key->perm |= KEY_USR_READ;
	key->perm |= KEY_USR_SEARCH;
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
	unsigned char data[MAX_BIN_KID];
	int ascii_len, hex_len, error;

	ascii_len = strlen(desc);

	/*
	 * clavis_acl format:
	 *    xx:yyyyyyyyy...
	 *
	 *    xx   - Single byte of the key type
	 *    :    - Ascii colon
	 *    yyyy - Even number of hexadecimal characters representing the keyid
	 */
	if (ascii_len < 5 || ascii_len > (MAX_ASCII_KID + 3) || desc[2] != ':')
		return -EINVAL;

	/* move past the colon */
	ascii_len -= 3;
	hex_len = ascii_len / 2;
	error = hex2bin(data, desc + 3, hex_len);

	if (error < 0)
		pr_err("Unparsable clavis key id\n");

	return error;
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
	static bool first_pass = true;

	/*
	 * Allow a single asymmetric key into this keyring. This key is used as the
	 * root of trust for anything added afterwards.
	 */
	if (type == &key_type_asymmetric && dest_keyring == clavis_keyring && first_pass) {
		first_pass = false;
		return 0;
	}

	if (type == &clavis_key_acl)
		return 0;

	return -EOPNOTSUPP;
}

static int __init clavis_param(char *kid)
{
	struct asymmetric_key_id *p = &setup_key.id;
	int error, hex_len, ascii_len = strlen(kid);

	if (!kid)
		return 1;

	hex_len = ascii_len / 2;

	if (hex_len > sizeof(setup_key.data))
		return 1;

	p->len = hex_len;
	error = hex2bin(p->data, kid, p->len);

	if (error < 0) {
		pr_err("Unparsable clavis key id\n");
	} else {
		setup_keyid = p;
		pr_info("clavis key id: %s\n", kid);
	}

	return 1;
}
__setup("clavis=", clavis_param);

static int __init clavis_keyring_init(void)
{
	struct key_restriction *restriction;

	if (register_key_type(&clavis_key_acl) < 0)
		panic("Can't allocate clavis key type\n");

	restriction = kzalloc(sizeof(*restriction), GFP_KERNEL);
	if (!restriction)
		panic("Can't allocate clavis keyring restriction\n");
	restriction->check = restrict_link_for_clavis;
	clavis_keyring =
		keyring_alloc(".clavis", GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, current_cred(),
			      KEY_POS_VIEW | KEY_POS_READ | KEY_POS_SEARCH | KEY_POS_WRITE |
			      KEY_USR_VIEW | KEY_USR_READ | KEY_USR_SEARCH | KEY_USR_WRITE,
			      KEY_ALLOC_NOT_IN_QUOTA | KEY_ALLOC_SET_KEEP,
			      restriction, NULL);

	if (IS_ERR(clavis_keyring))
		panic("Can't allocate clavis keyring\n");

	return 0;
}

void __init late_init_clavis_setup(void)
{
	int error;
	struct {
		struct asymmetric_key_id id;
		unsigned char data[MAX_BIN_KID];
	} efi_keyid;
	struct asymmetric_key_id *keyid = &efi_keyid.id;

	error = clavis_efi_param(keyid, sizeof(efi_keyid.data));

	if (error && !setup_keyid)
		return;

	if (error)
		keyid = setup_keyid;

	clavis_keyring_init();
	system_key_link(clavis_keyring, keyid);
}
