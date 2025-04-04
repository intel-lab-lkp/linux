#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <linux/err.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "task_local_storage_helpers.h"
#include "uptr_kv_store.h"

struct kv_store {
	int map_fd;
	int task_fd;
	char *map_pin_path;
	struct kv_store_data_map_value map_val;
};

static struct kv_store_meta *kvs_store_get_meta(struct kv_store *kvs, int key)
{
	return key < kvs->map_val.metas_cnt ? &kvs->map_val.metas->meta[key] : NULL;
}

static int kv_store_grow_metas(struct kv_store *kvs, int new_metas_cnt)
{
	int err, metas_cnt;
	void *new_metas;

	new_metas = mremap(kvs->map_val.metas,
			   sizeof(struct kv_store_meta) * kvs->map_val.metas_cnt,
			   sizeof(struct kv_store_meta) * new_metas_cnt, 0);

	if (new_metas == MAP_FAILED || new_metas != kvs->map_val.metas)
		return -EFAULT;

	metas_cnt = kvs->map_val.metas_cnt;
	kvs->map_val.metas_cnt = new_metas_cnt;
	err = bpf_map_update_elem(kvs->map_fd, &kvs->task_fd, &kvs->map_val, 0);
	if (err) {
		kvs->map_val.metas_cnt = metas_cnt;
		return err;
	}
	return 0;
}

static int kv_store_grow_page(struct kv_store *kvs, int new_page_size)
{
	int err, page_size;
	void *new_page;

	new_page = mremap(kvs->map_val.page, kvs->map_val.page_size,
			  new_page_size, 0);

	if (new_page == MAP_FAILED || new_page != kvs->map_val.page)
		return -EFAULT;

	page_size = kvs->map_val.page_size;
	kvs->map_val.page_size = new_page_size;
	err = bpf_map_update_elem(kvs->map_fd, &kvs->task_fd, &kvs->map_val, 0);
	if (err) {
		kvs->map_val.page_size = page_size;
		return err;
	}
	return 0;
}

void kv_store_close(struct kv_store *kvs)
{
	munmap(kvs->map_val.metas, sizeof(struct kv_store_meta) * kvs->map_val.metas_cnt);
	munmap(kvs->map_val.page, kvs->map_val.page_size);

	if (kvs->map_pin_path)
		unlink(kvs->map_pin_path);

	free(kvs);
}

struct kv_store *kv_store_init(int pid, struct bpf_map *data_map, const char *pin_path,
			       struct kv_pairs *kvp)
{
	struct kv_store *kvs;
	int i, err;
	size_t metas_cnt = 0, data_size = 0;

	kvs = calloc(1, sizeof(*kvs));
	if (!kvs) {
		errno = -ENOMEM;
		return NULL;
	}

	if (kvp) {
		for (i = 0; i < kvp->array_cnt; i++) {
			if (kvp->array[i].size > KVS_MAX_VAL_SIZE ||
			    kvp->array[i].size + data_size > PAGE_SIZE)
				continue;
			metas_cnt = metas_cnt > kvp->array[i].key ? metas_cnt : kvp->array[i].key + 1;
			data_size += kvp->array[i].size;
		}
	} else {
		metas_cnt = 16;
		data_size = 256;
	}

	kvs->map_val.metas_cnt = metas_cnt;
	kvs->map_val.metas = mmap(NULL, metas_cnt * sizeof(struct kv_store_meta),
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (kvs->map_val.metas == MAP_FAILED) {
		errno = -ENOMEM;
		return NULL;
	}

	kvs->map_val.page_size = data_size;
	kvs->map_val.page = mmap(NULL, data_size,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (kvs->map_val.page == MAP_FAILED) {
		munmap(kvs->map_val.metas, sizeof(struct kv_store_metas));
		errno = -ENOMEM;
		goto err;
	}

	kvs->map_fd = bpf_map__fd(data_map);
	if (!kvs->map_fd) {
		errno = -ENOENT;
		goto err;
	}

	kvs->task_fd = sys_pidfd_open(pid, 0);
	if (!kvs->task_fd) {
		errno = -ESRCH;
		goto err;
	}

	err = bpf_map_update_elem(kvs->map_fd, &kvs->task_fd, &kvs->map_val, 0);
	if (err) {
		errno = err;
		goto err;
	}

	kvs->map_pin_path = strdup(pin_path);
	if (!kvs->map_pin_path)
		goto err;

	err = bpf_map__pin(data_map, kvs->map_pin_path);
	if (err) {
		errno = err;
		goto err;
	}

	if (kvp) {
		data_size = 0;
		for (i = 0; i < kvp->array_cnt; i++) {
			struct kv_store_meta *meta;

			meta = &kvs->map_val.metas->meta[kvp->array[i].key];

			if (kvp->array[i].size > KVS_MAX_VAL_SIZE ||
			    kvp->array[i].size + data_size > PAGE_SIZE)
				continue;

			if (kvp->array[i].val) {
				memcpy(kvs->map_val.page->data + data_size,
				       kvp->array[i].val,
				       kvp->array[i].size);
			}

			meta->page_off = data_size;
			meta->size = kvp->array[i].size;
			meta->init = 1;

			data_size += kvp->array[i].size;
		}
	}

	return kvs;
err:
	kv_store_close(kvs);
	return NULL;
}

int kv_store_reuse(struct kv_store *kvs, struct bpf_map *data_map)
{
	return bpf_map__reuse_fd(data_map, kvs->map_fd);
}

static int comp_kv_pair(const void *kv1, const void *kv2)
{
	return ((const struct kv_pair *)kv1)->key -
	       ((const struct kv_pair *)kv2)->key;
}

int __kv_store_set(struct kv_store *kvs, int key, void *val, unsigned int val_size);

void kv_store_update(struct kv_store *kvs, struct kv_pairs *kvp)
{
	int i = 0, j = 0;

	qsort(kvp->array, kvp->array_cnt, sizeof(struct kv_pair), comp_kv_pair);

	/* for key = [0, max kvp key], delete unused key-value pairs and add new ones */
	while (j < kvp->array_cnt) {
		if (i < kvp->array[j].key) {
			if (kvs->map_val.metas->meta[i].init)
				kv_store_delete(kvs, i);
			i++;
			continue;
		}

		if (kvs->map_val.metas->meta[i].init &&
		    kvs->map_val.metas->meta[i].size != kvp->array[j].size)
			kv_store_delete(kvs, i);

		if (kvs->map_val.metas->meta[i].size != kvp->array[j].size ||
		    kvp->array[j].val)
			__kv_store_set(kvs, kvp->array[j].key, kvp->array[j].val,
				       kvp->array[j].size);
		i++;
		j++;
	}
	/* for key = [max kvp key + 1, max kvs key], delete unused key-value pairs */
	for (; i < kvs->map_val.metas_cnt; i++)
		if (kvs->map_val.metas->meta[i].init)
			kv_store_delete(kvs, i);
}

void *kv_store_get(struct kv_store *kvs, int key)
{
	struct kv_store_meta *meta;

	meta = kvs_store_get_meta(kvs, key);
	if (!meta || !meta->init)
		return NULL;

	return kvs->map_val.page->data + meta->page_off;
}

static int comp_meta(const void *m1, const void *m2)
{
	const struct kv_store_meta *meta1 = (const struct kv_store_meta *)m1;
	const struct kv_store_meta *meta2 = (const struct kv_store_meta *)m2;

	return (meta1->page_off + meta1->init ? 0 : PAGE_SIZE) -
	       (meta2->page_off + meta2->init ? 0 : PAGE_SIZE);
}

static int kv_store_find_next_slot(struct kv_store *kvs, int size, struct kv_store_meta *meta)
{
	struct kv_store_meta metas[KVS_MAX_VAL_ENTRIES];
	int i, err, off = 0;

	if (size > KVS_MAX_VAL_SIZE)
		return -E2BIG;

	memcpy(metas, kvs->map_val.metas, sizeof(struct kv_store_meta) * kvs->map_val.metas_cnt);

	qsort(metas, kvs->map_val.metas_cnt, sizeof(struct kv_store_meta), comp_meta);

	for (i = 0; i < kvs->map_val.metas_cnt; i++) {
		if (metas[i].page_off - off >= size || !metas[i].init)
			break;
		off = metas[i].page_off + metas[i].size;
	}

	if (size + off > PAGE_SIZE)
		return -ENOSPC;

	if (size + off > kvs->map_val.page_size) {
		err = kv_store_grow_page(kvs, size + off);
		if (err)
			return err;
	}

	meta->page_off = off;
	meta->size = size;
	return 0;
}

int __kv_store_set(struct kv_store *kvs, int key, void *val, unsigned int val_size)
{
	struct kv_store_meta *meta;
	int err;

	if (key >= kvs->map_val.metas_cnt && key < KVS_MAX_VAL_ENTRIES) {
		err = kv_store_grow_metas(kvs, key + 1);
		if (err)
			return err;
	}

	meta = kvs_store_get_meta(kvs, key);
	if (!meta)
		return -ENOENT;

	if (!meta->init) {
		err = kv_store_find_next_slot(kvs, val_size, meta);
		if (err)
			return err;
	}

	val_size = val_size < meta->size ? val_size : meta->size;
	if (val)
		memcpy(kvs->map_val.page->data + meta->page_off, val, val_size);
	else
		memset(kvs->map_val.page->data + meta->page_off, 0, val_size);

	meta->init = 1;
	return 0;
}

int kv_store_set(struct kv_store *kvs, int key, void *val, unsigned int val_size)
{
	if (!val)
		return -EINVAL;

	return __kv_store_set(kvs, key, val, val_size);
}

void kv_store_delete(struct kv_store *kvs, int key)
{
	struct kv_store_meta *meta;

	meta = kvs_store_get_meta(kvs, key);
	if (!meta)
		return;

	memset(kvs->map_val.page->data + meta->page_off, 0, meta->size);
	memset(meta, 0, sizeof(*meta));
}
