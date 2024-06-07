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
	REUSE,
} state;

static const char * const STATE_STR[] = {
	[FRESH] = "fresh",
	[INITIALIZED] = "initialized",
	[RECORDED] = "recorded",
	[LOADED] = "loaded",
	[REUSE] = "reuse"
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

unsigned long long dm_crypt_keys_addr;
EXPORT_SYMBOL_GPL(dm_crypt_keys_addr);

static int __init setup_dmcryptkeys(char *arg)
{
	char *end;

	if (!arg)
		return -EINVAL;
	dm_crypt_keys_addr = memparse(arg, &end);
	if (end > arg)
		return 0;

	dm_crypt_keys_addr = 0;
	return -EINVAL;
}

early_param("dmcryptkeys", setup_dmcryptkeys);

static size_t get_keys_header_size(struct keys_header *keys_header,
				   size_t total_keys)
{
	return struct_size(keys_header, keys, total_keys);
}

/*
 * Architectures may override this function to read dm crypt keys
 */
ssize_t __weak dm_crypt_keys_read(char *buf, size_t count, u64 *ppos)
{
	struct kvec kvec = { .iov_base = buf, .iov_len = count };
	struct iov_iter iter;

	iov_iter_kvec(&iter, READ, &kvec, 1, count);
	return read_from_oldmem(&iter, count, ppos, false);
}

static int add_key_to_keyring(struct dm_crypt_key *dm_key,
			      key_ref_t keyring_ref)
{
	key_ref_t key_ref;
	int r;

	/* create or update the requested key and add it to the target keyring */
	key_ref = key_create_or_update(keyring_ref, "user", dm_key->key_desc,
				       dm_key->data, dm_key->key_size,
				       KEY_USR_ALL, KEY_ALLOC_IN_QUOTA);

	if (!IS_ERR(key_ref)) {
		r = key_ref_to_ptr(key_ref)->serial;
		key_ref_put(key_ref);
		kexec_dprintk("Success adding key %s", dm_key->key_desc);
	} else {
		r = PTR_ERR(key_ref);
		kexec_dprintk("Error when adding key");
	}

	key_ref_put(keyring_ref);
	return r;
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

static void get_keys_from_kdump_reserved_memory(void)
{
	struct keys_header *keys_header_loaded;

	arch_kexec_unprotect_crashkres();

	keys_header_loaded = kmap_local_page(pfn_to_page(
		kexec_crash_image->dm_crypt_keys_addr >> PAGE_SHIFT));

	memcpy(keys_header, keys_header_loaded, keys_header_size);
	kunmap_local(keys_header_loaded);
	state = RECORDED;
	arch_kexec_protect_crashkres();
}

static int process_cmd(const char *buf, size_t count)
{
	if (strncmp(buf, "init ", 5) == 0)
		return init(buf);
	else if (strncmp(buf, "record ", 7) == 0 && count == KEY_DESC_LEN + 6)
		return record_key_desc(buf, &keys_header->keys[key_count]);
	else if (!strcmp(buf, "reuse")) {
		state = REUSE;
		get_keys_from_kdump_reserved_memory();
		return 0;
	}

	return -EINVAL;
}

static int restore_dm_crypt_keys_to_thread_keyring(const char *key_desc)
{
	struct dm_crypt_key *key;
	key_ref_t keyring_ref;
	u64 addr;

	/* find the target keyring (which must be writable) */
	keyring_ref =
		lookup_user_key(KEY_SPEC_USER_KEYRING, 0x01, KEY_NEED_WRITE);
	if (IS_ERR(keyring_ref)) {
		kexec_dprintk("Failed to get keyring %s\n", key_desc);
		return PTR_ERR(keyring_ref);
	}

	addr = dm_crypt_keys_addr;
	dm_crypt_keys_read((char *)&key_count, sizeof(key_count), &addr);
	if (key_count < 0 || key_count > KEY_NUM_MAX) {
		pr_info("Failed to read the number of dm_crypt keys\n");
		return -1;
	}

	kexec_dprintk("There are %u keys\n", key_count);
	addr = dm_crypt_keys_addr;

	keys_header_size = get_keys_header_size(keys_header, key_count);

	keys_header = kzalloc(keys_header_size, GFP_KERNEL);
	if (!keys_header)
		return -ENOMEM;

	dm_crypt_keys_read((char *)keys_header, keys_header_size, &addr);

	for (int i = 0; i < keys_header->total_keys; i++) {
		key = &keys_header->keys[i];
		kexec_dprintk("Get key (size=%u)\n", key->key_size);
		add_key_to_keyring(key, keyring_ref);
	}

	return 0;
}

int crash_sysfs_dm_crypt_keys_write(const char *buf, size_t count)
{
	if (!is_kdump_kernel())
		return process_cmd(buf, count);
	else
		return restore_dm_crypt_keys_to_thread_keyring(buf);
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
			kexec_dprintk("Failed to read key %s\n", keys_header->keys[i].key_desc);
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
	if (state != REUSE) {
		r = build_keys_header();
		if (r)
			return r;
	}

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
