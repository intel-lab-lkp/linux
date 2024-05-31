// SPDX-License-Identifier: GPL-2.0

#include <linux/security.h>
#include <linux/integrity.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include "clavis.h"

static struct key *clavis_keyring;
static struct asymmetric_key_id *setup_keyid;

#define MAX_BIN_KID   32

static struct {
	struct asymmetric_key_id id;
	unsigned char data[MAX_BIN_KID];
} setup_key;

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
