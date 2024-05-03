#include <linux/module.h>
#include <linux/key-type.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/parser.h>
#include <keys/user-type.h>
#include <crypto/kdf_sp800108.h>
#include <keys/request_key_auth-type.h>

#include "derived.h"
#include "../../integrity/ima/ima.h"

#define MIN_KEY_SIZE 1
#define MAX_KEY_SIZE 1024

enum { Opt_path, Opt_exe_csum, Opt_err };

static const match_table_t kdf_tokens = {
	{ Opt_path, "path" },
	{ Opt_exe_csum, "csum" },
	{ Opt_err, NULL }
};

static int get_current_exe_path(char *buf, size_t buf_len, char **path)
{
	struct file *exe_file = get_task_exe_file(current);
	if (!exe_file)
		return -EFAULT;

	*path = file_path(exe_file, buf, buf_len);
	fput(exe_file);

	return IS_ERR(*path) ? PTR_ERR(*path) : 0;
}

#ifdef CONFIG_IMA
static struct ima_digest_data *get_currect_exe_integrity(void)
{
	struct ima_iint_cache *iint;
	struct file *exe_file = get_task_exe_file(current);
	if (!exe_file)
		return NULL;

	iint = ima_iint_find(file_inode(exe_file));
	fput(exe_file);

	return iint ? iint->ima_hash : NULL;
}
#endif

static int tpm2_kdf_generate(int kdf_mix, u8 *out, size_t out_len)
{
	int ret;
	struct kvec kbuf_iov[4];
	char *path;
	kuid_t euid = current_euid();

	struct crypto_shash *tpm2_hash = crypto_alloc_shash(
		TPM2_HASH_IMPL_NAME, CRYPTO_ALG_INTERNAL, CRYPTO_ALG_INTERNAL);
	if (IS_ERR(tpm2_hash))
		return PTR_ERR(tpm2_hash);

	kbuf_iov[0].iov_base = &out_len;
	kbuf_iov[0].iov_len = sizeof(u32);

	kbuf_iov[1].iov_base = &euid;
	kbuf_iov[1].iov_len = sizeof(euid);

	switch (kdf_mix) {
	case Opt_path:
		kbuf_iov[2].iov_base = "path";
		kbuf_iov[2].iov_len = sizeof("path");

		path = kmalloc(PATH_MAX, GFP_KERNEL);
		if (!path) {
			ret = -ENOMEM;
			goto free_hash;
		}

		ret = get_current_exe_path(path, PATH_MAX,
					   (char **)&kbuf_iov[3].iov_base);
		if (ret) {
			kfree(path);
			goto free_hash;
		}

		kbuf_iov[3].iov_len = strlen(kbuf_iov[3].iov_base);
		break;
#ifdef CONFIG_IMA
	case Opt_exe_csum:
		kbuf_iov[2].iov_base = "csum";
		kbuf_iov[2].iov_len = sizeof("csum");

		struct ima_digest_data *digest = get_currect_exe_integrity();
		if (!digest) {
			ret = -EOPNOTSUPP;
			goto free_hash;
		}

		kbuf_iov[3].iov_base = digest->digest;
		kbuf_iov[3].iov_len = digest->length;
		break;
#endif
	}

	ret = crypto_kdf108_ctr_generate(tpm2_hash, kbuf_iov,
					 ARRAY_SIZE(kbuf_iov), out, out_len);
	switch (kdf_mix) {
	case Opt_path:
		kfree(path);
	}

free_hash:
	crypto_free_shash(tpm2_hash);
	return ret;
}

static int derived_preparse(struct key_preparsed_payload *prep)
{
	int ret;
	char *opts, *cur, *opt;
	int kdf_mix;
	unsigned long keylen;
	substring_t args[MAX_OPT_ARGS];
	struct user_key_payload *upayload;
	size_t optslen = prep->datalen;

	if (!prep->data || !prep->datalen)
		return -EINVAL;

	opts = cur = kmalloc(optslen + 1, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;
	opts[optslen] = 0;
	memcpy(opts, prep->data, optslen);

	opt = strsep(&cur, " \t");
	if (!opt) {
		ret = -EINVAL;
		goto free_opts;
	}

	ret = kstrtoul(opts, 10, &keylen);
	if (ret)
		goto free_opts;

	if (keylen < MIN_KEY_SIZE || keylen > MAX_KEY_SIZE) {
		ret = -EINVAL;
		goto free_opts;
	}

	if (!cur) {
		ret = -EINVAL;
		goto free_opts;
	}

	kdf_mix = match_token(cur, kdf_tokens, args);
	switch (kdf_mix) {
#ifndef CONFIG_IMA
	case Opt_exe_csum:
		ret = -EOPNOTSUPP;
		goto free_opts;
#endif
	case Opt_err:
		ret = -EINVAL;
		goto free_opts;
	}

	upayload = kmalloc(sizeof(*upayload) + keylen, GFP_KERNEL);
	if (!upayload) {
		ret = -ENOMEM;
		goto free_opts;
	}

	ret = tpm2_kdf_generate(kdf_mix, upayload->data, keylen);
	if (ret)
		goto free_payload;

	prep->quotalen = keylen;
	prep->payload.data[0] = upayload;
	upayload->datalen = keylen;
	goto free_opts;

free_payload:
	kfree(upayload);
free_opts:
	kfree(opts);
	return ret;
}

static struct key_type key_type_derived = {
	.name = "derived",
	.preparse = derived_preparse,
	.free_preparse = user_free_preparse,
	.instantiate = generic_key_instantiate,
	.revoke = user_revoke,
	.destroy = user_destroy,
	.describe = user_describe,
	.read = user_read,
};

static int __init init_derived(void)
{
	int ret;

	ret = register_tpm2_shash();
	if (ret)
		return ret;

	ret = register_key_type(&key_type_derived);
	if (ret)
		unregister_tpm2_shash();

	return ret;
}

static void __exit cleanup_derived(void)
{
	unregister_key_type(&key_type_derived);
	unregister_tpm2_shash();
}

late_initcall(init_derived);
module_exit(cleanup_derived);

MODULE_LICENSE("GPL");
