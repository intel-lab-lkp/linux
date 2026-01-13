// SPDX-License-Identifier: GPL-2.0+
/*
 * Module signature checker
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/module_signature.h>
#include <asm/byteorder.h>

/**
 * mod_check_sig - check that the given signature is sane
 *
 * @ms:		Signature to check.
 * @file_len:	Size of the file to which @ms is appended.
 * @name:	What is being checked. Used for error messages.
 */
static int mod_check_sig(const struct module_signature *ms, size_t file_len, const char *name)
{
	if (be32_to_cpu(ms->sig_len) >= file_len - sizeof(*ms))
		return -EBADMSG;

	if (ms->algo != 0 ||
	    ms->hash != 0 ||
	    ms->signer_len != 0 ||
	    ms->key_id_len != 0 ||
	    ms->__pad[0] != 0 ||
	    ms->__pad[1] != 0 ||
	    ms->__pad[2] != 0) {
		pr_err("%s: signature info has unexpected non-zero params\n",
		       name);
		return -EBADMSG;
	}

	return 0;
}

int mod_split_sig(const void *buf, size_t *buf_len, bool mangled,
		  enum pkey_id_type *sig_type, size_t *sig_len, const u8 **sig, const char *name)
{
	const unsigned long markerlen = sizeof(MODULE_SIG_STRING) - 1;
	struct module_signature ms;
	size_t modlen = *buf_len;
	int ret;

	/*
	 * Do not allow mangled modules as a module with version information
	 * removed is no longer the module that was signed.
	 */
	if (!mangled &&
	    *buf_len > markerlen &&
	    memcmp(buf + modlen - markerlen, MODULE_SIG_STRING, markerlen) == 0) {
		/* We truncate the module to discard the signature */
		modlen -= markerlen;
	}

	if (modlen <= sizeof(ms))
		return -EBADMSG;

	memcpy(&ms, buf + (modlen - sizeof(ms)), sizeof(ms));

	ret = mod_check_sig(&ms, modlen, name);
	if (ret)
		return ret;

	*sig_type = ms.id_type;
	*sig_len = be32_to_cpu(ms.sig_len);
	modlen -= *sig_len + sizeof(ms);
	*buf_len = modlen;
	*sig = buf + modlen;

	return 0;
}
