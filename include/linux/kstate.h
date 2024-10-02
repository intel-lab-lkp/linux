/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KSTATE_H
#define _KSTATE_H

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/stringify.h>

struct kstate_description;
enum kstate_flags {
	KS_SIMPLE = (1 << 0),
	KS_POINTER = (1 << 1),
	KS_STRUCT = (1 << 2),
	KS_CUSTOM = (1 << 3),
	KS_ARRAY_OF_POINTER = (1 << 4),
	KS_END = (1UL << 31),
};

struct kstate_field {
	const char *name;
	size_t offset;
	size_t size;
	enum kstate_flags flags;
	const struct kstate_description *ksd;
	int version_id;
	int (*restore)(void *mig_stream, void *obj, const struct kstate_field *field);
	int (*save)(void *mig_stream, void *obj, const struct kstate_field *field);
	int (*count)(void);
};

enum kstate_ids {
	KSTATE_PAGE_ID,
	KSTATE_RSVD_MEM_ID,
	KSTATE_TEST_ID,
	KSTATE_TRACE_ID,
	KSTATE_TRACE_BUFFER_ID,
	KSTATE_TRACE_RING_BUFFER_ID,
	KSTATE_TRACE_BUFFER_PAGE_ID,
	KSTATE_LAST_ID = -1,
};

struct kstate_description {
	const char *name;
	enum kstate_ids id;
	atomic_t instance_id;
	int version_id;
	struct list_head state_list;

	const struct kstate_field *fields;
};

struct state_entry {
	u64 id;
	struct list_head list;
	struct kstate_description *kstd;
	void *obj;
};

static inline bool kstate_get_byte(void **mig_stream)
{
	bool ret = **(u8 **)mig_stream;
	(*mig_stream)++;
	return ret;
}
static inline void *kstate_save_byte(void *mig_stream, u8 val)
{
	*(u8 *)mig_stream = val;
	return mig_stream + sizeof(val);
}

static inline void *kstate_save_ulong(void *mig_stream, unsigned long val)
{
	*(unsigned long *)mig_stream = val;
	return mig_stream + sizeof(val);
}
static inline unsigned long kstate_get_ulong(void **mig_stream)
{
	unsigned long ret = **(unsigned long **)mig_stream;
	(*mig_stream) += sizeof(unsigned long);
	return ret;
}

#ifdef CONFIG_KSTATE
bool is_migrate_kernel(void);

void save_migrate_state(unsigned long mig_stream);

void __kstate_register(struct kstate_description *state,
		void *obj, struct state_entry *se);
int kstate_register(struct kstate_description *state, void *obj);

struct kstate_entry;
void *save_kstate(void *stream, int id, const struct kstate_description *kstate,
		void *obj);
void *restore_kstate(struct kstate_entry *ke, int id,
		const struct kstate_description *kstate, void *obj);

int kstate_page_save(void *mig_stream, void *obj,
		const struct kstate_field *field);
int kstate_register_page(struct page *page, int order);
#else

#define __kstate_register(state, obj, se)
#define kstate_register(state, obj)

static inline void save_migrate_state(unsigned long mig_stream) { }

#endif


#define KSTATE_SIMPLE(_f, _state) {			\
		.name = (__stringify(_f)),		\
		.size = sizeof_field(_state, _f),	\
		.flags = KS_SIMPLE,			\
		.offset = offsetof(_state, _f),		\
	}

#define KSTATE_POINTER(_f, _state) {			\
		.name = (__stringify(_f)),		\
		.size = sizeof(*(((_state *)0)->_f)),	\
		.flags = KS_POINTER,			\
		.offset = offsetof(_state, _f),		\
	}

#define KSTATE_END_OF_LIST() {	\
		.flags = KS_END,\
	}

#endif
