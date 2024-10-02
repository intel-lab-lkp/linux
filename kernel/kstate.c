// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/kexec.h>
#include <linux/kstate.h>

static LIST_HEAD(states);

struct kstate_entry {
	int state_id;
	int version_id;
	int instance_id;
	int size;
	DECLARE_FLEX_ARRAY(u8, data);
};

void *save_kstate(void *stream, int id, const struct kstate_description *kstate,
		void *obj)
{
	const struct kstate_field *field = kstate->fields;
	struct kstate_entry *ke = stream;

	stream = ke->data;

	ke->state_id = kstate->id;
	ke->version_id = kstate->version_id;
	ke->instance_id = id;

	while (field->flags != KS_END) {
		void *first,  *cur;
		int n_elems = 1;
		int size, i;

		first = obj + field->offset;

		if (field->flags & KS_POINTER)
			first = *(void **)(obj + field->offset);
		if (field->count)
			n_elems = field->count();
		size = field->size;
		for (i = 0; i < n_elems; i++) {
			cur = first + i * size;

			if (field->flags & KS_ARRAY_OF_POINTER)
				cur = *(void **)cur;

			if (field->flags & KS_STRUCT)
				stream = save_kstate(stream, 0, field->ksd, cur);
			else if (field->flags & KS_CUSTOM) {
				if (field->save)
					stream += field->save(stream, cur, field);
			} else if (field->flags & (KS_SIMPLE|KS_POINTER)) {
				memcpy(stream, cur, size);
				stream += size;
			} else
				WARN_ON_ONCE(1);

		}
		field++;

	}

	ke->size = (u8 *)stream - ke->data;
	return stream;
}

void save_migrate_state(unsigned long mig_stream)
{
	struct state_entry *se;
	struct kstate_entry *ke;
	void *dest;
	struct page *page;

	page = boot_pfn_to_page(mig_stream >> PAGE_SHIFT);
	arch_kexec_post_alloc_pages(page_address(page), 512, 0);
	dest = page_address(page);
	list_for_each_entry(se, &states, list)
		dest = save_kstate(dest, se->id, se->kstd, se->obj);
	ke = dest;
	ke->state_id = KSTATE_LAST_ID;
}

void *restore_kstate(struct kstate_entry *ke, int id,
		const struct kstate_description *kstate, void *obj)
{
	const struct kstate_field *field = kstate->fields;
	u8 *stream = ke->data;

	WARN_ONCE(ke->version_id != kstate->version_id, "version mismatch %d %d\n",
		ke->version_id, kstate->version_id);

	WARN_ONCE(ke->instance_id != id, "instance id mismatch %d %d\n",
		ke->instance_id, id);

	while (field->flags != KS_END) {
		void *first, *cur;
		int n_elems = 1;
		int size, i;

		first = obj + field->offset;
		if (field->flags & KS_POINTER)
			first = *(void **)(obj + field->offset);
		if (field->count)
			n_elems = field->count();
		size = field->size;
		for (i = 0; i < n_elems; i++) {
			cur = first + i * size;

			if (field->flags & KS_ARRAY_OF_POINTER)
				cur = *(void **)cur;

			if (field->flags & KS_STRUCT)
				stream = restore_kstate((struct kstate_entry *)stream,
							0, field->ksd, cur);
			else if (field->flags & KS_CUSTOM) {
				if (field->restore)
					stream += field->restore(stream, cur, field);
			} else if (field->flags & (KS_SIMPLE|KS_POINTER)) {
				memcpy(cur, stream, size);
				stream += size;
			} else
				WARN_ON_ONCE(1);

		}
		field++;
	}

	return stream;
}

static void restore_migrate_state(unsigned long mig_stream,
				struct state_entry *se)
{
	char *dest;
	struct kstate_entry *ke;

	if (mig_stream == -1)
		return;

	dest = phys_to_virt(mig_stream);
	ke = (struct kstate_entry *)dest;
	while (ke->state_id != KSTATE_LAST_ID) {
		if (ke->state_id != se->kstd->id ||
		    ke->instance_id != se->id) {
			ke = (struct kstate_entry *)(ke->data + ke->size);
			continue;
		}

		restore_kstate(ke, se->id, se->kstd, se->obj);
		ke = (struct kstate_entry *)(ke->data + ke->size);
	}
}

unsigned long long migrate_stream_addr = -1;
EXPORT_SYMBOL_GPL(migrate_stream_addr);
unsigned long long migrate_stream_size;

bool is_migrate_kernel(void)
{
	return migrate_stream_addr != -1;
}

void __kstate_register(struct kstate_description *state, void *obj, struct state_entry *se)
{
	se->kstd = state;
	se->id = atomic_inc_return(&state->instance_id);
	se->obj = obj;
	list_add(&se->list, &states);
	restore_migrate_state(migrate_stream_addr, se);
}

int kstate_register(struct kstate_description *state, void *obj)
{
	struct state_entry *se;

	se = kmalloc(sizeof(*se), GFP_KERNEL);
	if (!se)
		return -ENOMEM;

	__kstate_register(state, obj, se);
	return 0;
}

int kstate_page_save(void *mig_stream, void *obj,
		const struct kstate_field *field)
{
	kstate_register_page(*(struct page **)obj, 0);
	return 0;
}

static int __init setup_migrate(char *arg)
{
	char *end;

	if (!arg)
		return -EINVAL;
	migrate_stream_addr = memparse(arg, &end);
	if (*end == '@') {
		migrate_stream_size = migrate_stream_addr;
		migrate_stream_addr = memparse(end + 1, &end);
	}
	return end > arg ? 0 : -EINVAL;
}
early_param("migrate_stream", setup_migrate);
