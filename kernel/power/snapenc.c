// SPDX-License-Identifier: GPL-2.0-only
/* This file provides encryption support for system snapshots. */

#include <linux/crypto.h>
#include <crypto/aead.h>
#include <crypto/gcm.h>
#include <crypto/sha2.h>
#include <linux/hex.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "power.h"
#include "user.h"

#define SNAPSHOT_SEED_SIZE SHA256_DIGEST_SIZE
#define SNAPSHOT_KEY_BLOB_VERSION 1

static const u8 snapshot_key_blob_magic[8] = {
	'S', 'W', 'S', 'U', 'S', 'P', 'K', '1',
};

struct snapshot_wrapped_key {
	u8 magic[sizeof(snapshot_key_blob_magic)];
	__le32 version;
	u8 wrap_nonce[GCM_AES_IV_SIZE] __nonstring;
	u8 encrypted_key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring;
	u8 tag[SNAPSHOT_AUTH_TAG_SIZE] __nonstring;
} __packed;

static DEFINE_MUTEX(snapshot_seed_mutex);
static u8 snapshot_seed[SNAPSHOT_SEED_SIZE] __nonstring;
static bool snapshot_seed_valid;

int snapshot_store_encryption_seed(const char *buf, size_t count)
{
	u8 seed[SNAPSHOT_SEED_SIZE] __nonstring;
	size_t len = count;
	int ret;

	if (len && buf[len - 1] == '\n')
		len--;
	if (len != SNAPSHOT_SEED_SIZE * 2)
		return -EINVAL;

	ret = hex2bin(seed, buf, sizeof(seed));
	if (ret)
		return ret;

	mutex_lock(&snapshot_seed_mutex);
	if (snapshot_seed_valid) {
		ret = memcmp(snapshot_seed, seed, sizeof(seed)) ? -EPERM : 0;
		goto out;
	}

	memcpy(snapshot_seed, seed, sizeof(seed));
	snapshot_seed_valid = true;
	pr_info("PM: hibernate: snapshot encryption seed locked\n");

out:
	mutex_unlock(&snapshot_seed_mutex);
	memzero_explicit(seed, sizeof(seed));
	return ret;
}

static bool snapshot_copy_encryption_seed(u8 seed[SNAPSHOT_SEED_SIZE])
{
	bool valid;

	mutex_lock(&snapshot_seed_mutex);
	valid = snapshot_seed_valid;
	if (valid)
		memcpy(seed, snapshot_seed, SNAPSHOT_SEED_SIZE);
	mutex_unlock(&snapshot_seed_mutex);

	return valid;
}

static int snapshot_derive_wrapping_key(u8 key[SNAPSHOT_ENCRYPTION_KEY_SIZE])
{
	static const char label[] = "Linux hibernate snapshot key wrap v1";
	u8 digest[SHA256_DIGEST_SIZE];
	u8 seed[SNAPSHOT_SEED_SIZE] __nonstring;
	struct sha256_ctx sha256_ctx;

	if (!snapshot_copy_encryption_seed(seed))
		return -ENOKEY;

	sha256_init(&sha256_ctx);
	sha256_update(&sha256_ctx, label, strlen(label));
	sha256_update(&sha256_ctx, seed, sizeof(seed));
	sha256_final(&sha256_ctx, digest);

	memcpy(key, digest, SNAPSHOT_ENCRYPTION_KEY_SIZE);
	memzero_explicit(seed, sizeof(seed));
	memzero_explicit(digest, sizeof(digest));
	return 0;
}

static int snapshot_crypt_wrapped_key(struct snapshot_wrapped_key *wrapped,
				      u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE],
				      bool encrypt)
{
	u8 wrap_key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring;
	u8 buf[SNAPSHOT_ENCRYPTION_KEY_SIZE + SNAPSHOT_AUTH_TAG_SIZE] __nonstring;
	struct crypto_aead *tfm;
	struct aead_request *req;
	struct scatterlist sg;
	DECLARE_CRYPTO_WAIT(wait);
	int rc;

	rc = snapshot_derive_wrapping_key(wrap_key);
	if (rc)
		return rc;

	tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		rc = PTR_ERR(tfm);
		goto out_key;
	}

	rc = crypto_aead_setkey(tfm, wrap_key, sizeof(wrap_key));
	if (rc)
		goto out_tfm;

	rc = crypto_aead_setauthsize(tfm, SNAPSHOT_AUTH_TAG_SIZE);
	if (rc)
		goto out_tfm;

	req = aead_request_alloc(tfm, GFP_KERNEL);
	if (!req) {
		rc = -ENOMEM;
		goto out_tfm;
	}

	if (encrypt) {
		get_random_bytes(wrapped->wrap_nonce, sizeof(wrapped->wrap_nonce));
		memcpy(buf, image_key, SNAPSHOT_ENCRYPTION_KEY_SIZE);
	} else {
		memcpy(buf, wrapped->encrypted_key, SNAPSHOT_ENCRYPTION_KEY_SIZE);
		memcpy(buf + SNAPSHOT_ENCRYPTION_KEY_SIZE, wrapped->tag,
		       SNAPSHOT_AUTH_TAG_SIZE);
	}

	sg_init_one(&sg, buf, sizeof(buf));
	aead_request_set_callback(req, 0, crypto_req_done, &wait);
	aead_request_set_ad(req, 0);
	aead_request_set_crypt(req, &sg, &sg,
			       encrypt ? SNAPSHOT_ENCRYPTION_KEY_SIZE :
					 sizeof(buf),
			       wrapped->wrap_nonce);

	rc = crypto_wait_req(encrypt ? crypto_aead_encrypt(req) :
				       crypto_aead_decrypt(req),
			     &wait);
	if (rc)
		goto out_req;

	if (encrypt) {
		memcpy(wrapped->encrypted_key, buf, SNAPSHOT_ENCRYPTION_KEY_SIZE);
		memcpy(wrapped->tag, buf + SNAPSHOT_ENCRYPTION_KEY_SIZE,
		       SNAPSHOT_AUTH_TAG_SIZE);
	} else {
		memcpy(image_key, buf, SNAPSHOT_ENCRYPTION_KEY_SIZE);
	}

out_req:
	aead_request_free(req);
out_tfm:
	crypto_free_aead(tfm);
out_key:
	memzero_explicit(buf, sizeof(buf));
	memzero_explicit(wrap_key, sizeof(wrap_key));
	return rc;
}

static int snapshot_wrap_image_key(const u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE],
				   struct snapshot_wrapped_key *wrapped)
{
	u8 key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring;
	int rc;

	memset(wrapped, 0, sizeof(*wrapped));
	memcpy(wrapped->magic, snapshot_key_blob_magic, sizeof(wrapped->magic));
	wrapped->version = cpu_to_le32(SNAPSHOT_KEY_BLOB_VERSION);

	memcpy(key, image_key, sizeof(key));
	rc = snapshot_crypt_wrapped_key(wrapped, key, true);
	memzero_explicit(key, sizeof(key));
	return rc;
}

static int snapshot_unwrap_image_key(const struct snapshot_wrapped_key *wrapped,
				     u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE])
{
	struct snapshot_wrapped_key tmp;
	int rc;

	if (memcmp(wrapped->magic, snapshot_key_blob_magic,
		   sizeof(snapshot_key_blob_magic)))
		return -EINVAL;
	if (le32_to_cpu(wrapped->version) != SNAPSHOT_KEY_BLOB_VERSION)
		return -EINVAL;

	tmp = *wrapped;
	rc = snapshot_crypt_wrapped_key(&tmp, image_key, false);
	memzero_explicit(&tmp, sizeof(tmp));
	return rc;
}

static int snapshot_install_image_key(struct snapshot_data *data,
				      const u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE])
{
	int rc;

	memcpy(data->encryption_key, image_key, sizeof(data->encryption_key));
	rc = crypto_aead_setkey(data->aead_tfm, data->encryption_key,
				sizeof(data->encryption_key));
	if (rc)
		memzero_explicit(data->encryption_key,
				 sizeof(data->encryption_key));

	return rc;
}

/* Derive a key from the kernel and user keys for data encryption. */
static int snapshot_use_user_key(struct snapshot_data *data)
{
	u8 digest[SHA256_DIGEST_SIZE];
	struct sha256_ctx sha256_ctx;
	int rc;

	/*
	 * Hash the kernel key and the user key together. This folds in the user
	 * key, but not in a way that gives the user mode predictable control
	 * over the key bits.
	 */
	sha256_init(&sha256_ctx);

	sha256_update(&sha256_ctx, data->encryption_key,
		      SNAPSHOT_ENCRYPTION_KEY_SIZE);
	sha256_update(&sha256_ctx, data->user_key, sizeof(data->user_key));
	sha256_final(&sha256_ctx, digest);

	BUILD_BUG_ON(SNAPSHOT_ENCRYPTION_KEY_SIZE > SHA256_DIGEST_SIZE);

	rc = crypto_aead_setkey(data->aead_tfm,
				digest,
				SNAPSHOT_ENCRYPTION_KEY_SIZE);
	memzero_explicit(digest, sizeof(digest));

	return rc;
}

/* Check to see if it's time to switch to the user key, and do it if so. */
static int snapshot_check_user_key_switch(struct snapshot_data *data)
{
	if (data->user_key_valid && data->meta_size &&
	    data->crypt_total == data->meta_size) {
		return snapshot_use_user_key(data);
	}

	return 0;
}

/* Encrypt more data from the snapshot into the staging area. */
static int snapshot_encrypt_refill(struct snapshot_data *data)
{
	struct aead_request *req = data->aead_req;
	u8 nonce[GCM_AES_IV_SIZE];
	DECLARE_CRYPTO_WAIT(wait);
	size_t total = 0;
	int pg_idx;
	int res;

	if (data->crypt_total == 0) {
		data->meta_size = snapshot_get_meta_page_count() << PAGE_SHIFT;
	} else {
		res = snapshot_check_user_key_switch(data);
		if (res)
			return res;
	}

	/*
	 * The first buffer is the associated data, set to the offset to prevent
	 * attacks that rearrange chunks.
	 */
	sg_set_buf(&data->sg[0], &data->crypt_total, sizeof(data->crypt_total));

	/* Load the crypt buffer with snapshot pages. */
	for (pg_idx = 0; pg_idx < CHUNK_SIZE; pg_idx++) {
		void *buf = data->crypt_pages[pg_idx];

		/* Stop at the meta page boundary to potentially switch keys. */
		if (total &&
		    ((data->crypt_total + total) == data->meta_size))
			break;

		res = snapshot_read_next(&data->handle);
		if (res < 0)
			return res;
		if (res == 0)
			break;

		WARN_ON(res != PAGE_SIZE);

		/*
		 * Copy the page into the staging area. A future optimization
		 * could potentially skip this copy for lowmem pages.
		 */
		memcpy(buf, data_of(data->handle), PAGE_SIZE);
		sg_set_buf(&data->sg[1 + pg_idx], buf, PAGE_SIZE);
		total += PAGE_SIZE;
	}

	sg_set_buf(&data->sg[1 + pg_idx], &data->auth_tag, SNAPSHOT_AUTH_TAG_SIZE);
	aead_request_set_callback(req, 0, crypto_req_done, &wait);
	/*
	 * Use incrementing nonces for each chunk, since a 64 bit value won't
	 * roll into re-use for any given hibernate image.
	 */
	memcpy(&nonce[0], &data->nonce_low, sizeof(data->nonce_low));
	memcpy(&nonce[sizeof(data->nonce_low)],
	       &data->nonce_high,
	       sizeof(nonce) - sizeof(data->nonce_low));

	data->nonce_low += 1;
	/* Total does not include AAD or the auth tag. */
	aead_request_set_crypt(req, data->sg, data->sg, total, nonce);
	res = crypto_wait_req(crypto_aead_encrypt(req), &wait);
	if (res)
		return res;

	data->crypt_size = total;
	data->crypt_total += total;
	return 0;
}

/* Decrypt data from the staging area and push it to the snapshot. */
static int snapshot_decrypt_drain(struct snapshot_data *data)
{
	struct aead_request *req = data->aead_req;
	u8 nonce[GCM_AES_IV_SIZE];
	DECLARE_CRYPTO_WAIT(wait);
	int page_count;
	size_t total;
	int pg_idx;
	int res;

	/* Set up the associated data. */
	sg_set_buf(&data->sg[0], &data->crypt_total, sizeof(data->crypt_total));

	/*
	 * Get the number of full pages, which could be short at the end. There
	 * should also be a tag at the end, so the offset won't be an even page.
	 */
	page_count = data->crypt_offset >> PAGE_SHIFT;
	total = page_count << PAGE_SHIFT;
	if (total == 0 || total == data->crypt_offset)
		return -EINVAL;

	/*
	 * Load the sg list with the crypt buffer. Inline decrypt back into the
	 * staging buffer. A future optimization could decrypt directly into
	 * lowmem pages.
	 */
	for (pg_idx = 0; pg_idx < page_count; pg_idx++)
		sg_set_buf(&data->sg[1 + pg_idx], data->crypt_pages[pg_idx], PAGE_SIZE);

	/*
	 * It's possible this is the final decrypt, or the final decrypt of the
	 * meta region, and there are fewer than CHUNK_SIZE pages. If this is
	 * the case we would have just written the auth tag into the first few
	 * bytes of a new page. Copy to the tag if so.
	 */
	if (page_count < CHUNK_SIZE &&
	    (data->crypt_offset - total) == sizeof(data->auth_tag)) {
		memcpy(data->auth_tag, data->crypt_pages[pg_idx],
		       sizeof(data->auth_tag));
	} else if (data->crypt_offset !=
		   ((CHUNK_SIZE << PAGE_SHIFT) + SNAPSHOT_AUTH_TAG_SIZE)) {
		return -EINVAL;
	}

	sg_set_buf(&data->sg[1 + pg_idx], &data->auth_tag, SNAPSHOT_AUTH_TAG_SIZE);
	aead_request_set_callback(req, 0, crypto_req_done, &wait);
	memcpy(&nonce[0], &data->nonce_low, sizeof(data->nonce_low));
	memcpy(&nonce[sizeof(data->nonce_low)],
	       &data->nonce_high,
	       sizeof(nonce) - sizeof(data->nonce_low));

	data->nonce_low += 1;
	aead_request_set_crypt(req, data->sg, data->sg, total + SNAPSHOT_AUTH_TAG_SIZE, nonce);
	res = crypto_wait_req(crypto_aead_decrypt(req), &wait);
	if (res)
		return res;

	data->crypt_size = 0;
	data->crypt_offset = 0;

	/* Push the decrypted pages further down the stack. */
	total = 0;
	for (pg_idx = 0; pg_idx < page_count; pg_idx++) {
		void *buf = data->crypt_pages[pg_idx];

		res = snapshot_write_next(&data->handle);
		if (res < 0)
			return res;
		if (res == 0)
			break;

		if (!data_of(data->handle))
			return -EINVAL;

		WARN_ON(res != PAGE_SIZE);

		/* Copy the decrypted page into the snapshot image. */
		memcpy(data_of(data->handle), buf, PAGE_SIZE);
		total += PAGE_SIZE;
	}

	if (data->crypt_total == 0)
		data->meta_size = snapshot_get_meta_page_count() << PAGE_SHIFT;

	data->crypt_total += total;
	res = snapshot_check_user_key_switch(data);
	if (res)
		return res;

	return 0;
}

static ssize_t snapshot_read_next_encrypted(struct snapshot_data *data,
					    void **buf)
{
	size_t tag_off;

	/* Refill the encrypted buffer if it's empty. */
	if (data->crypt_size == 0 ||
	    (data->crypt_offset >=
	     (data->crypt_size + SNAPSHOT_AUTH_TAG_SIZE))) {
		int rc;

		data->crypt_size = 0;
		data->crypt_offset = 0;
		rc = snapshot_encrypt_refill(data);
		if (rc < 0)
			return rc;
	}

	/* Return data pages if the offset is in that region. */
	if (data->crypt_offset < data->crypt_size) {
		size_t pg_idx = data->crypt_offset >> PAGE_SHIFT;
		size_t pg_off = data->crypt_offset & (PAGE_SIZE - 1);
		*buf = data->crypt_pages[pg_idx] + pg_off;
		return PAGE_SIZE - pg_off;
	}

	/* Use offsets just beyond the size to return the tag. */
	tag_off = data->crypt_offset - data->crypt_size;
	if (tag_off > SNAPSHOT_AUTH_TAG_SIZE)
		tag_off = SNAPSHOT_AUTH_TAG_SIZE;

	*buf = data->auth_tag + tag_off;
	return SNAPSHOT_AUTH_TAG_SIZE - tag_off;
}

static ssize_t snapshot_write_next_encrypted(struct snapshot_data *data,
					     void **buf)
{
	size_t tag_off;

	/* Return data pages if the offset is in that region. */
	if (data->crypt_offset < (PAGE_SIZE * CHUNK_SIZE)) {
		size_t pg_idx = data->crypt_offset >> PAGE_SHIFT;
		size_t pg_off = data->crypt_offset & (PAGE_SIZE - 1);
		size_t size_avail = PAGE_SIZE;
		*buf = data->crypt_pages[pg_idx] + pg_off;

		/*
		 * If this is the boundary where the meta pages end, then just
		 * return enough for the auth tag.
		 */
		if (data->meta_size &&
		    data->crypt_total < data->meta_size) {
			u64 total_done =
				data->crypt_total + data->crypt_offset;

			if (total_done >= data->meta_size &&
			    (total_done <
			     (data->meta_size + SNAPSHOT_AUTH_TAG_SIZE))) {
				size_avail = SNAPSHOT_AUTH_TAG_SIZE;
			}
		}

		return size_avail - pg_off;
	}

	/* Use offsets just beyond the size to return the tag. */
	tag_off = data->crypt_offset - (PAGE_SIZE * CHUNK_SIZE);
	if (tag_off > SNAPSHOT_AUTH_TAG_SIZE)
		tag_off = SNAPSHOT_AUTH_TAG_SIZE;

	*buf = data->auth_tag + tag_off;
	return SNAPSHOT_AUTH_TAG_SIZE - tag_off;
}

ssize_t snapshot_read_encrypted(struct snapshot_data *data,
				char __user *buf, size_t count, loff_t *offp)
{
	ssize_t total = 0;

	/* Loop getting buffers of varying sizes and copying to userspace. */
	while (count) {
		size_t copy_size;
		size_t not_done;
		void *src;
		ssize_t src_size = snapshot_read_next_encrypted(data, &src);

		if (src_size <= 0) {
			if (total == 0)
				return src_size;

			break;
		}

		copy_size = min(count, (size_t)src_size);
		not_done = copy_to_user(buf + total, src, copy_size);
		copy_size -= not_done;
		total += copy_size;
		count -= copy_size;
		data->crypt_offset += copy_size;
		if (copy_size == 0) {
			if (total == 0)
				return -EFAULT;

			break;
		}
	}

	*offp += total;
	return total;
}

ssize_t snapshot_write_encrypted(struct snapshot_data *data,
				 const char __user *buf, size_t count,
				 loff_t *offp)
{
	ssize_t total = 0;

	/* Loop getting buffers of varying sizes and copying from. */
	while (count) {
		size_t copy_size;
		size_t not_done;
		void *dst;
		ssize_t dst_size = snapshot_write_next_encrypted(data, &dst);

		if (dst_size <= 0) {
			if (total == 0)
				return dst_size;

			break;
		}

		copy_size = min(count, (size_t)dst_size);
		not_done = copy_from_user(dst, buf + total, copy_size);
		copy_size -= not_done;
		total += copy_size;
		count -= copy_size;
		data->crypt_offset += copy_size;
		if (copy_size == 0) {
			if (total == 0)
				return -EFAULT;

			break;
		}

		/*
		 * Drain the encrypted buffer if it's full, or if we hit the end
		 * of the meta pages and need a key change.
		 */
		if (data->crypt_offset >=
		    (PAGE_SIZE * CHUNK_SIZE) + SNAPSHOT_AUTH_TAG_SIZE ||
		    (data->meta_size &&
		     data->crypt_total < data->meta_size &&
		     data->crypt_total + data->crypt_offset ==
		     data->meta_size + SNAPSHOT_AUTH_TAG_SIZE)) {
			int rc;

			rc = snapshot_decrypt_drain(data);
			if (rc < 0)
				return rc;
		}
	}

	*offp += total;
	return total;
}

void snapshot_teardown_encryption(struct snapshot_data *data)
{
	int i;

	if (data->aead_req) {
		aead_request_free(data->aead_req);
		data->aead_req = NULL;
	}

	if (data->aead_tfm) {
		crypto_free_aead(data->aead_tfm);
		data->aead_tfm = NULL;
	}

	for (i = 0; i < CHUNK_SIZE; i++) {
		if (data->crypt_pages[i]) {
			free_page((unsigned long)data->crypt_pages[i]);
			data->crypt_pages[i] = NULL;
		}
	}

	memzero_explicit(data->encryption_key, sizeof(data->encryption_key));
	memzero_explicit(data->user_key, sizeof(data->user_key));
}

static int snapshot_setup_encryption_common(struct snapshot_data *data)
{
	int i, rc;

	data->crypt_total = 0;
	data->crypt_offset = 0;
	data->crypt_size = 0;
	data->user_key_valid = false;
	memset(data->crypt_pages, 0, sizeof(data->crypt_pages));
	/* This only works once per hibernate. */
	if (data->aead_tfm)
		return -EINVAL;

	/* Set up the encryption transform */
	data->aead_tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
	if (IS_ERR(data->aead_tfm)) {
		rc = PTR_ERR(data->aead_tfm);
		data->aead_tfm = NULL;
		return rc;
	}

	rc = -ENOMEM;
	data->aead_req = aead_request_alloc(data->aead_tfm, GFP_KERNEL);
	if (!data->aead_req)
		goto setup_fail;

	/* Allocate the staging area */
	for (i = 0; i < CHUNK_SIZE; i++) {
		data->crypt_pages[i] = (void *)__get_free_page(GFP_ATOMIC);
		if (!data->crypt_pages[i])
			goto setup_fail;
	}

	sg_init_table(data->sg, CHUNK_SIZE + 2);

	/*
	 * The associated data will be the offset so that blocks can't be
	 * rearranged.
	 */
	aead_request_set_ad(data->aead_req, sizeof(data->crypt_total));
	rc = crypto_aead_setauthsize(data->aead_tfm, SNAPSHOT_AUTH_TAG_SIZE);
	if (rc)
		goto setup_fail;

	return 0;

setup_fail:
	snapshot_teardown_encryption(data);
	return rc;
}

int snapshot_get_encryption_key(struct snapshot_data *data,
				struct uswsusp_key_blob __user *key)
{
	struct snapshot_wrapped_key wrapped = {};
	u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring = {};
	u8 nonce[USWSUSP_KEY_NONCE_SIZE];
	int rc;

	/* Don't pull a random key from a world that can be reset. */
	if (data->ready)
		return -EPIPE;

	rc = snapshot_setup_encryption_common(data);
	if (rc)
		return rc;

	/* Build a random starting nonce. */
	get_random_bytes(nonce, sizeof(nonce));
	memcpy(&data->nonce_low, &nonce[0], sizeof(data->nonce_low));
	memcpy(&data->nonce_high, &nonce[8], sizeof(data->nonce_high));

	/* Build and install a random image encryption key. */
	get_random_bytes(image_key, sizeof(image_key));
	rc = snapshot_install_image_key(data, image_key);
	if (rc)
		goto fail;

	rc = snapshot_wrap_image_key(image_key, &wrapped);
	if (rc)
		goto fail;

	/* Hand the wrapped key and clear nonce back to user mode. */
	rc = put_user(sizeof(wrapped), &key->blob_len);
	if (rc)
		goto fail;

	BUILD_BUG_ON(sizeof(wrapped) >
		     sizeof(((struct uswsusp_key_blob *)0)->blob));
	rc = copy_to_user(&key->blob, &wrapped, sizeof(wrapped));
	if (rc)
		goto fail;

	rc = copy_to_user(&key->nonce, &nonce, sizeof(nonce));
	if (rc)
		goto fail;

	memzero_explicit(image_key, sizeof(image_key));
	memzero_explicit(&wrapped, sizeof(wrapped));
	return 0;

fail:
	memzero_explicit(image_key, sizeof(image_key));
	memzero_explicit(&wrapped, sizeof(wrapped));
	snapshot_teardown_encryption(data);
	return rc;
}

int snapshot_set_encryption_key(struct snapshot_data *data,
				struct uswsusp_key_blob __user *key)
{
	struct uswsusp_key_blob *blob;
	struct snapshot_wrapped_key wrapped = {};
	u8 image_key[SNAPSHOT_ENCRYPTION_KEY_SIZE] __nonstring = {};
	int rc;

	/* It's too late if data's been pushed in. */
	if (data->handle.cur)
		return -EPIPE;

	rc = snapshot_setup_encryption_common(data);
	if (rc)
		return rc;

	/* Load the key from user mode. */
	blob = memdup_user(key, sizeof(*blob));
	if (IS_ERR(blob)) {
		rc = PTR_ERR(blob);
		goto crypto_setup_fail;
	}

	if (blob->blob_len != sizeof(wrapped)) {
		rc = -EINVAL;
		goto out_blob;
	}

	memcpy(&wrapped, blob->blob, sizeof(wrapped));
	rc = snapshot_unwrap_image_key(&wrapped, image_key);
	if (rc)
		goto out_blob;

	rc = snapshot_install_image_key(data, image_key);
	if (rc)
		goto out_blob;

	/* Load the starting nonce. */
	memcpy(&data->nonce_low, &blob->nonce[0], sizeof(data->nonce_low));
	memcpy(&data->nonce_high, &blob->nonce[8], sizeof(data->nonce_high));

out_blob:
	kfree_sensitive(blob);
	memzero_explicit(&wrapped, sizeof(wrapped));
	memzero_explicit(image_key, sizeof(image_key));
	if (rc)
		goto crypto_setup_fail;

	return 0;

crypto_setup_fail:
	memzero_explicit(&wrapped, sizeof(wrapped));
	memzero_explicit(image_key, sizeof(image_key));
	snapshot_teardown_encryption(data);
	return rc;
}

static loff_t snapshot_encrypted_byte_count(loff_t plain_size)
{
	loff_t pages = plain_size >> PAGE_SHIFT;
	loff_t chunks = (pages + (CHUNK_SIZE - 1)) / CHUNK_SIZE;
	/*
	 * The encrypted size is the normal size, plus a stitched in
	 * authentication tag for every chunk of pages.
	 */
	return plain_size + (chunks * SNAPSHOT_AUTH_TAG_SIZE);
}

static loff_t snapshot_get_meta_data_size(void)
{
	loff_t pages = snapshot_get_meta_page_count();

	return snapshot_encrypted_byte_count(pages << PAGE_SHIFT);
}

int snapshot_set_user_key(struct snapshot_data *data,
			  struct uswsusp_user_key __user *key)
{
	struct uswsusp_user_key user_key;
	unsigned int key_len;
	u64 size;
	int rc;

	/*
	 * Return the metadata size, the number of bytes that can be fed in before
	 * the user data key is needed at resume time.
	 */
	size = snapshot_get_meta_data_size();
	rc = put_user(size, &key->meta_size);
	if (rc)
		return rc;

	rc = copy_from_user(&user_key, key, sizeof(struct uswsusp_user_key));
	if (rc)
		return rc;

	BUILD_BUG_ON(sizeof(data->user_key) < sizeof(user_key.key));
	if (user_key.reserved)
		return -EINVAL;
	if (user_key.key_len > sizeof(data->user_key))
		return -EINVAL;
	if (user_key.key_len < 8)
		return -EINVAL;

	key_len = user_key.key_len;

	/* Don't allow it if it's too late. */
	if (data->crypt_total > data->meta_size)
		return -EBUSY;

	memset(data->user_key, 0, sizeof(data->user_key));
	memcpy(data->user_key, user_key.key, key_len);
	data->user_key_valid = true;
	/* Install the key if the user is just under the wire. */
	rc = snapshot_check_user_key_switch(data);
	if (rc)
		return rc;

	return 0;
}

loff_t snapshot_get_encrypted_image_size(loff_t raw_size)
{
	loff_t pages = raw_size >> PAGE_SHIFT;
	loff_t meta_size;

	pages -= snapshot_get_meta_page_count();
	meta_size = snapshot_get_meta_data_size();
	return snapshot_encrypted_byte_count(pages << PAGE_SHIFT) + meta_size;
}

int snapshot_finalize_decrypted_image(struct snapshot_data *data)
{
	int rc;

	if (data->crypt_offset != 0) {
		rc = snapshot_decrypt_drain(data);
		if (rc)
			return rc;
	}

	return 0;
}
