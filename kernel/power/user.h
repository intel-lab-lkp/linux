/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/suspend_ioctls.h>
#include <crypto/aead.h>
#include <crypto/aes.h>

#define SNAPSHOT_ENCRYPTION_KEY_SIZE AES_KEYSIZE_128
#define SNAPSHOT_AUTH_TAG_SIZE 16

/* Define the number of pages in a single AEAD encryption chunk. */
#define CHUNK_SIZE 16

struct snapshot_data {
	struct snapshot_handle handle;
	int swap;
	int mode;
	bool frozen;
	bool ready;
	bool platform_support;
	bool free_bitmaps;
	dev_t dev;

#if defined(CONFIG_ENCRYPTED_HIBERNATION)
	struct crypto_aead *aead_tfm;
	struct aead_request *aead_req;
	void *crypt_pages[CHUNK_SIZE];
	u8 auth_tag[SNAPSHOT_AUTH_TAG_SIZE];
	struct scatterlist sg[CHUNK_SIZE + 2]; /* Add room for AD and auth tag. */
	size_t crypt_offset;
	size_t crypt_size;
	u64 crypt_total;
	u64 nonce_low;
	u64 nonce_high;
	u8 encryption_key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring;
	u8 user_key[USWSUSP_USER_KEY_SIZE] __nonstring;
	bool user_key_valid;
	u64 meta_size;
#endif

};

extern struct snapshot_data snapshot_state;

/* kernel/power/snapenc.c routines */
#if defined(CONFIG_ENCRYPTED_HIBERNATION)

ssize_t snapshot_read_encrypted(struct snapshot_data *data,
				char __user *buf, size_t count, loff_t *offp);

ssize_t snapshot_write_encrypted(struct snapshot_data *data,
				 const char __user *buf, size_t count,
				 loff_t *offp);

void snapshot_teardown_encryption(struct snapshot_data *data);
int snapshot_get_encryption_key(struct snapshot_data *data,
				struct uswsusp_key_blob __user *key);

int snapshot_set_encryption_key(struct snapshot_data *data,
				struct uswsusp_key_blob __user *key);

int snapshot_set_user_key(struct snapshot_data *data,
			  struct uswsusp_user_key __user *key);

int snapshot_store_encryption_seed(const char *buf, size_t count);

loff_t snapshot_get_encrypted_image_size(loff_t raw_size);

int snapshot_finalize_decrypted_image(struct snapshot_data *data);

static inline bool snapshot_encryption_enabled(struct snapshot_data *data)
{
	return data->aead_tfm;
}

#else

static inline ssize_t snapshot_read_encrypted(struct snapshot_data *data,
					      char __user *buf, size_t count,
					      loff_t *offp)
{
	return -ENOTTY;
}

static inline ssize_t snapshot_write_encrypted(struct snapshot_data *data,
					       const char __user *buf,
					       size_t count, loff_t *offp)
{
	return -ENOTTY;
}

static inline void snapshot_teardown_encryption(struct snapshot_data *data) {}
static inline int snapshot_get_encryption_key(struct snapshot_data *data,
					      struct uswsusp_key_blob __user *key)
{
	return -ENOTTY;
}

static inline int snapshot_set_encryption_key(struct snapshot_data *data,
					      struct uswsusp_key_blob __user *key)
{
	return -ENOTTY;
}

static inline int snapshot_set_user_key(struct snapshot_data *data,
					struct uswsusp_user_key __user *key)
{
	return -ENOTTY;
}

static inline int snapshot_store_encryption_seed(const char *buf, size_t count)
{
	return -ENOTTY;
}

static inline loff_t snapshot_get_encrypted_image_size(loff_t raw_size)
{
	return raw_size;
}

static inline int snapshot_finalize_decrypted_image(struct snapshot_data *data)
{
	return -ENOTTY;
}

static inline bool snapshot_encryption_enabled(struct snapshot_data *data)
{
	return false;
}

#endif
