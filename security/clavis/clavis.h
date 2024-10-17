/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SECURITY_CLAVIS_H_
#define _SECURITY_CLAVIS_H_
#include <keys/asymmetric-type.h>

/* Max length for the asymmetric key id contained on the boot param */
#define CLAVIS_BIN_KID_MAX   32
#define CLAVIS_ASCII_KID_MAX 64

struct asymmetric_setup_kid {
	struct asymmetric_key_id id;
	unsigned char data[CLAVIS_BIN_KID_MAX];
};

#ifndef CONFIG_SYSTEM_TRUSTED_KEYRING
const char __initconst *const clavis_module_acl[] = {
	 NULL
};
#else
extern const char __initconst *const clavis_module_acl[];
#endif

#endif /* _SECURITY_CLAVIS_H_ */
