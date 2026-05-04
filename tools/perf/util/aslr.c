// SPDX-License-Identifier: GPL-2.0
#include "aslr.h"

#include "addr_location.h"
#include "debug.h"
#include "event.h"
#include "evsel.h"
#include "machine.h"
#include "map.h"
#include "thread.h"
#include "tool.h"
#include "session.h"
#include "data.h"
#include "dso.h"

#include <internal/lib.h>  /* page_size */
#include <linux/compiler.h>
#include <linux/zalloc.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

static int skipn(int fd, u64 n)
{
	char buf[4096];
	ssize_t ret;

	while (n > 0) {
		ret = read(fd, buf, (n < (u64)sizeof(buf) ? n : (u64)sizeof(buf)));
		if (ret <= 0)
			return ret;
		n -= ret;
	}

	return 0;
}

/**
 * struct remap_addresses_key - Key for mapping original addresses to remapped ones.
 * @dso: Pointer to the DSO (Dynamic Shared Object) associated with the mapping.
 * @invariant: Unique offset invariant within the VMA (Virtual Memory Area).
 *             Calculated as `start - pgoff`. This value remains constant when
 *             perf's internal `maps__fixup_overlap_and_insert` splits a map into
 *             fragmented VMA pieces due to overlapping events, allowing us to
 *             resolve split maps consistently back to the original VMA.
 * @pid: Process ID associated with the mapping.
 */
struct remap_addresses_key {
	struct dso *dso;
	u64 invariant;
	pid_t pid;
};

struct aslr_mapping {
	struct list_head node;
	u64 orig_start;
	u64 len;
	u64 remap_start;
};

struct aslr_tool {
	/** @tool: The tool implemented here and a pointer to a delegate to process the data. */
	struct delegate_tool tool;
	/** @machines: The machines with the input, not remapped, virtual address layout. */
	struct machines machines;
	/** @event_copy: Buffer used to create an event to pass to the delegate. */
	char event_copy[PERF_SAMPLE_MAX_SIZE];
	/** @remap_addresses: mapping from remap_addresses_key to remapped address. */
	struct hashmap remap_addresses;
	/** @top_addresses: mapping from process to max remapped address. */
	struct hashmap top_addresses;
};

static const pid_t kernel_pid = -1;

/* Start remapping user processes from a small non-zero offset. */
static const u64 user_space_start = 0x200000;
static const u64 kernel_space_start = 0xffff800010000000;

static size_t remap_addresses__hash(long _key, void *ctx __maybe_unused)
{
	struct remap_addresses_key *key = (struct remap_addresses_key *)_key;

	return (size_t)key->dso ^ key->invariant ^ key->pid;
}

static bool remap_addresses__equal(long _key1, long _key2, void *ctx __maybe_unused)
{
	struct remap_addresses_key *key1 = (struct remap_addresses_key *)_key1;
	struct remap_addresses_key *key2 = (struct remap_addresses_key *)_key2;

	return key1->dso == key2->dso &&
	       key1->invariant == key2->invariant &&
	       key1->pid == key2->pid;
}

static size_t top_addresses__hash(long key, void *ctx __maybe_unused)
{
	return key;
}

static bool top_addresses__equal(long key1, long key2, void *ctx __maybe_unused)
{
	return key1 == key2;
}

static u64 round_up_to_page_size(u64 addr)
{
	return (addr + page_size - 1) & ~((u64)page_size - 1);
}

static u64 aslr_tool__remap_address(struct aslr_tool *aslr,
				    struct thread *aslr_thread,
				    u8 cpumode,
				    u64 addr)
{
	struct addr_location al;
	struct remap_addresses_key key;
	u64 *remapped_invariant_ptr = NULL;
	u64 remap_addr = 0;
	u8 effective_cpumode = cpumode;

	if (!aslr_thread)
		return 0; /* No thread. */

	addr_location__init(&al);
	if (!thread__find_map(aslr_thread, cpumode, addr, &al)) {
		/*
		 * If lookup fails with specified cpumode, try fallback to the other space
		 * to be robust against bad cpumode in samples.
		 */
		if (cpumode == PERF_RECORD_MISC_KERNEL)
			effective_cpumode = PERF_RECORD_MISC_USER;
		else if (cpumode == PERF_RECORD_MISC_USER)
			effective_cpumode = PERF_RECORD_MISC_KERNEL;
		else if (cpumode == PERF_RECORD_MISC_GUEST_KERNEL)
			effective_cpumode = PERF_RECORD_MISC_GUEST_USER;
		else if (cpumode == PERF_RECORD_MISC_GUEST_USER)
			effective_cpumode = PERF_RECORD_MISC_GUEST_KERNEL;

		if (!thread__find_map(aslr_thread, effective_cpumode, addr, &al)) {
			addr_location__exit(&al);
			return 0; /* No mmap. */
		}
	}

	key.dso = map__dso(al.map);
	key.invariant = map__start(al.map) - map__pgoff(al.map);
	key.pid = effective_cpumode == PERF_RECORD_MISC_KERNEL ? kernel_pid : aslr_thread->pid_;

	if (hashmap__find(&aslr->remap_addresses, &key, &remapped_invariant_ptr)) {
		remap_addr = *remapped_invariant_ptr + map__pgoff(al.map) +
			     (addr - map__start(al.map));
	} else {
		pr_debug("Cannot find a remapped entry for address %lx in mapping %lx(%lx) for pid=%d\n",
			 addr, map__start(al.map), map__size(al.map), key.pid);
	}

	addr_location__exit(&al);
	return remap_addr;
}

static u64 aslr_tool__remap_mapping(struct aslr_tool *aslr,
				    struct thread *aslr_thread,
				    u8 cpumode,
				    u64 start, u64 len, u64 pgoff)
{
	struct addr_location al;
	struct remap_addresses_key key;
	u64 remap_addr = 0;
	u64 *remapped_invariant_ptr = NULL;
	u64 *max_addr_ptr = NULL;
	bool is_contiguous = false;
	bool first_mapping = false;
	bool key_found = false;

	if (!aslr_thread)
		return 0; /* No thread. */

	addr_location__init(&al);
	if (thread__find_map(aslr_thread, cpumode, start, &al))
		key.dso = map__dso(al.map);
	else
		key.dso = NULL;

	key.invariant = start - pgoff;
	key.pid = cpumode == PERF_RECORD_MISC_KERNEL ? kernel_pid : aslr_thread->pid_;

	if (hashmap__find(&aslr->remap_addresses, &key, &remapped_invariant_ptr)) {
		remap_addr = *remapped_invariant_ptr + pgoff;
		key_found = true;
	} else {
		struct addr_location prev_al;

		addr_location__init(&prev_al);
		if (thread__find_map(aslr_thread, cpumode, start - 1, &prev_al)) {
			if (map__start(prev_al.map) + map__size(prev_al.map) == start) {
				is_contiguous = true;
			} else {
				pr_debug("Previous mmap [%lx, %lx] overlaps current map [%lx, %lx]\n",
					 map__start(prev_al.map),
					 map__start(prev_al.map) + map__size(prev_al.map),
					 start, start+len);
			}
		}
		addr_location__exit(&prev_al);

		if (!hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr)) {
			first_mapping = true;
			remap_addr = (cpumode == PERF_RECORD_MISC_KERNEL ?
				      kernel_space_start : user_space_start);
		} else {
			remap_addr = *max_addr_ptr;
		}

		remap_addr = round_up_to_page_size(remap_addr);
		if (!is_contiguous && !first_mapping)
			remap_addr += page_size;

		{
			struct remap_addresses_key *new_key = malloc(sizeof(*new_key));
			u64 *new_val = malloc(sizeof(u64));

			if (!new_key || !new_val) {
				free(new_key);
				free(new_val);
				addr_location__exit(&al);
				return 0;
			}
			*new_key = key;
			new_key->dso = dso__get(key.dso);
			*new_val = remap_addr - pgoff;

			if (hashmap__add(&aslr->remap_addresses, new_key, new_val) != 0) {
				dso__put(new_key->dso);
				free(new_key);
				free(new_val);
				addr_location__exit(&al);
				return 0;
			}
		}
	}

	/* Update top_addresses */
	{
		u64 *new_max = malloc(sizeof(u64));
		u64 *old_val = NULL;
		int err;

		if (!new_max) {
			struct remap_addresses_key *old_key = NULL;
			u64 *old_val_remap = NULL;

			if (!key_found) {
				hashmap__delete(&aslr->remap_addresses, &key,
						&old_key, &old_val_remap);
				if (old_key)
					dso__put(old_key->dso);
				free(old_key);
				free(old_val_remap);
			}
			addr_location__exit(&al);
			return 0;
		}
		*new_max = remap_addr + len;

		if (hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr)) {
			if (*max_addr_ptr > *new_max)
				*new_max = *max_addr_ptr;
		}

		err = hashmap__insert(&aslr->top_addresses, key.pid, new_max,
					  (first_mapping && !key_found) ?
						HASHMAP_ADD : HASHMAP_UPDATE,
					  NULL, &old_val);
		if (err) {
			struct remap_addresses_key *old_key = NULL;
			u64 *old_val_remap = NULL;

			free(new_max);
			if (!key_found) {
				hashmap__delete(&aslr->remap_addresses, &key,
						&old_key, &old_val_remap);
				if (old_key)
					dso__put(old_key->dso);
				free(old_key);
				free(old_val_remap);
			}
			addr_location__exit(&al);
			return 0;
		}
		free(old_val);
	}

	addr_location__exit(&al);
	return remap_addr;
}

static u64 aslr_tool__remap_ksymbol(struct aslr_tool *aslr,
				    struct thread *aslr_thread,
				    u64 addr, u32 len)
{
	struct addr_location al;
	struct remap_addresses_key key;
	u64 remap_addr = 0;
	u64 *remapped_invariant_ptr = NULL;
	u64 *max_addr_ptr = NULL;
	bool first_mapping = false;

	if (!aslr_thread)
		return 0; /* No thread. */

	addr_location__init(&al);
	if (thread__find_map(aslr_thread, PERF_RECORD_MISC_KERNEL, addr, &al)) {
		key.dso = map__dso(al.map);
		key.invariant = map__start(al.map) - map__pgoff(al.map);
	} else {
		key.dso = NULL;
		key.invariant = addr; /* pgoff is 0 for ksymbols */
	}
	key.pid = aslr_thread->pid_;

	if (hashmap__find(&aslr->remap_addresses, &key, &remapped_invariant_ptr)) {
		if (al.map)
			remap_addr = *remapped_invariant_ptr + map__pgoff(al.map) +
				     (addr - map__start(al.map));
		else
			remap_addr = *remapped_invariant_ptr;
		addr_location__exit(&al);
		return remap_addr;
	}

	if (!hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr)) {
		first_mapping = true;
		remap_addr = kernel_space_start;
	} else {
		remap_addr = *max_addr_ptr;
	}

	remap_addr = round_up_to_page_size(remap_addr) + page_size;

	{
		struct remap_addresses_key *new_key = malloc(sizeof(*new_key));
		u64 *new_val = malloc(sizeof(u64));

		if (!new_key || !new_val) {
			free(new_key);
			free(new_val);
			addr_location__exit(&al);
			return 0;
		}
		*new_key = key;
		new_key->dso = dso__get(key.dso);
		if (al.map)
			*new_val = remap_addr - (addr - map__start(al.map)) - map__pgoff(al.map);
		else
			*new_val = remap_addr;

		if (hashmap__add(&aslr->remap_addresses, new_key, new_val) < 0) {
			dso__put(new_key->dso);
			free(new_key);
			free(new_val);
			addr_location__exit(&al);
			return 0;
		}
	}

	{
		u64 *new_max = malloc(sizeof(u64));
		u64 *old_val = NULL;
		int err;

		if (!new_max) {
			struct remap_addresses_key *old_key = NULL;
			u64 *old_val_remap = NULL;

			hashmap__delete(&aslr->remap_addresses, &key, &old_key, &old_val_remap);
			if (old_key)
				dso__put(old_key->dso);
			free(old_key);
			free(old_val_remap);
			addr_location__exit(&al);
			return 0;
		}
		*new_max = remap_addr + len;

		if (hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr)) {
			if (*max_addr_ptr > *new_max)
				*new_max = *max_addr_ptr;
		}

		err = hashmap__insert(&aslr->top_addresses, key.pid, new_max,
					  first_mapping ?
						HASHMAP_ADD : HASHMAP_UPDATE,
					  NULL, &old_val);
		if (err) {
			struct remap_addresses_key *old_key = NULL;
			u64 *old_val_remap = NULL;

			free(new_max);
			hashmap__delete(&aslr->remap_addresses, &key, &old_key, &old_val_remap);
			if (old_key)
				dso__put(old_key->dso);
			free(old_key);
			free(old_val_remap);
			addr_location__exit(&al);
			return 0;
		}
		free(old_val);
	}

	addr_location__exit(&al);
	return remap_addr;
}


static int aslr_tool__process_mmap(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	union perf_event *new_event;
	u8 cpumode;
	struct thread *thread;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	new_event = (union perf_event *)aslr->event_copy;
	cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_mmap(tool, event, sample, aslr_machine);
	if (err)
		return err;

	thread = machine__findnew_thread(aslr_machine, event->mmap.pid, event->mmap.tid);
	if (!thread)
		return -ENOMEM;
	memcpy(&new_event->mmap, &event->mmap, event->mmap.header.size);
	/* Remaps the mmap.start. */
	new_event->mmap.start = aslr_tool__remap_mapping(aslr, thread, cpumode,
							   event->mmap.start,
							   event->mmap.len,
							   event->mmap.pgoff);
	err = delegate->mmap(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static int aslr_tool__process_mmap2(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	union perf_event *new_event;
	u8 cpumode;
	struct thread *thread;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	new_event = (union perf_event *)aslr->event_copy;
	cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_mmap2(tool, event, sample, aslr_machine);
	if (err)
		return err;

	thread = machine__findnew_thread(aslr_machine, event->mmap2.pid, event->mmap2.tid);
	if (!thread)
		return -ENOMEM;
	memcpy(&new_event->mmap2, &event->mmap2, event->mmap2.header.size);
	/* Remaps the mmap.start. */
	new_event->mmap2.start = aslr_tool__remap_mapping(aslr, thread, cpumode,
							   event->mmap2.start,
							   event->mmap2.len,
							   event->mmap2.pgoff);
	err = delegate->mmap2(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static int aslr_tool__process_comm(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_comm(tool, event, sample, aslr_machine);
	if (err)
		return err;

	return delegate->comm(delegate, event, sample, machine);
}

static int aslr_tool__process_fork(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_fork(tool, event, sample, aslr_machine);
	if (err)
		return err;

	return delegate->fork(delegate, event, sample, machine);
}

static int aslr_tool__process_exit(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_exit(tool, event, sample, aslr_machine);
	if (err)
		return err;

	return delegate->exit(delegate, event, sample, machine);
}

static int aslr_tool__process_text_poke(const struct perf_tool *tool,
					union perf_event *event,
					struct perf_sample *sample,
					struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	union perf_event *new_event;
	u8 cpumode;
	struct thread *thread;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	new_event = (union perf_event *)aslr->event_copy;
	cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	thread = machine__findnew_thread(aslr_machine, sample->pid, sample->tid);
	if (!thread)
		return -ENOMEM;
	memcpy(&new_event->text_poke, &event->text_poke, event->text_poke.header.size);
	new_event->text_poke.addr = aslr_tool__remap_address(aslr, thread, cpumode,
							     event->text_poke.addr);

	err = delegate->text_poke(delegate, new_event, sample, machine);

	thread__put(thread);
	return err;
}

static int aslr_tool__process_ksymbol(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	union perf_event *new_event;
	struct thread *thread;
	struct machine *aslr_machine;
	int err;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	new_event = (union perf_event *)aslr->event_copy;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_ksymbol(tool, event, sample, aslr_machine);
	if (err)
		return err;

	thread = machine__findnew_thread(aslr_machine, kernel_pid, 0);
	if (!thread)
		return -ENOMEM;
	memcpy(&new_event->ksymbol, &event->ksymbol, event->ksymbol.header.size);
	/* Remaps the ksymbol.start */
	new_event->ksymbol.addr = aslr_tool__remap_ksymbol(aslr, thread,
							   event->ksymbol.addr, event->ksymbol.len);

	err = delegate->ksymbol(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static int aslr_tool__process_sample(const struct perf_tool *tool, union perf_event *event,
				struct perf_sample *sample,
				struct evsel *evsel, struct machine *machine)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	int ret;
	u64 sample_type;
	struct thread *thread;
	struct machine *aslr_machine;
	__u64 max_i;
	__u64 max_j;
	union perf_event *new_event;
	struct perf_sample new_sample;
	__u64 *in_array, *out_array;
	u8 cpumode;
	u64 addr;
	size_t i;
	size_t j;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	ret = -EFAULT;
	sample_type = evsel->core.attr.sample_type;
	max_i = (event->header.size - sizeof(struct perf_event_header)) / sizeof(__u64);
	max_j = (PERF_SAMPLE_MAX_SIZE - sizeof(struct perf_event_header)) / sizeof(__u64);
	new_event = (union perf_event *)aslr->event_copy;
	cpumode = sample->cpumode;
	i = 0;
	j = 0;

	aslr_machine = machines__findnew(&aslr->machines, machine->pid);
	if (!aslr_machine)
		return -ENOMEM;

	thread = machine__findnew_thread(aslr_machine, sample->pid, sample->tid);

	if (!thread)
		return -ENOMEM;

	if (max_i > PERF_SAMPLE_MAX_SIZE / sizeof(u64))
		goto out_put;



	new_event->sample.header = event->sample.header;

	in_array = &event->sample.array[0];
	out_array = &new_event->sample.array[0];

#define CHECK_BOUNDS(required_i, required_j) \
	do { \
		if (i + (required_i) > max_i || j + (required_j) > max_j) { \
			ret = -EFAULT; \
			goto out_put; \
		} \
	} while (0)

#define COPY_U64() \
	do { \
		CHECK_BOUNDS(1, 1); \
		out_array[j++] = in_array[i++]; \
	} while (0)

#define REMAP_U64(addr_field) \
	do { \
		CHECK_BOUNDS(1, 1); \
		out_array[j++] = aslr_tool__remap_address(aslr, thread, cpumode, addr_field); \
		i++; \
	} while (0)

	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		COPY_U64(); /* id */
	if (sample_type & PERF_SAMPLE_IP)
		REMAP_U64(sample->ip);
	if (sample_type & PERF_SAMPLE_TID)
		COPY_U64(); /* pid, tid */
	if (sample_type & PERF_SAMPLE_TIME)
		COPY_U64(); /* time */
	if (sample_type & PERF_SAMPLE_ADDR)
		REMAP_U64(sample->addr);
	if (sample_type & PERF_SAMPLE_ID)
		COPY_U64(); /* id */
	if (sample_type & PERF_SAMPLE_STREAM_ID)
		COPY_U64(); /* stream_id */
	if (sample_type & PERF_SAMPLE_CPU)
		COPY_U64(); /* cpu, res */
	if (sample_type & PERF_SAMPLE_PERIOD)
		COPY_U64(); /* period */
	if (sample_type & PERF_SAMPLE_READ) {
		if ((evsel->core.attr.read_format & PERF_FORMAT_GROUP) == 0) {
			COPY_U64(); /* value */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
				COPY_U64(); /* time_enabled */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
				COPY_U64(); /* time_running */
			if (evsel->core.attr.read_format & PERF_FORMAT_ID)
				COPY_U64(); /* id */
			if (evsel->core.attr.read_format & PERF_FORMAT_LOST)
				COPY_U64(); /* lost */
		} else {
			u64 nr;

			CHECK_BOUNDS(1, 1);
			nr = out_array[j++] = in_array[i++];
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
				COPY_U64(); /* time_enabled */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
				COPY_U64(); /* time_running */
			for (u64 cntr = 0; cntr < nr; cntr++) {
				COPY_U64(); /* value */
				if (evsel->core.attr.read_format & PERF_FORMAT_ID)
					COPY_U64(); /* id */
				if (evsel->core.attr.read_format & PERF_FORMAT_LOST)
					COPY_U64(); /* lost */
			}
		}
	}
	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		u64 nr;

		CHECK_BOUNDS(1, 1);
		nr = out_array[j++] = in_array[i++];

		for (u64 cntr = 0; cntr < nr; cntr++) {
			CHECK_BOUNDS(1, 1);
			addr = in_array[i++];
			if (addr >= PERF_CONTEXT_MAX) {
				out_array[j++] = addr;
				switch (addr) {
				case PERF_CONTEXT_HV:
					cpumode = PERF_RECORD_MISC_HYPERVISOR;
					break;
				case PERF_CONTEXT_KERNEL:
					cpumode = PERF_RECORD_MISC_KERNEL;
					break;
				case PERF_CONTEXT_USER:
					cpumode = PERF_RECORD_MISC_USER;
					break;
				case PERF_CONTEXT_GUEST:
					cpumode = PERF_RECORD_MISC_GUEST_KERNEL;
					break;
				case PERF_CONTEXT_GUEST_KERNEL:
					cpumode = PERF_RECORD_MISC_GUEST_KERNEL;
					break;
				case PERF_CONTEXT_GUEST_USER:
					cpumode = PERF_RECORD_MISC_GUEST_USER;
					break;
				case PERF_CONTEXT_USER_DEFERRED:
					/*
					 * Immediately followed by a 64-bit
					 * stitching cookie. Skip/Copy it!
					 */
					CHECK_BOUNDS(1, 1);
					out_array[j++] = in_array[i++];
					cntr++;
					break;
				default:
					pr_debug("invalid callchain context: %"PRIx64"\n", addr);
					ret = 0;
					goto out_put;
				}
				continue;
			}
			out_array[j++] = aslr_tool__remap_address(aslr, thread, cpumode, addr);
		}
	}
	if (sample_type & PERF_SAMPLE_RAW) {
		size_t bytes = sizeof(u32) + sample->raw_size;
		size_t u64_words = (bytes + 7) / 8;

		if (i + u64_words > max_i || j + u64_words > max_j) {
			ret = -EFAULT;
			goto out_put;
		}
		memcpy(&out_array[j], &in_array[i], bytes);
		i += u64_words;
		j += u64_words;
		/*
		 * TODO: certain raw samples can be remapped, such as
		 * tracepoints by examining their fields.
		 */
		pr_debug("Dropping raw samples as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}
	if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
		u64 nr;

		CHECK_BOUNDS(1, 1);
		nr = out_array[j++] = in_array[i++];

		if (evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_HW_INDEX)
			COPY_U64(); /* hw_idx */

		if (nr > (ULLONG_MAX / 3)) {
			ret = -EFAULT;
			goto out_put;
		}
		if (nr * 3 > max_i - i || nr * 3 > max_j - j) {
			ret = -EFAULT;
			goto out_put;
		}
		for (u64 cntr = 0; cntr < nr; cntr++) {
			out_array[j++] = aslr_tool__remap_address(aslr, thread,
								  sample->cpumode,
								  in_array[i++]); /* from */
			out_array[j++] = aslr_tool__remap_address(aslr, thread,
								  sample->cpumode,
								  in_array[i++]); /* to */
			out_array[j++] = in_array[i++]; /* flags */
		}
		if (evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_COUNTERS) {
			if (nr > max_i - i || nr > max_j - j) {
				ret = -EFAULT;
				goto out_put;
			}
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
			/* TODO: confirm branch counters don't leak ASLR information. */
			pr_debug("Dropping sample branch counters as possible ASLR leak\n");
			ret = 0;
			goto out_put;
		}
	}
	if (sample_type & PERF_SAMPLE_REGS_USER) {
		u64 abi;

		COPY_U64(); /* abi */
		abi = out_array[j-1];
		if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_user);

			if (nr > max_i - i || nr > max_j - j) {
				ret = -EFAULT;
				goto out_put;
			}
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping regs user sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}
	if (sample_type & PERF_SAMPLE_STACK_USER) {
		u64 size;

		CHECK_BOUNDS(1, 1);
		size = out_array[j++] = in_array[i++];
		if (size > 0) {
			size_t u64_words = size / 8 + (size % 8 ? 1 : 0);

			if (u64_words > max_i - i || u64_words > max_j - j) {
				ret = -EFAULT;
				goto out_put;
			}
			memcpy(&out_array[j], &in_array[i], size);
			if (size % 8) {
				size_t pad = 8 - (size % 8);

				memset(((char *)&out_array[j]) + size, 0, pad);
			}
			i += u64_words;
			j += u64_words;

			COPY_U64(); /* dyn_size */
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping stack user sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}
	if (sample_type & PERF_SAMPLE_WEIGHT_TYPE)
		COPY_U64(); /* perf_sample_weight */
	if (sample_type & PERF_SAMPLE_DATA_SRC)
		COPY_U64(); /* data_src */
	if (sample_type & PERF_SAMPLE_TRANSACTION)
		COPY_U64(); /* transaction */
	if (sample_type & PERF_SAMPLE_REGS_INTR) {
		u64 abi;

		COPY_U64(); /* abi */
		abi = out_array[j-1];
		if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_intr);

			if (nr > max_i - i || nr > max_j - j) {
				ret = -EFAULT;
				goto out_put;
			}
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping interrupt register sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}
	if (sample_type & PERF_SAMPLE_PHYS_ADDR) {
		COPY_U64(); /* phys_addr */
		/* TODO: can this be less conservative? */
		pr_debug("Dropping physical address sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}
	if (sample_type & PERF_SAMPLE_CGROUP)
		COPY_U64(); /* cgroup */
	if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE)
		COPY_U64(); /* data_page_size */
	if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE)
		COPY_U64(); /* code_page_size */

	if (sample_type & PERF_SAMPLE_AUX) {
		u64 size;

		CHECK_BOUNDS(1, 1);
		size = out_array[j++] = in_array[i++];
		if (size > 0) {
			size_t u64_words = size / 8 + (size % 8 ? 1 : 0);

			if (u64_words > max_i - i || u64_words > max_j - j) {
				ret = -EFAULT;
				goto out_put;
			}
			memcpy(&out_array[j], &in_array[i], size);
			if (size % 8) {
				size_t pad = 8 - (size % 8);

				memset(((char *)&out_array[j]) + size, 0, pad);
			}
			i += u64_words;
			j += u64_words;
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping aux sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}

	if (evsel__is_offcpu_event(evsel)) {
		/* TODO: can this be less conservative? */
		pr_debug("Dropping off-CPU sample as possible ASLR leak\n");
		ret = 0;
		goto out_put;
	}

	new_event->sample.header.size = sizeof(struct perf_event_header) + j * sizeof(u64);

	perf_sample__init(&new_sample, /*all=*/ true);
	ret = evsel__parse_sample(evsel, new_event, &new_sample);
	if (ret) {
		perf_sample__exit(&new_sample);
		goto out_put;
	}

	ret = delegate->sample(delegate, new_event, &new_sample, evsel, machine);
	perf_sample__exit(&new_sample);

out_put:
	thread__put(thread);
	return ret;
}

#undef CHECK_BOUNDS
#undef COPY_U64
#undef REMAP_U64


static int aslr_tool__process_attr(const struct perf_tool *tool,
				   union perf_event *event,
				   struct evlist **pevlist)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct perf_tool *delegate;
	union perf_event *new_event;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);
	delegate = aslr->tool.delegate;
	new_event = (union perf_event *)aslr->event_copy;

	memcpy(&new_event->attr, &event->attr, event->attr.header.size);
	if (new_event->attr.attr.type == PERF_TYPE_BREAKPOINT)
		new_event->attr.attr.bp_addr = 0;  /* Conservatively remove addresses. */

	return delegate->attr(delegate, new_event, pevlist);
}

static s64 aslr_tool__process_auxtrace(const struct perf_tool *tool __maybe_unused,
				       struct perf_session *session,
				       union perf_event *event)
{
	if (perf_data__is_pipe(session->data)) {
		int err = skipn(perf_data__fd(session->data), event->auxtrace.size);

		if (err < 0)
			return err;
	}
	return event->auxtrace.size;
}

static int aslr_tool__process_auxtrace_info(const struct perf_tool *tool __maybe_unused,
					    struct perf_session *session __maybe_unused,
					    union perf_event *event __maybe_unused)
{
	return 0;
}

static int aslr_tool__process_auxtrace_error(const struct perf_tool *tool __maybe_unused,
					     struct perf_session *session __maybe_unused,
					     union perf_event *event __maybe_unused)
{
	return 0;
}

static void aslr_tool__init(struct aslr_tool *aslr, struct perf_tool *delegate)
{
	delegate_tool__init(&aslr->tool, delegate);
	aslr->tool.tool.ordered_events = true;

	machines__init(&aslr->machines);

	hashmap__init(&aslr->remap_addresses,
		      remap_addresses__hash, remap_addresses__equal,
		      /*ctx=*/NULL);
	hashmap__init(&aslr->top_addresses,
		      top_addresses__hash, top_addresses__equal,
		      /*ctx=*/NULL);

	aslr->tool.tool.sample	= aslr_tool__process_sample;
	/* read - reads a counter, okay to delegate. */
	aslr->tool.tool.mmap	= aslr_tool__process_mmap;
	aslr->tool.tool.mmap2	= aslr_tool__process_mmap2;
	aslr->tool.tool.comm	= aslr_tool__process_comm;
	aslr->tool.tool.fork	= aslr_tool__process_fork;
	aslr->tool.tool.exit	= aslr_tool__process_exit;
	/* namesspaces, cgroup, lost, lost_sample, aux, */
	/* itrace_start, aux_output_hw_id, context_switch, throttle, unthrottle */
	/* - no virtual addresses. */
	aslr->tool.tool.ksymbol	= aslr_tool__process_ksymbol;
	/* bpf - no virtual address. */
	aslr->tool.tool.text_poke = aslr_tool__process_text_poke;
	aslr->tool.tool.attr = aslr_tool__process_attr;
	/* event_update, tracing_data, finished_round, build_id, id_index, */
	/* event_update, tracing_data, finished_round, build_id, id_index, */
	/* auxtrace_info, auxtrace_error, time_conv, thread_map, cpu_map, */
	/* stat_config, stat, feature, finished_init, bpf_metadata, compressed, */
	/* auxtrace - no virtual addresses. */
	aslr->tool.tool.auxtrace = aslr_tool__process_auxtrace;
	aslr->tool.tool.auxtrace_info = aslr_tool__process_auxtrace_info;
	aslr->tool.tool.auxtrace_error = aslr_tool__process_auxtrace_error;
}

struct perf_tool *aslr_tool__new(struct perf_tool *delegate)
{
	struct aslr_tool *aslr = zalloc(sizeof(*aslr));

	if (!aslr)
		return NULL;

	aslr_tool__init(aslr, delegate);
	return &aslr->tool.tool;
}

void aslr_tool__delete(struct perf_tool *tool)
{
	struct delegate_tool *del_tool;
	struct aslr_tool *aslr;
	struct hashmap_entry *cur;
	size_t bkt;

	if (!tool)
		return;

	del_tool = container_of(tool, struct delegate_tool, tool);
	aslr = container_of(del_tool, struct aslr_tool, tool);

	hashmap__for_each_entry(&aslr->remap_addresses, cur, bkt) {
		struct remap_addresses_key *key = (struct remap_addresses_key *)cur->pkey;

		if (key)
			dso__put(key->dso);
		zfree(&cur->pkey);
		zfree(&cur->pvalue);
	}
	hashmap__for_each_entry(&aslr->top_addresses, cur, bkt) {
		zfree(&cur->pvalue);
	}

	hashmap__clear(&aslr->remap_addresses);
	hashmap__clear(&aslr->top_addresses);
	machines__exit(&aslr->machines);
	free(aslr);
}
