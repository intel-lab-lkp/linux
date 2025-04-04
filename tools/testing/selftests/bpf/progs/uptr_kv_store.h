#ifndef _UPTR_KV_STORE_H
#define _UPTR_KV_STORE_H

#include <errno.h>
#include <string.h>
#include <bpf/bpf_helpers.h>

#include "uptr_kv_store_common.h"

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct kv_store_data_map_value);
} data_map SEC(".maps");

__attribute__((unused))
static int kv_store_set(struct kv_store_data_map_value *data, u16 key, void *val, int val_size)
{
	struct kv_store_meta *meta;
	struct bpf_dynptr ptr;
	int err = 0;

	if (!data || !data->metas || !data->page)
		return -ENOENT;

	if (key >= KVS_MAX_VAL_ENTRIES || key >= data->metas_cnt)
		return -ENOENT;

	meta = &data->metas->meta[key];
	if (!meta->init)
		return -ENOENT;

	switch (meta->size) {
	case 1:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 1)
			return -EFAULT;
		*(u8 *)(data->page->data + meta->page_off) = *(u8 *)val;
		break;
	case 4:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 4)
			return -EFAULT;
		*(u32 *)(data->page->data + meta->page_off) = *(u32 *)val;
		break;
	case 8:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 8)
			return -EFAULT;
		*(u64 *)(data->page->data + meta->page_off) = *(u64 *)val;
		break;
	default:
		if (meta->page_off >= KVS_MAX_VAL_SIZE)
			return -EFAULT;

		err = bpf_dynptr_from_mem(data->page->data, KVS_MAX_VAL_SIZE, 0, &ptr);
		if (err)
			return err;

		val_size = val_size > meta->size ? meta->size : val_size;
		err = bpf_dynptr_write(&ptr, meta->page_off, val, val_size, 0);
	}
	return err;
}

__attribute__((unused))
static int kv_store_get(struct kv_store_data_map_value *data, u16 key, void *val, int val_size)
{
	struct kv_store_meta *meta;
	struct bpf_dynptr ptr;
	int err = 0;

	if (!data || !data->metas || !data->page)
		return -ENOENT;

	if (key >= KVS_MAX_VAL_ENTRIES || key >= data->metas_cnt)
		return -ENOENT;

	meta = &data->metas->meta[key];
	if (!meta->init)
		return -ENOENT;

	switch (meta->size) {
	case 1:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 1)
			return -EFAULT;
		*(u8 *)val = *(u8 *)(data->page->data + meta->page_off);
		break;
	case 4:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 4)
			return -EFAULT;
		*(u32 *)val = *(u32 *)(data->page->data + meta->page_off);
		break;
	case 8:
		if (meta->page_off > KVS_MAX_VAL_SIZE - 8)
			return -EFAULT;
		*(u64 *)val = *(u64 *)(data->page->data + meta->page_off);
		break;
	default:
		if (meta->page_off >= KVS_MAX_VAL_SIZE)
			return -EFAULT;

		err = bpf_dynptr_from_mem(data->page->data, KVS_MAX_VAL_SIZE, 0, &ptr);
		if (err)
			return err;

		val_size = val_size > meta->size ? meta->size : val_size;
		err = bpf_dynptr_read(val, val_size, &ptr, meta->page_off, 0);
	}
	return err;
}

__attribute__((unused))
static int kv_store_delete(struct kv_store_data_map_value *data, u16 key)
{
	struct kv_store_meta *meta;

	if (!data || !data->metas)
		return -ENOENT;

	if (key >= KVS_MAX_VAL_ENTRIES)
		return -ENOENT;

	meta = &data->metas->meta[key];
	meta->init = 0;
	return 0;
}

#endif
