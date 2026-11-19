/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_FIRMWARE_CACHE_H__
#define __MALI_VPU_FIRMWARE_CACHE_H__

#include <asm/byteorder.h>
#include <linux/firmware.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct device;
struct task_struct;

/**
 * struct mali_vpu_fw_cache - Per-device firmware cache context.
 * @dev: Device used for firmware requests and logging.
 * @lock: Serializes access to @fw_bins and cache-global state.
 * @fw_bins: List of cached firmware binaries (linked by struct mali_vpu_fw_bin::cache_link).
 * @cache_thread: Background thread that periodically prunes bins as per cache policy.
 *
 * One cache context exists per VPU device.
 */

struct mali_vpu_fw_cache {
	struct device *dev;
	struct mutex lock;		/* Protects the fw_bins list */
	struct list_head fw_bins;
	struct task_struct *cache_thread;
};

/**
 * struct mali_vpu_fw_header - Firmware binary header.
 * @rasc_jmp: Start address.
 * @protocol_minor: Host interface protocol minor version.
 * @protocol_major: Host interface protocol major version.
 * @reserved: Reserved for future use. Always 0.
 * @mve_version: MVE version string.
 * @info_string: Human readable codec information.
 * @part_number: Part number.
 * @git_hash: Short git hash of the firmware source.
 * @fw_version: Semantic firmware version encoded as 0x00MMmmpp where MM = major,
 *		mm = minor, pp = patch.
 * @fw_version_str: Semantic firmware version as a string.
 * @text_length: Length in bytes of the read-only part of the firmware.
 * @bss_start_address: Start address for BSS segment. This is always page-aligned.
 * @bss_bitmap_size: The number of bits used in 'bss_bitmap'.
 * @bss_bitmap: Bitmap which pages that shall be allocated and MMU mapped.
 *		If bit N is set, then a page shall be allocated and MMU mapped to VA address
 *		FW_BASE + bss_start_address + N * MVE_PAGE_SIZE.
 * @master_rw_start_address: Defines a region of shared pages.
 * @master_rw_size: Defines a region of shared pages.
 *
 * This structure is read directly from the firmware binary and describes the layout
 * of the firmware in memory. All multibyte integer fields are little-endian.
 */
struct mali_vpu_fw_header {
	__le32 rasc_jmp;
	u8 protocol_minor;
	u8 protocol_major;
	u8 reserved[2];

	u8 mve_version[8];
	u8 info_string[48];

	u8 part_number[8];
	u8 git_hash[8];

	__le32 fw_version;
	u8 fw_version_str[16];

	__le32 text_length;
	__le32 bss_start_address;
	__le32 bss_bitmap_size;
	__le32 bss_bitmap[16];

	__le32 master_rw_start_address;
	__le32 master_rw_size;
};

/**
 * struct mali_vpu_fw_key - Structure describing the key fields to identify a firmware binary.
 * @codec_fourcc: Fourcc code of the codec.
 * @session_role: Role of the session (encoder/decoder).
 * @fw_ver_major: Expected major firmware version.
 *
 * The key uniquely identifies the firmware variant expected by a session.
 */
struct mali_vpu_fw_key {
	u32 codec_fourcc;
	u32 session_role;
	u32 fw_ver_major;
};

enum mali_vpu_fw_bin_state {
	MALI_VPU_FW_BIN_LOADING = 0,
	MALI_VPU_FW_BIN_READY,
	MALI_VPU_FW_BIN_FAILED,
};

/**
 * struct mali_vpu_fw_bin - Structure describing a loaded firmware binary.
 * @refcount: Lifetime reference count (cache/session/loader references).
 * @cache: Owning firmware cache context.
 * @cache_link: Node in mali_vpu_fw_cache::fw_bins list.
 * @event_list: List of waiters (mali_vpu_fw_event) waiting for this firmware to load.
 * @lock: Protects load, status, header pointers and @event_list.
 * @key: Identity key for cache match and filename generation.
 * @filename: Firmware filename derived from @key.
 * @state: Current load state of the firmware binary (loading/ready/failed).
 * @load_status: Final load result (0 on success, negative errno on failure).
 * @fw: Firmware object returned by firmware loader.
 * @header: Parsed firmware header within @fw->data.
 * @text_page_count: Derived page count for firmware text segment.
 * @bss_page_count: Derived page count for firmware BSS segment.
 * @shared_bss_page_count: Derived page count for shared BSS area.
 *
 * A bin remains in the cache while referenced and may be shared by many sessions
 * requesting the same @key.
 */
struct mali_vpu_fw_bin {
	struct kref refcount;
	struct mali_vpu_fw_cache *cache;

	struct list_head cache_link;
	struct list_head event_list;
	struct mutex lock;      /* protects state/load_status/header/waiters */

	struct mali_vpu_fw_key key;
	char filename[128];

	enum mali_vpu_fw_bin_state state;
	int load_status;

	const struct firmware *fw;
	const struct mali_vpu_fw_header *header;

	/* Derived fields from mali_vpu_fw_header */
	u32 text_page_count;
	u32 bss_page_count;
	u32 shared_bss_page_count;
};

/**
 * struct mali_vpu_fw_event - Completion callback registration for firmware loads.
 * @link: Internal list node used while in queued in a bin's waiter list.
 * @fw_bin_ready: Callback routine invoked when firmware load finishes. Its same_thread
 *		argument is true when called synchronously from mali_vpu_fw_cache_get(),
 *		and false when called from the asynchronous firmware-loader callback.
 * @arg: Opaque client context passed to @fw_bin_ready.
 *
 * Callback is invoked for both success and failure. The client checks fw_bin->load_status
 * to determine result.
 */
struct mali_vpu_fw_event {
	struct list_head link;
	void (*fw_bin_ready)(struct mali_vpu_fw_bin *fw_bin, void *arg, bool same_thread);
	void *arg;
};

int mali_vpu_fw_cache_init(struct mali_vpu_fw_cache *cache, struct device *dev);
void mali_vpu_fw_cache_cleanup(struct mali_vpu_fw_cache *cache);
int mali_vpu_fw_cache_get(struct mali_vpu_fw_cache *cache,
			  const struct mali_vpu_fw_key *key,
			  struct mali_vpu_fw_event *event);
void mali_vpu_fw_cache_put(struct mali_vpu_fw_bin *bin);
bool mali_vpu_fw_cache_cancel_waiter(struct mali_vpu_fw_cache *cache,
				     struct mali_vpu_fw_event *event);

#endif /* __MALI_VPU_FIRMWARE_CACHE_H__ */
