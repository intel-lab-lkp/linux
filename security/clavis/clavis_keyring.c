// SPDX-License-Identifier: GPL-2.0

#include <linux/security.h>
#include <linux/integrity.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include "clavis.h"

static struct key *clavis_keyring;
static struct asymmetric_key_id *clavis_boot_akid;
static struct asymmetric_setup_kid clavis_setup_akid;
static bool clavis_enforced;

static bool clavis_acl_enforced(void)
{
	return clavis_enforced;
}
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

static struct key_restriction *clavis_restriction_alloc(key_restrict_link_func_t check_func)
{
	struct key_restriction *restriction;

	restriction = kzalloc(sizeof(*restriction), GFP_KERNEL);

	if (restriction)
		restriction->check = check_func;

	return restriction;
}

static int __init clavis_keyring_init(void)
{
	struct key_restriction *restriction;

	restriction = clavis_restriction_alloc(restrict_link_for_clavis);
	if (!restriction)
		panic("Can't allocate clavis keyring restriction\n");

	clavis_keyring = clavis_keyring_alloc(".clavis", restriction);
	if (IS_ERR(clavis_keyring))
		panic("Can't allocate clavis keyring\n");

	return 0;
}

void __init late_init_clavis_setup(void)
{
	clavis_keyring_init();

	if (!clavis_boot_akid)
		return;

	system_key_link(clavis_keyring, clavis_boot_akid);
}
