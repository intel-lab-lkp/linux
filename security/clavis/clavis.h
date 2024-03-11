/* SPDX-License-Identifier: GPL-2.0 */

struct key;
struct public_key_signature;
struct asymmetric_key_id;

#ifdef CONFIG_EFI
int __init clavis_efi_param(struct asymmetric_key_id *kid, int len);
#else
static inline int __init clavis_efi_param(struct asymmetric_key_id *kid, int len)
{
	return -EINVAL;
}
#endif

int clavis_sig_verify(const struct key *key, const struct public_key_signature *sig);
