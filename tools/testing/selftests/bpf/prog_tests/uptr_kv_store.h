#ifndef _UPTR_KV_STORE_H
#define _UPTR_KV_STORE_H

#include "uptr_kv_store_common.h"

struct kv_store;

struct kv_pair {
	int key;
	int size;
	void *val;
};

struct kv_pairs {
	int array_cnt;
	struct kv_pair array[];
};

/**
 * @brief kv_store_close() closes a KV store object and release all resources.
 *
 * @param kvs A pointer to a KV store object
 */
void kv_store_close(struct kv_store *kvs);

/**
 * @brief kv_store_init() creates a KV store object backed by uptr in a task
 * local storage map shared between user space and bpf programs. It allocates
 * memory to be assigned to uptr, initializes key-value pairs, updates the
 * task local storage map and pins the map to bpffs for reuse.
 *
 * @param pid The pid of the process whose task local storage will be used
 * @param data_map The bpf task local storage map defined in the bpf uptr KV
 * store header
 * @param pin_path The path of which the task local storage map will be pinned
 * (must be under bpffs)
 * @param kvp A pointer to a list of key-value pairs for initializing the KV
 * store. If kv_pair::val is NULL, the value will be initialized. If kvp is not
 * provided, a default of 16-key, 256-byte value storage KV store will be
 * created. Users can still initialize the KV store later using kv_store_set().
 * @return A pointer to a KV store object on success; NULL on error
 */
struct kv_store *kv_store_init(int pid, struct bpf_map *data_map, const char *pin_path,
			       struct kv_pairs *kvp);

/**
 * @brief kv_store_reuse() tells libbpf to reuse the task localstorage map
 * associated with a KV store object before loading a new bpf program. It must
 * be called before loading a bpf program using libbpf so that no new map is
 * created (e.g., call it after skeleton __open() and before __load())
 *
 * @param kvs A pointer to a KV store object
 * @param data_map The bpf task local storage map defined in the bpf uptr KV
 * store header
 * @return 0 on success; negative error code, otherwise
 */
int kv_store_reuse(struct kv_store *kvs, struct bpf_map *data_map);

/**
 * @brief kv_store_update() updates a KV store object according a list of
 * key-value pairs.
 *
 * @param kvs A pointer to a KV store object
 * @param kvp A pointer to a list of key-value pairs for updating the KV store.
 * Keys in kvs that do not present in kvp or exist but have different value
 * sizes will be deleted. New key-value pairs will be set. For existing
 * key-value pairs, if kv_pair::val is provided, the value will be updated.
 * @return A pointer to a KV store object on success; NULL on error
 */
void kv_store_update(struct kv_store *kvs, struct kv_pairs *kvp);

/**
 * @brief kv_store_get() gets the value corresponding to a key.
 *
 * @param kvs A pointer to a KV store object
 * @param key An integer key, whose value will be retrieved
 * @return A pointer to the value corresponding to the key on success; NULL if
 * the key-value pair does not exist
 */
void *kv_store_get(struct kv_store *kvs, int key);

/**
 * @brief kv_store_set() sets the value corresponding to a key. If the key already
 * exists, updates the value. Otherwise, creates a new key-value pair.
 *
 * @param kvs A pointer to a KV store object
 * @param key The integer key of the value to be set. Must be within the range
 * [0, 1023]
 * @param val A pointer to the value to be stored. Cannot be NULL
 * @param val_size The size of the value in bytes. Must be within [0, 4096]
 * @return 0 on success; negative error code, otherwise
 */
int kv_store_set(struct kv_store *kvs, int key, void *val, unsigned int val_size);

/**
 * @brief kv_store_delete() deletes a key-value pair.
 *
 * @param kvs A pointer to a KV store object
 * @param key The integer key of the key-value pair to be deleted
 */
void kv_store_delete(struct kv_store *kvs, int key);

#endif
