// SPDX-License-Identifier: GPL-2.0-or-later
/* Module signature checker
 *
 * Copyright (C) 2012 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/module_signature.h>
#include <linux/string.h>
#include <linux/verification.h>
#include <linux/security.h>
#include <crypto/public_key.h>
#include <uapi/linux/module.h>
#include "internal.h"

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "module."

int module_sig_check_wait;

static bool sig_enforce = IS_ENABLED(CONFIG_MODULE_SIG_FORCE);
module_param(sig_enforce, bool_enable_only, 0644);

/*
 * Export sig_enforce kernel cmdline parameter to allow other subsystems rely
 * on that instead of directly to CONFIG_MODULE_SIG_FORCE config.
 */
bool is_module_sig_enforced(void)
{
	return sig_enforce;
}
EXPORT_SYMBOL(is_module_sig_enforced);

void set_module_sig_enforced(void)
{
	sig_enforce = true;
}

/*
 * test thing to enable sig enforcing later in boot sequence
 */
static int __init module_sig_check_wait_arg(char *str)
{
	return kstrtoint(str, 0, &module_sig_check_wait);
}
__setup("module_sig_check_wait=", module_sig_check_wait_arg);

/*
 * securityfs entry to disable module_sig_check_wait, and start enforcing modules signature check
 */
static ssize_t module_sig_check_wait_read(struct file *file, char __user *buf, size_t count,
					  loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos,
				       module_sig_check_wait == 1 ? "1\n" : "0\n", 2);
}

static ssize_t module_sig_check_wait_write(struct file *file, const char __user *buf,
					   size_t n, loff_t *ppos)
{
	int tmp;

	if (kstrtoint_from_user(buf, n, 10, &tmp))
		return -EINVAL;
	if (tmp != 0) {
		pr_info("module_sig_check_wait can be only disabled!\n");
		return -EINVAL;
	}
	pr_info("module_sig_check_wait disabled!\n");
	module_sig_check_wait = tmp;

	return n;
}

static const struct file_operations module_sig_check_wait_ops = {
	.read  = module_sig_check_wait_read,
	.write = module_sig_check_wait_write,
};

static int __init module_sig_check_wait_secfs_init(void)
{
	struct dentry *dentry;

	dentry = securityfs_create_file("module_sig_check_wait", 0644, NULL, NULL,
					&module_sig_check_wait_ops);
	return PTR_ERR_OR_ZERO(dentry);
}

core_initcall(module_sig_check_wait_secfs_init);

/*
 * Verify the signature on a module.
 */
int mod_verify_sig(const void *mod, struct load_info *info)
{
	struct module_signature ms;
	size_t sig_len, modlen = info->len;
	int ret;

	pr_devel("==>%s(,%zu)\n", __func__, modlen);

	if (modlen <= sizeof(ms))
		return -EBADMSG;

	memcpy(&ms, mod + (modlen - sizeof(ms)), sizeof(ms));

	ret = mod_check_sig(&ms, modlen, "module");
	if (ret)
		return ret;

	sig_len = be32_to_cpu(ms.sig_len);
	modlen -= sig_len + sizeof(ms);
	info->len = modlen;

	return verify_pkcs7_signature(mod, modlen, mod + modlen, sig_len,
				      VERIFY_USE_SECONDARY_KEYRING,
				      VERIFYING_MODULE_SIGNATURE,
				      NULL, NULL);
}

int module_sig_check(struct load_info *info, int flags)
{
	if (module_sig_check_wait)
		return 0;
	int err = -ENODATA;
	const unsigned long markerlen = sizeof(MODULE_SIG_STRING) - 1;
	const char *reason;
	const void *mod = info->hdr;
	bool mangled_module = flags & (MODULE_INIT_IGNORE_MODVERSIONS |
				       MODULE_INIT_IGNORE_VERMAGIC);
	/*
	 * Do not allow mangled modules as a module with version information
	 * removed is no longer the module that was signed.
	 */
	if (!mangled_module &&
	    info->len > markerlen &&
	    memcmp(mod + info->len - markerlen, MODULE_SIG_STRING, markerlen) == 0) {
		/* We truncate the module to discard the signature */
		info->len -= markerlen;
		err = mod_verify_sig(mod, info);
		if (!err) {
			info->sig_ok = true;
			return 0;
		}
	}

	/*
	 * We don't permit modules to be loaded into the trusted kernels
	 * without a valid signature on them, but if we're not enforcing,
	 * certain errors are non-fatal.
	 */
	switch (err) {
	case -ENODATA:
		reason = "unsigned module";
		break;
	case -ENOPKG:
		reason = "module with unsupported crypto";
		break;
	case -ENOKEY:
		reason = "module with unavailable key";
		break;

	default:
		/*
		 * All other errors are fatal, including lack of memory,
		 * unparseable signatures, and signature check failures --
		 * even if signatures aren't required.
		 */
		return err;
	}

	if (is_module_sig_enforced()) {
		pr_notice("Loading of %s is rejected\n", reason);
		return -EKEYREJECTED;
	}

	return security_locked_down(LOCKDOWN_MODULE_SIGNATURE);
}
