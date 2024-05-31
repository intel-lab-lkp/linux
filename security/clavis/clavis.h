/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SECURITY_CLAVIS_H_
#define _SECURITY_CLAVIS_H_

struct asymmetric_key_id;

#ifdef CONFIG_EFI
int clavis_efi_param(struct asymmetric_key_id *kid, int len);
#else
static inline int __init clavis_efi_param(struct asymmetric_key_id *kid, int len)
{
	return -EINVAL;
}
#endif

#endif /* _SECURITY_CLAVIS_H_ */
