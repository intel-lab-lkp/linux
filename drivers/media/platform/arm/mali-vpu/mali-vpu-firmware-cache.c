// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/delay.h>
#include <linux/kthread.h>

#include "mali-vpu-device.h"
#include "mali-vpu-firmware-cache.h"
#include "mali-vpu-mmu.h"
#include "mali-vpu-protocol-def-v3.h"

#define MALI_VPU_FW_CACHE_CLEANUP_INTERVAL_MS 30000

#define MALI_VPU_GET_MAJOR_VERSION(ver) (((ver) >> 16) & 0xFF)

static inline bool mali_vpu_test_bit_le32(u32 bit, const __le32 *addr)
{
	return !!(le32_to_cpu(addr[bit >> 5]) & BIT(bit & 0x1F));
}

static void mali_vpu_fw_bin_release(struct kref *ref)
{
	struct mali_vpu_fw_bin *bin = container_of(ref, struct mali_vpu_fw_bin, refcount);

	/* cache_link must already be detached before final put */
	WARN_ON_ONCE(!list_empty(&bin->cache_link));

	if (bin->fw)
		release_firmware(bin->fw);

	kfree(bin);
}

static int mali_vpu_fw_build_name(char *name, size_t name_sz,
				  const struct mali_vpu_fw_key *key)
{
	const char *role_str = NULL;
	const char *codec = NULL;
	int n;

	if (!name || !name_sz || !key)
		return -EINVAL;

	if (key->session_role != MALI_VPU_DECODER)
		return -EINVAL;

	role_str = "dec";

	if (key->codec_fourcc == V4L2_PIX_FMT_H264)
		codec = "h264";

	if (!codec)
		return -ENOENT;

	n = snprintf(name, name_sz, "arm/mali-vpu/v%u/%s%s.fwb",
		     key->fw_ver_major, codec, role_str);
	if (n < 0)
		return n;
	if ((size_t)n >= name_sz)
		return -ENAMETOOLONG;

	return 0;
}

static int mali_vpu_fw_parse_header(const struct firmware *fw,
				    struct mali_vpu_fw_bin *vpu_fw_bin)
{
	const struct mali_vpu_fw_header *header;
	u32 header_fw_ver_major;
	u32 fw_version, text_length, bss_start, bss_bitmap_size;
	u32 master_rw_start, master_rw_size;
	mali_vpu_mmu_va va;
	u32 i;

	/* Validate the fw binary */
	if (fw->size < sizeof(*header)) {
		dev_err(vpu_fw_bin->cache->dev,
			"firmware bin too small: %zu bytes, need at least %zu\n",
			fw->size, sizeof(*header));
		return -EINVAL;
	}

	header = (const struct mali_vpu_fw_header *)fw->data;

	fw_version	= le32_to_cpu(header->fw_version);
	text_length	= le32_to_cpu(header->text_length);
	bss_start	= le32_to_cpu(header->bss_start_address);
	bss_bitmap_size	= le32_to_cpu(header->bss_bitmap_size);
	master_rw_start	= le32_to_cpu(header->master_rw_start_address);
	master_rw_size	= le32_to_cpu(header->master_rw_size);

	if (text_length < sizeof(*header) ||
	    text_length > fw->size) {
		dev_err(vpu_fw_bin->cache->dev,
			"invalid firmware text length: %u\n",
			text_length);
		return -EBADMSG;
	}

	if (bss_bitmap_size > ARRAY_SIZE(header->bss_bitmap) * 32) {
		dev_err(vpu_fw_bin->cache->dev,
			"invalid firmware BSS bitmap size: %u (max %zu)\n",
			bss_bitmap_size, ARRAY_SIZE(header->bss_bitmap) * 32UL);
		return -EBADMSG;
	}

	/* Firmware version mismatch */
	header_fw_ver_major = MALI_VPU_GET_MAJOR_VERSION(fw_version);
	if (header_fw_ver_major != vpu_fw_bin->key.fw_ver_major) {
		dev_err(vpu_fw_bin->cache->dev,
			"firmware major version mismatch: expected %u, got %u. Binary ver: %#x\n",
			vpu_fw_bin->key.fw_ver_major, header_fw_ver_major, fw_version);
		return -EINVAL;
	}

	/* Match host<->firmware communication protocol version */
	if (header->protocol_major != MALI_VPU_HOST_PROTOCOL_MAJOR ||
	    header->protocol_minor > MALI_VPU_HOST_PROTOCOL_MINOR) {
		dev_err(vpu_fw_bin->cache->dev,
			"firmware protocol version mismatch: expected %u.%u, got %u.%u\n",
			MALI_VPU_HOST_PROTOCOL_MAJOR, MALI_VPU_HOST_PROTOCOL_MINOR,
			header->protocol_major, header->protocol_minor);
		return -EINVAL;
	}

	vpu_fw_bin->header = header;

	/* Calculate number of pages needed for the text segment. */
	vpu_fw_bin->text_page_count = DIV_ROUND_UP(text_length, MALI_VPU_PAGE_SIZE);

	/* Calculate number of pages needed for the BSS segments. */
	vpu_fw_bin->bss_page_count = 0;
	vpu_fw_bin->shared_bss_page_count = 0;

	va = bss_start;
	for (i = 0; i < bss_bitmap_size; i++) {
		if (va >= master_rw_start &&
		    va < (master_rw_start + master_rw_size))
			vpu_fw_bin->shared_bss_page_count++;
		else if (mali_vpu_test_bit_le32(i, header->bss_bitmap))
			vpu_fw_bin->bss_page_count++;

		va += MALI_VPU_PAGE_SIZE;
	}

	return 0;
}

static void mali_vpu_fw_notify_event_list(struct list_head *event_list,
					  struct mali_vpu_fw_bin *bin,
					  bool same_thread)
{
	struct mali_vpu_fw_event *event, *tmp;

	list_for_each_entry_safe(event, tmp, event_list, link) {
		list_del_init(&event->link);
		event->fw_bin_ready(bin, event->arg, same_thread);
	}
}

static void mali_vpu_fw_request_done(const struct firmware *fw, void *context)
{
	struct mali_vpu_fw_bin *vpu_fw_bin = context;
	const struct mali_vpu_fw_header *hdr;
	LIST_HEAD(local_event_list);
	int ret = 0;

	mutex_lock(&vpu_fw_bin->cache->lock);
	mutex_lock(&vpu_fw_bin->lock);

	if (!fw) {
		ret = -ENOENT;
		vpu_fw_bin->fw = NULL;
	} else {
		ret = mali_vpu_fw_parse_header(fw, vpu_fw_bin);
		if (ret) {
			release_firmware(fw);
			vpu_fw_bin->fw = NULL;
		} else {
			vpu_fw_bin->fw = fw;
		}
	}

	/* We have to notify waiters in failure or success case */
	vpu_fw_bin->load_status = ret;
	vpu_fw_bin->state = ret ? MALI_VPU_FW_BIN_FAILED : MALI_VPU_FW_BIN_READY;

	/*
	 * Take a local copy of events/waiters
	 * so that notify callback can be called outside vpu_fw_bin->lock
	 */
	list_splice_init(&vpu_fw_bin->event_list, &local_event_list);

	if (ret) {
		/* Firmware load failed, remove cached entry */
		list_del_init(&vpu_fw_bin->cache_link);
		kref_put(&vpu_fw_bin->refcount, mali_vpu_fw_bin_release); /* Drop cache-ref */
	}

	mutex_unlock(&vpu_fw_bin->lock);
	mutex_unlock(&vpu_fw_bin->cache->lock);

	/*
	 * Keep the loader-owned reference until all waiters have been notified
	 * and completion logging has finished. Callbacks may release their
	 * caller-owned reference to vpu_fw_bin; on a load failure, the cache
	 * reference has also already been dropped.
	 */
	mali_vpu_fw_notify_event_list(&local_event_list, vpu_fw_bin, false);

	if (ret) {
		dev_warn(vpu_fw_bin->cache->dev, "firmware load failed: %s (status=%d)\n",
			 vpu_fw_bin->filename, ret);
	} else {
		hdr = vpu_fw_bin->header;
		dev_info(vpu_fw_bin->cache->dev, "firmware loaded: %s\n", vpu_fw_bin->filename);

		dev_dbg(vpu_fw_bin->cache->dev,
			"firmware info: Version %.*s, MVE Protocol Version %u.%u,\n",
			(int)sizeof(hdr->fw_version_str), (const char *)hdr->fw_version_str,
			hdr->protocol_major, hdr->protocol_minor);
		dev_dbg(vpu_fw_bin->cache->dev,
			"firmware info: \"%.*s\", Rasc jmp = %#x, text_length=%u, bss=%#x\n",
			(int)sizeof(hdr->info_string), (const char *)hdr->info_string,
			le32_to_cpu(hdr->rasc_jmp),
			le32_to_cpu(hdr->text_length),
			le32_to_cpu(hdr->bss_start_address));
		dev_dbg(vpu_fw_bin->cache->dev,
			"firmware info: text pages=%u, shared bss=%u, bss pages=%u\n",
			vpu_fw_bin->text_page_count, vpu_fw_bin->shared_bss_page_count,
			vpu_fw_bin->bss_page_count);
	}

	/* Notification and logging are complete; drop loader-owned reference */
	kref_put(&vpu_fw_bin->refcount, mali_vpu_fw_bin_release);
}

static struct mali_vpu_fw_bin *
mali_vpu_fw_bin_create(struct mali_vpu_fw_cache *cache,
		       const struct mali_vpu_fw_key *key,
		       struct mali_vpu_fw_event *event,
		       bool *call_now)
{
	struct mali_vpu_fw_bin *vpu_fw_bin;
	int ret;

	vpu_fw_bin = kzalloc_obj(*vpu_fw_bin);
	if (!vpu_fw_bin)
		return ERR_PTR(-ENOMEM);

	/* One cache-owned reference while binary is in cache->fw_bins */
	kref_init(&vpu_fw_bin->refcount);
	/* One caller-owned reference returned by this get(). */
	kref_get(&vpu_fw_bin->refcount);

	vpu_fw_bin->cache = cache;
	vpu_fw_bin->key = *key;
	vpu_fw_bin->state = MALI_VPU_FW_BIN_LOADING;
	vpu_fw_bin->load_status = -EINPROGRESS;
	mutex_init(&vpu_fw_bin->lock);
	INIT_LIST_HEAD(&vpu_fw_bin->cache_link);
	INIT_LIST_HEAD(&vpu_fw_bin->event_list);

	ret = mali_vpu_fw_build_name(vpu_fw_bin->filename,
				     sizeof(vpu_fw_bin->filename), key);
	if (ret) {
		dev_err(vpu_fw_bin->cache->dev,
			"could not build fw name: fourcc=%#x, role=%d, version=%#x\n",
			key->codec_fourcc, key->session_role, key->fw_ver_major);
		goto err_put_caller_cache;
	}

	list_add(&vpu_fw_bin->cache_link, &cache->fw_bins);

	/* One loader-owned reference while firmware is being loaded */
	kref_get(&vpu_fw_bin->refcount);

	ret = request_firmware_nowait(THIS_MODULE, true, vpu_fw_bin->filename,
				      cache->dev, GFP_KERNEL, vpu_fw_bin,
				      mali_vpu_fw_request_done);
	if (ret)
		goto err_put_loader_caller_cache;

	/*
	 * Serialise adding event to the event_list and firmware load completion.
	 * For fast callback where firmware load is completed before this thread can
	 * queue the event to the event_list for notification.
	 */
	mutex_lock(&vpu_fw_bin->lock);
	if (vpu_fw_bin->state == MALI_VPU_FW_BIN_LOADING)
		list_add(&event->link, &vpu_fw_bin->event_list);
	else
		*call_now = true;
	mutex_unlock(&vpu_fw_bin->lock);

	return vpu_fw_bin;

err_put_loader_caller_cache:
	kref_put(&vpu_fw_bin->refcount, mali_vpu_fw_bin_release); /* Loader ref */
	list_del_init(&vpu_fw_bin->cache_link);

err_put_caller_cache:
	kref_put(&vpu_fw_bin->refcount, mali_vpu_fw_bin_release); /* Caller ref */
	kref_put(&vpu_fw_bin->refcount, mali_vpu_fw_bin_release); /* Cache ref */
	/* Binary freed in release callback */
	return ERR_PTR(ret);
}

static bool mali_vpu_fw_key_match(const struct mali_vpu_fw_key *a,
				  const struct mali_vpu_fw_key *b)
{
	return a->codec_fourcc == b->codec_fourcc &&
	       a->session_role == b->session_role &&
	       a->fw_ver_major == b->fw_ver_major;
}

static struct mali_vpu_fw_bin *mali_vpu_fw_find_locked(struct mali_vpu_fw_cache *cache,
						       const struct mali_vpu_fw_key *key)
{
	struct mali_vpu_fw_bin *bin;

	list_for_each_entry(bin, &cache->fw_bins, cache_link) {
		if (!mali_vpu_fw_key_match(&bin->key, key))
			continue;
		if (!kref_get_unless_zero(&bin->refcount))
			continue;

		return bin;
	}

	return NULL;
}

int mali_vpu_fw_cache_get(struct mali_vpu_fw_cache *cache,
			  const struct mali_vpu_fw_key *key,
			  struct mali_vpu_fw_event *event)
{
	struct mali_vpu_fw_bin *bin;
	bool call_now = false;
	int ret = 0;

	if (!cache || !key || !event || !event->fw_bin_ready)
		return -EINVAL;

	/*
	 * The callback may run synchronously before this function returns if
	 * firmware loading has already completed. In that case, same_thread is
	 * true. Otherwise, the firmware loader invokes it asynchronously with
	 * same_thread set to false.
	 *
	 * The event must remain valid until the callback runs. Every call that
	 * returns 0 owns one reference to the binary supplied to the callback;
	 * the caller must release it with mali_vpu_fw_cache_put()
	 */
	INIT_LIST_HEAD(&event->link);

	mutex_lock(&cache->lock);
	bin = mali_vpu_fw_find_locked(cache, key);
	if (!bin) {
		bin = mali_vpu_fw_bin_create(cache, key, event, &call_now);
		if (IS_ERR(bin)) {
			ret = PTR_ERR(bin);
			goto unlock_cache;
		}
	} else {
		mutex_lock(&bin->lock);
		if (bin->state == MALI_VPU_FW_BIN_LOADING)
			list_add(&event->link, &bin->event_list);
		else
			call_now = true;
		mutex_unlock(&bin->lock);
	}

unlock_cache:
	mutex_unlock(&cache->lock);

	if (ret)
		return ret;

	if (call_now)
		event->fw_bin_ready(bin, event->arg, true);

	return 0;
}

void mali_vpu_fw_cache_put(struct mali_vpu_fw_bin *bin)
{
	kref_put(&bin->refcount, mali_vpu_fw_bin_release);
}

bool mali_vpu_fw_cache_cancel_waiter(struct mali_vpu_fw_cache *cache,
				     struct mali_vpu_fw_event *event)
{
	struct mali_vpu_fw_bin *cancelled_bin = NULL;
	struct mali_vpu_fw_event *waiter;
	struct mali_vpu_fw_bin *bin;

	if (!cache || !event)
		return false;

	mutex_lock(&cache->lock);

	list_for_each_entry(bin, &cache->fw_bins, cache_link) {
		mutex_lock(&bin->lock);

		list_for_each_entry(waiter, &bin->event_list, link) {
			if (waiter != event)
				continue;

			list_del_init(&event->link);
			cancelled_bin = bin;
			break;
		}

		mutex_unlock(&bin->lock);

		if (cancelled_bin)
			break;
	}

	mutex_unlock(&cache->lock);

	if (!cancelled_bin)
		return false;

	/*
	 * Every successful cache_get() owns one caller reference. Since the
	 * callback will no longer run for this waiter, release that reference
	 * here.
	 */
	mali_vpu_fw_cache_put(cancelled_bin);

	return true;
}

static void mali_vpu_fw_cache_prune(struct mali_vpu_fw_cache *cache)
{
	LIST_HEAD(prune_list);
	struct mali_vpu_fw_bin *bin, *temp;

	mutex_lock(&cache->lock);
	list_for_each_entry_safe(bin, temp, &cache->fw_bins, cache_link) {
		if (kref_read(&bin->refcount) != 1) /* If only cache holds ref */
			continue;

		list_del_init(&bin->cache_link);
		list_add(&bin->cache_link, &prune_list);
	}
	mutex_unlock(&cache->lock);

	list_for_each_entry_safe(bin, temp, &prune_list, cache_link) {
		list_del_init(&bin->cache_link);
		kref_put(&bin->refcount, mali_vpu_fw_bin_release);
	}
}

static int mali_vpu_fw_cache_thread_fn(void *data)
{
	struct mali_vpu_fw_cache *cache = data;

	while (!kthread_should_stop()) {
		mali_vpu_fw_cache_prune(cache);
		msleep_interruptible(MALI_VPU_FW_CACHE_CLEANUP_INTERVAL_MS);
	}

	return 0;
}

int mali_vpu_fw_cache_init(struct mali_vpu_fw_cache *cache, struct device *dev)
{
	int ret;

	cache->dev = dev;
	mutex_init(&cache->lock);
	INIT_LIST_HEAD(&cache->fw_bins);

	cache->cache_thread = kthread_run(mali_vpu_fw_cache_thread_fn, cache, "mali_vpu_fw_cache");
	if (IS_ERR(cache->cache_thread)) {
		ret = PTR_ERR(cache->cache_thread);
		cache->cache_thread = NULL;
		return ret;
	}

	return 0;
}

void mali_vpu_fw_cache_cleanup(struct mali_vpu_fw_cache *cache)
{
	struct mali_vpu_fw_bin *bin;
	struct mali_vpu_fw_bin *temp;
	LIST_HEAD(local);

	if (cache->cache_thread)
		kthread_stop(cache->cache_thread);

	mutex_lock(&cache->lock);
	list_splice_init(&cache->fw_bins, &local);
	mutex_unlock(&cache->lock);

	list_for_each_entry_safe(bin, temp, &local, cache_link) {
		list_del_init(&bin->cache_link);
		kref_put(&bin->refcount, mali_vpu_fw_bin_release);
	}
}
