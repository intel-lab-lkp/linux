// SPDX-License-Identifier: GPL-2.0-only
#include <keys/user-type.h>
#include <linux/crash_dump.h>

#define KEY_NUM_MAX 128
#define KEY_SIZE_MAX 256

// The key scription has the format: cryptsetup:UUID 11+36+1(NULL)=48
#define KEY_DESC_LEN 48

static char *STATE_STR[] = {"fresh", "initialized", "recorded", "loaded"};
enum STATE_ENUM {
	FRESH = 0,
	INITIALIZED,
	RECORDED,
	LOADED,
} state;

static unsigned int key_count;
static size_t keys_header_size;

struct dm_crypt_key {
	unsigned int key_size;
	char key_desc[KEY_DESC_LEN];
	u8 data[KEY_SIZE_MAX];
};

struct keys_header {
	unsigned int key_count;
	struct dm_crypt_key keys[] __counted_by(key_count);
} *keys_header;

static size_t get_keys_header_size(struct keys_header *keys_header,
				   size_t key_count)
{
	return struct_size(keys_header, keys, key_count);
}

static int init(const char *buf)
{
	unsigned int total_keys;
	char dummy[5];

	if (sscanf(buf, "%4s %u", dummy, &total_keys) != 2)
		return -EINVAL;

	if (key_count > KEY_NUM_MAX) {
		pr_err("Exceed the maximum number of keys (KEY_NUM_MAX=%u)\n",
		       KEY_NUM_MAX);
		return -EINVAL;
	}

	keys_header_size = get_keys_header_size(keys_header, total_keys);
	key_count = 0;

	keys_header = kzalloc(keys_header_size, GFP_KERNEL);
	if (!keys_header)
		return -ENOMEM;

	keys_header->key_count = total_keys;
	state = INITIALIZED;
	return 0;
}

static int record_key_desc(const char *buf, struct dm_crypt_key *dm_key)
{
	char key_desc[KEY_DESC_LEN];
	char dummy[7];

	if (state != INITIALIZED)
		pr_err("Please send the cmd 'init <KEY_NUM>' first\n");

	if (sscanf(buf, "%6s %s", dummy, key_desc) != 2)
		return -EINVAL;

	if (key_count >= keys_header->key_count) {
		pr_warn("Already have %u keys", key_count);
		return -EINVAL;
	}

	strscpy(dm_key->key_desc, key_desc, KEY_DESC_LEN);
	pr_debug("Key%d (%s) recorded\n", key_count, dm_key->key_desc);
	key_count++;

	if (key_count == keys_header->key_count)
		state = RECORDED;

	return 0;
}

static int process_cmd(const char *buf, size_t count)
{
	if (strncmp(buf, "init ", 5) == 0)
		return init(buf);
	else if (strncmp(buf, "record ", 7) == 0)
		return record_key_desc(buf, &keys_header->keys[key_count]);

	return -EINVAL;
}

int crash_sysfs_dm_crypt_keys_write(const char *buf, size_t count)
{
	if (!is_kdump_kernel())
		return process_cmd(buf, count);
	return -EINVAL;
}
EXPORT_SYMBOL(crash_sysfs_dm_crypt_keys_write);

int crash_sysfs_dm_crypt_keys_read(char *buf)
{
	return sprintf(buf, "%s\n", STATE_STR[state]);
}
EXPORT_SYMBOL(crash_sysfs_dm_crypt_keys_read);
