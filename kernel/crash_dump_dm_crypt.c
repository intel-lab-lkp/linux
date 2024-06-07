// SPDX-License-Identifier: GPL-2.0-only
#include <linux/key.h>
#include <linux/keyctl.h>
#include <keys/user-type.h>
#include <linux/crash_dump.h>

#define KEY_NUM_MAX 128   /* maximum dm crypt keys */
#define KEY_SIZE_MAX 256  /* maximum dm crypt key size */

// The key scription has the format: cryptsetup:UUID 11+36+1(NULL)=48
#define KEY_DESC_LEN 48

static enum STATE_ENUM {
	FRESH = 0,
	INITIALIZED,
	RECORDED,
	LOADED,
} state;

static const char * const STATE_STR[] = {
	[FRESH] = "fresh",
	[INITIALIZED] = "initialized",
	[RECORDED] = "recorded",
	[LOADED] = "loaded"
};

static unsigned int key_count;
static size_t keys_header_size;

struct dm_crypt_key {
	unsigned int key_size;
	char key_desc[KEY_DESC_LEN];
	u8 data[KEY_SIZE_MAX];
};

static struct keys_header {
	unsigned int total_keys;
	struct dm_crypt_key keys[] __counted_by(total_keys);
} *keys_header;

static size_t get_keys_header_size(struct keys_header *keys_header,
				   size_t total_keys)
{
	return struct_size(keys_header, keys, total_keys);
}

/*
 * Let the kernel know the number of dm crypt keys and allocate memory to
 * initialize related structures.
 */
static int init(const char *buf)
{
	unsigned int total_keys;

	if (sscanf(buf, "init %u", &total_keys) != 1)
		return -EINVAL;

	if (total_keys > KEY_NUM_MAX) {
		kexec_dprintk(
			"Exceed the maximum number of keys (KEY_NUM_MAX=%u)\n",
			KEY_NUM_MAX);
		return -EINVAL;
	}

	keys_header_size = get_keys_header_size(keys_header, total_keys);
	key_count = 0;

	if (keys_header != NULL)
		kvfree(keys_header);

	keys_header = kzalloc(keys_header_size, GFP_KERNEL);
	if (!keys_header)
		return -ENOMEM;

	keys_header->total_keys = total_keys;
	state = INITIALIZED;
	return 0;
}

/*
 * Record the key description of a dm crypt key.
 */
static int record_key_desc(const char *buf, struct dm_crypt_key *dm_key)
{
	char key_desc[KEY_DESC_LEN];

	if (state != INITIALIZED) {
		kexec_dprintk("Please send the cmd 'init <KEY_NUM>' first\n");
		return -EINVAL;
	}

	if (sscanf(buf, "record %s", key_desc) != 1)
		return -EINVAL;

	if (key_count >= keys_header->total_keys) {
		kexec_dprintk("Already have %u keys", key_count);
		return -EINVAL;
	}

	strscpy(dm_key->key_desc, key_desc, KEY_DESC_LEN);
	kexec_dprintk("Key%d (%s) recorded\n", key_count, dm_key->key_desc);
	key_count++;

	if (key_count == keys_header->total_keys)
		state = RECORDED;

	return 0;
}

static int process_cmd(const char *buf, size_t count)
{
	if (strncmp(buf, "init ", 5) == 0)
		return init(buf);
	else if (strncmp(buf, "record ", 7) == 0 && count == KEY_DESC_LEN + 6)
		return record_key_desc(buf, &keys_header->keys[key_count]);

	return -EINVAL;
}

int crash_sysfs_dm_crypt_keys_write(const char *buf, size_t count)
{
	if (!is_kdump_kernel())
		return process_cmd(buf, count);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(crash_sysfs_dm_crypt_keys_write);

int crash_sysfs_dm_crypt_keys_read(char *buf)
{
	return sysfs_emit(buf, "%s\n", STATE_STR[state]);
}
EXPORT_SYMBOL_GPL(crash_sysfs_dm_crypt_keys_read);

static int read_key_from_user_keying(struct dm_crypt_key *dm_key)
{
	const struct user_key_payload *ukp;
	struct key *key;

	kexec_dprintk("Requesting key %s", dm_key->key_desc);
	key = request_key(&key_type_logon, dm_key->key_desc, NULL);

	if (IS_ERR(key)) {
		pr_warn("No such key %s\n", dm_key->key_desc);
		return PTR_ERR(key);
	}

	ukp = user_key_payload_locked(key);
	if (!ukp)
		return -EKEYREVOKED;

	memcpy(dm_key->data, ukp->data, ukp->datalen);
	dm_key->key_size = ukp->datalen;
	kexec_dprintk("Get dm crypt key (size=%u) %s: %8ph\n", dm_key->key_size,
		      dm_key->key_desc, dm_key->data);
	return 0;
}

static int build_keys_header(void)
{
	int i, r;

	for (i = 0; i < key_count; i++) {
		r = read_key_from_user_keying(&keys_header->keys[i]);
		if (r != 0) {
			pr_err("Failed to read key %s\n", keys_header->keys[i].key_desc);
			return r;
		}
	}

	return 0;
}

int crash_load_dm_crypt_keys(struct kimage *image)
{
	struct kexec_buf kbuf = {
		.image = image,
		.buf_min = 0,
		.buf_max = ULONG_MAX,
		.top_down = false,
		.random = true,
	};

	int r;

	if (state == FRESH)
		return 0;

	if (key_count != keys_header->total_keys) {
		kexec_dprintk("Only record %u keys (%u in total)\n", key_count,
			      keys_header->total_keys);
		return -EINVAL;
	}

	image->dm_crypt_keys_addr = 0;
	r = build_keys_header();
	if (r)
		return r;

	kbuf.buffer = keys_header;
	kbuf.bufsz = keys_header_size;

	kbuf.memsz = kbuf.bufsz;
	kbuf.buf_align = ELF_CORE_HEADER_ALIGN;
	kbuf.mem = KEXEC_BUF_MEM_UNKNOWN;
	r = kexec_add_buffer(&kbuf);
	if (r) {
		kvfree((void *)kbuf.buffer);
		return r;
	}
	state = LOADED;
	image->dm_crypt_keys_addr = kbuf.mem;
	image->dm_crypt_keys_sz = kbuf.bufsz;
	kexec_dprintk("Loaded dm crypt keys to kexec_buffer bufsz=0x%lx memsz=0x%lx\n",
		      kbuf.bufsz, kbuf.bufsz);

	return r;
}
