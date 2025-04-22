/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KMEMDUMP_H
#define _KMEMDUMP_H

#define KMEMDUMP_ZONE_MAX_HANDLE 8
/**
 * struct kmemdump_zone - region mark zone information
 * @id: unique id for this zone
 * @zone: pointer to the memory area for this zone
 * @size: size of the memory area of this zone
 * @registered: bool indicating whether this zone is registered into the
 *	backend or not.
 * @handle: a string representing this region
 */
struct kmemdump_zone {
	int id;
	void *zone;
	size_t size;
	bool registered;
	char handle[KMEMDUMP_ZONE_MAX_HANDLE];
};

#define KMEMDUMP_BACKEND_MAX_NAME 128
/**
 * struct kmemdump_backend - region mark backend information
 * @name: the name of the backend
 * @register_region: callback to register region in the backend
 * @unregister_region: callback to unregister region in the backend
 */
struct kmemdump_backend {
	char name[KMEMDUMP_BACKEND_MAX_NAME];
	int (*register_region)(unsigned int id, char *, void *, size_t);
	int (*unregister_region)(unsigned int id);
};

#ifdef CONFIG_DRIVER_KMEMDUMP
int kmemdump_register(char *handle, void *zone, size_t size);
void kmemdump_unregister(int id);
#else
static inline int kmemdump_register(char *handle, void *area, size_t size)
{
	return 0;
}

static inline void kmemdump_unregister(int id)
{
}
#endif

int kmemdump_register_backend(struct kmemdump_backend *backend);
void kmemdump_unregister_backend(struct kmemdump_backend *backend);

#ifdef CONFIG_DRIVER_KMEMDUMP_COREIMAGE
int init_elfheader(struct kmemdump_backend *be);
void update_elfheader(const struct kmemdump_zone *z);
int clear_elfheader(const struct kmemdump_zone *z);
void register_coreinfo(void);
#else
static inline int init_elfheader(struct kmemdump_backend *be)
{
	return 0;
}

static inline void update_elfheader(const struct kmemdump_zone *z)
{
}

static inline int clear_elfheader(const struct kmemdump_zone *z)
{
	return 0;
}

static inline void register_coreinfo(void)
{
}
#endif
#endif
