/* SPDX-License-Identifier: GPL-2.0 */

struct asymmetric_key_id;

#ifdef CONFIG_EFI
int __init clavis_efi_param(struct asymmetric_key_id *kid, int len);
#else
static inline int __init clavis_efi_param(struct asymmetric_key_id *kid, int len)
{
	return -EINVAL;
}
#endif
