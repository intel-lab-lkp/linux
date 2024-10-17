/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SECURITY_CLAVIS_H_
#define _SECURITY_CLAVIS_H_
#include <keys/asymmetric-type.h>

struct public_key_signature;

/* Max length for the asymmetric key id contained on the boot param */
#define CLAVIS_BIN_KID_MAX   32
#define CLAVIS_ASCII_KID_MAX 64

struct asymmetric_setup_kid {
	struct asymmetric_key_id id;
	unsigned char data[CLAVIS_BIN_KID_MAX];
};

extern const char __initconst *const clavis_builtin_acl_list[];

#ifndef CONFIG_SYSTEM_TRUSTED_KEYRING
const char __initconst *const clavis_module_acl[] = {
	 NULL
};
#else
extern const char __initconst *const clavis_module_acl[];
#endif

int __init clavis_keyring_init(void);
int clavis_sig_verify(const struct key *key, const struct public_key_signature *sig);
#endif /* _SECURITY_CLAVIS_H_ */
