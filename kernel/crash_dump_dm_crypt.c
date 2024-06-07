// SPDX-License-Identifier: GPL-2.0-only
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
