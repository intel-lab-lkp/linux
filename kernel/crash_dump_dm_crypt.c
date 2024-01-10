// SPDX-License-Identifier: GPL-2.0-only
#include <keys/user-type.h>
#include <linux/crash_dump.h>

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
	return -EINVAL;
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
