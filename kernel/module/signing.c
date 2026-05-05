// SPDX-License-Identifier: GPL-2.0-or-later
/* Module signature checker
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/types.h>
#include <linux/verification.h>
#include "internal.h"

int module_sig_check(const void *mod, size_t mod_len, const void *sig, size_t sig_len)
{
	return verify_pkcs7_signature(mod, mod_len, sig, sig_len,
				      VERIFY_USE_SECONDARY_KEYRING,
				      VERIFYING_MODULE_SIGNATURE,
				      NULL, NULL);
}
