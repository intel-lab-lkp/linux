// SPDX-License-Identifier: GPL-2.0-only
#include <linux/key.h>
#include <linux/keyctl.h>
#include <keys/user-type.h>
#include <linux/crash_dump.h>

unsigned long long dm_crypt_key_addr;
EXPORT_SYMBOL_GPL(dm_crypt_key_addr);

static int __init setup_dmcryptkey(char *arg)
{
	char *end;

	if (!arg)
		return -EINVAL;
	dm_crypt_key_addr = memparse(arg, &end);
	if (end > arg)
		return 0;

	dm_crypt_key_addr = 0;
	return -EINVAL;
}

early_param("dmcryptkey", setup_dmcryptkey);

/*
 * Architectures may override this function to read dm crypt key
 */
ssize_t __weak dm_crypt_key_read(char *buf, size_t count, u64 *ppos)
{
	struct kvec kvec = { .iov_base = buf, .iov_len = count };
	struct iov_iter iter;

	iov_iter_kvec(&iter, READ, &kvec, 1, count);
	return read_from_oldmem(&iter, count, ppos, false);
}

static int retrive_kdump_dm_crypt_key(u8 *buffer, unsigned int *sz)
{
	unsigned int key_size;
	size_t dm_crypt_keybuf_sz;
	unsigned int *size_ptr;
	char *dm_crypt_keybuf;
	u64 addr;
	int r;

	if (dm_crypt_key_addr == 0) {
		pr_debug("dm crypt key memory address inaccessible");
		return -EINVAL;
	}

	addr = dm_crypt_key_addr;

	/* Read dm crypt key size */
	r = dm_crypt_key_read((char *)&key_size, sizeof(unsigned int), &addr);

	if (r < 0)
		return r;

	pr_debug("Retrieve dm crypt key: size=%u\n", key_size);
	/* Read in dm cryptrkey */
	dm_crypt_keybuf_sz = sizeof(unsigned int) + key_size * sizeof(u8);
	dm_crypt_keybuf = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					      get_order(dm_crypt_keybuf_sz));
	if (!dm_crypt_keybuf)
		return -ENOMEM;

	addr = dm_crypt_key_addr;
	r = dm_crypt_key_read((char *)dm_crypt_keybuf, dm_crypt_keybuf_sz, &addr);

	if (r < 0)
		return r;
	size_ptr = (unsigned int *)dm_crypt_keybuf;
	memcpy(buffer, size_ptr + 1, key_size * sizeof(u8));
	pr_debug("Retrieve dm crypt key (size=%u): %48ph...\n", key_size, buffer);
	*sz = key_size;
	return 0;
}

static u8 *dm_crypt_key;
static unsigned int dm_crypt_key_size;

void wipe_dm_crypt_key(void)
{
	if (dm_crypt_key) {
		memset(dm_crypt_key, 0, dm_crypt_key_size * sizeof(u8));
		kfree(dm_crypt_key);
		dm_crypt_key = NULL;
	}
}

static void _wipe_dm_crypt_key(struct work_struct *dummy)
{
	wipe_dm_crypt_key();
}

static DECLARE_DELAYED_WORK(wipe_dm_crypt_key_work, _wipe_dm_crypt_key);

static unsigned __read_mostly wipe_key_delay = 120; /* 2 mins */

static int retore_dm_crypt_key_to_thread_keyring(const char *key_desc)
{
	key_ref_t keyring_ref, key_ref;
	int ret;

	/* find the target keyring (which must be writable) */
	keyring_ref = lookup_user_key(KEY_SPEC_USER_KEYRING, 0x01, KEY_NEED_WRITE);
	if (IS_ERR(keyring_ref)) {
		pr_alert("Failed to get keyring");
		return PTR_ERR(keyring_ref);
	}

	dm_crypt_key = kmalloc(128, GFP_KERNEL);
	ret = retrive_kdump_dm_crypt_key(dm_crypt_key, &dm_crypt_key_size);
	if (ret) {
		kfree(dm_crypt_key);
		return ret;
	}

	/* create or update the requested key and add it to the target keyring */
	key_ref = key_create_or_update(keyring_ref, "user", key_desc,
				       dm_crypt_key, dm_crypt_key_size,
				       KEY_USR_ALL, KEY_ALLOC_IN_QUOTA);

	if (!IS_ERR(key_ref)) {
		ret = key_ref_to_ptr(key_ref)->serial;
		key_ref_put(key_ref);
		pr_alert("Success adding key %s", key_desc);
	} else {
		ret = PTR_ERR(key_ref);
		pr_alert("Error when adding key");
	}

	key_ref_put(keyring_ref);
	return ret;
}

static int crash_save_temp_dm_crypt_key(const char *key_desc, size_t count)
{
	const struct user_key_payload *ukp;
	struct key *key;

	if (dm_crypt_key) {
		memset(dm_crypt_key, 0, dm_crypt_key_size * sizeof(u8));
		kfree(dm_crypt_key);
	}

	pr_debug("Requesting key %s", key_desc);
	key = request_key(&key_type_user, key_desc, NULL);

	if (IS_ERR(key)) {
		pr_debug("No such key %s", key_desc);
		return PTR_ERR(key);
	}

	ukp = user_key_payload_locked(key);
	if (!ukp)
		return -EKEYREVOKED;

	dm_crypt_key = kmalloc(ukp->datalen, GFP_KERNEL);
	if (!dm_crypt_key)
		return -ENOMEM;
	memcpy(dm_crypt_key, ukp->data, ukp->datalen);
	dm_crypt_key_size = ukp->datalen;
	pr_debug("dm crypt key (size=%u): %8ph\n", dm_crypt_key_size, dm_crypt_key);
	schedule_delayed_work(&wipe_dm_crypt_key_work,
			      round_jiffies_relative(wipe_key_delay * HZ));
	return 0;
}

int crash_sysfs_dm_crypt_key_write(const char *key_desc, size_t count)
{
	if (!is_kdump_kernel())
		return crash_save_temp_dm_crypt_key(key_desc, count);
	else
		return retore_dm_crypt_key_to_thread_keyring(key_desc);
}
EXPORT_SYMBOL(crash_sysfs_dm_crypt_key_write);

int crash_pass_temp_dm_crypt_key(void **addr, unsigned long *sz)
{
	unsigned long dm_crypt_key_sz;
	unsigned char *buf;
	unsigned int *size_ptr;

	if (!dm_crypt_key)
		return -EINVAL;

	dm_crypt_key_sz = sizeof(unsigned int) + dm_crypt_key_size * sizeof(u8);

	buf = vzalloc(dm_crypt_key_sz);
	if (!buf)
		return -ENOMEM;

	size_ptr = (unsigned int *)buf;
	memcpy(size_ptr, &dm_crypt_key_size, sizeof(unsigned int));
	memcpy(size_ptr + 1, dm_crypt_key, dm_crypt_key_size * sizeof(u8));
	*addr = buf;
	*sz = dm_crypt_key_sz;
	wipe_dm_crypt_key();
	return 0;
}

int crash_load_dm_crypt_key(struct kimage *image)
{
	int ret;
	struct kexec_buf kbuf = {
		.image = image,
		.buf_min = 0,
		.buf_max = ULONG_MAX,
		.top_down = false,
		.random = true,
	};

	image->dm_crypt_key_addr = 0;
	ret = crash_pass_temp_dm_crypt_key(&kbuf.buffer, &kbuf.bufsz);
	if (ret)
		return ret;

	kbuf.memsz = kbuf.bufsz;
	kbuf.buf_align = ELF_CORE_HEADER_ALIGN;
	kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	ret = kexec_add_buffer(&kbuf);
	if (ret) {
		vfree((void *)kbuf.buffer);
		return ret;
	}
	image->dm_crypt_key_addr = kbuf.mem;
	image->dm_crypt_key_sz = kbuf.bufsz;
	pr_debug("Loaded dm crypt key at 0x%lx bufsz=0x%lx memsz=0x%lx\n",
		 image->dm_crypt_key_addr, kbuf.bufsz, kbuf.bufsz);

	return ret;
}
