/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BOOTCACHE_H
#define _LINUX_BOOTCACHE_H

#include <linux/types.h>

#ifdef CONFIG_BOOTCACHE

#define BOOTCACHE_HASH_BITS 6  /* 64 buckets */

struct bootcache_entry {
	struct hlist_node node;
	char *key;
	void *data;
	size_t len;
};

/**
 * struct bootcache_info - Structure for registering a boot cache backend.
 *
 * @name:   The name of the backend.
 *
 * Callbacks:
 * @load_cache: Callback function to read and populate the framework from the cache.
 */

struct bootcache_info {
	const char *name;
	/* Callback Function Pointers */
	int (*load_cache)(void);
};

/**
 * bootcache_add_entry - Add an entry directly into the hash table
 * @entry: bootcache_entry structure
 *
 * Returns: 0 on success, entry was added, do not free it.
 *          1 on success but an existing entry was updated,
 *            free to deallocate entry.
 */
int bootcache_add_entry(struct bootcache_entry *entry);

/**
 * bootcache_register_backend - Register a backend provider with the framework
 * @bci: bootcache_info structure
 *
 * Returns: 0 on success, -EPROBE_DEFER if the frontend is not ready,
 *          -EBUSY if another backend is already registered,
 *          -EINVAL on invalid registration information.
 */
int bootcache_register_backend(struct bootcache_info *bci);

/**
 * bootcache_get - Retrieve arbitrary data from the cache
 * @name: Key to look up
 * @buf: Buffer to store retrieved data
 * @len: On input, size of buffer; on output, actual data size
 *
 * Returns: 0 on success, -EINVAL for invalid parameters,
 *          -ENOENT if not found, -ENOSPC if buffer too small
 */
int bootcache_get(const char *name, void *buf, size_t *len);

/**
 * bootcache_set - Store arbitrary data in the cache
 * @name: Key to store under
 * @data: Data to store
 * @len: Length of data
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOMEM on allocation failure
 */
int bootcache_set(const char *name, const void *data, size_t len);

#else /* !CONFIG_BOOTCACHE */

static inline int bootcache_get(const char *name, void *buf, size_t *len)
{
	return -ENOENT;
}

static inline int bootcache_set(const char *name, const void *data, size_t len)
{
	return -EOPNOTSUPP;
}

/**
 * bootcache_get_u16 - Retrieve a u16 value from the cache
 * @name: Key to look up
 * @out_val: Pointer to store the retrieved value
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOENT if not found
 */
static inline int bootcache_get_u16(const char *name, u16 *out_val)
{
	size_t len = sizeof(u16);

	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_get(name, out_val, &len);
	else
		return -ENOENT;
}

/**
 * bootcache_set_u16 - Store a u16 value in the cache
 * @name: Key to store under
 * @val: Value to store
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOMEM on allocation failure
 */
static inline int bootcache_set_u16(const char *name, u16 val)
{
	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_set(name, &val, sizeof(u16));
	else
		return -EOPNOTSUPP;
}

/**
 * bootcache_get_u32 - Retrieve a u32 value from the cache
 * @name: Key to look up
 * @out_val: Pointer to store the retrieved value
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOENT if not found
 */
static inline int bootcache_get_u32(const char *name, u32 *out_val)
{
	size_t len = sizeof(u32);

	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_get(name, out_val, &len);
	else
		return -ENOENT;
}

/**
 * bootcache_set_u32 - Store a u32 value in the cache
 * @name: Key to store under
 * @val: Value to store
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOMEM on allocation failure
 */
static inline int bootcache_set_u32(const char *name, u32 val)
{
	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_set(name, &val, sizeof(u32));
	else
		return -EOPNOTSUPP;
}

/**
 * bootcache_get_u64 - Retrieve a u64 value from the cache
 * @name: Key to look up
 * @out_val: Pointer to store the retrieved value
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOENT if not found
 */
static inline int bootcache_get_u64(const char *name, u64 *out_val)
{
	size_t len = sizeof(u64);

	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_get(name, out_val, &len);
	else
		return -ENOENT;
}

/**
 * bootcache_set_u64 - Store a u64 value in the cache
 * @name: Key to store under
 * @val: Value to store
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOMEM on allocation failure
 */
static inline int bootcache_set_u64(const char *name, u64 val)
{
	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_set(name, &val, sizeof(u64));
	else
		return -EOPNOTSUPP;
}

/**
 * bootcache_get_string - Retrieve a string from the cache
 * @name: Key to look up
 * @buf: Buffer to store retrieved string
 * @buflen: Size of buffer
 *
 * Returns: 0 on success, -EINVAL for invalid parameters,
 *          -ENOENT if not found, -ENOSPC if buffer too small
 */
static inline int bootcache_get_string(const char *name, char *buf, size_t buflen)
{
	size_t len = buflen;

	if (IS_ENABLED(CONFIG_BOOTCACHE))
		return bootcache_get(name, buf, &len);
	else
		return -ENOENT;
}

/**
 * bootcache_set_string - Store a string in the cache
 * @name: Key to store under
 * @str: Null-terminated string to store
 *
 * Returns: 0 on success, -EINVAL for invalid parameters, -ENOMEM on allocation failure
 */
static inline int bootcache_set_string(const char *name, const char *str)
{
	if (IS_ENABLED(CONFIG_BOOTCACHE)) {
		if (!str)
			return -EINVAL;
		return bootcache_set(name, str, strlen(str) + 1);
	} else {
		return -EOPNOTSUPP;
	}
}

#endif /* _LINUX_BOOTCACHE_H */
