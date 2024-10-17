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

#ifdef CONFIG_EFI
int clavis_efi_param(struct asymmetric_key_id *kid, int len);
#else
static inline int __init clavis_efi_param(struct asymmetric_key_id *kid, int len)
{
	return -EINVAL;
}
#endif

int __init clavis_keyring_init(void);
int clavis_sig_verify(const struct key *key, const struct public_key_signature *sig);
#ifdef CONFIG_SECURITY_CLAVIS_KUNIT_TEST
extern void key_type_put(struct key_type *ktype);
extern struct key_type *key_type_lookup(const char *type);
extern long keyctl_update_key(key_serial_t id, const void __user *_payload, size_t plen);
extern struct key * (* const clavis_keyring_get_fn_ptr)(void);
extern int (* const key_acl_preparse_fn_ptr)(struct key_preparsed_payload *prep);
extern void (* const clavis_add_acl_fn_ptr)(const char *const *skid_list, struct key *keyring);

extern struct key *
	(*const keyring_alloc_fn_ptr)(const char *desc, struct key_restriction *restriction);

extern struct key_restriction *
	(* const restriction_alloc_fn_ptr)(key_restrict_link_func_t check_func);

extern struct asymmetric_key_id  *
	(* const parse_boot_param_fn_ptr)(char *kid, struct asymmetric_key_id *akid,
					  int akid_max_len);

extern int
	(* const pkcs7_preparse_content_fn_ptr)(void *ctx, const void *data, size_t len,
						size_t asn1hdrlen);

extern bool (* const clavis_acl_enforced_fn_ptr)(void);
#endif
#endif /* _SECURITY_CLAVIS_H_ */
