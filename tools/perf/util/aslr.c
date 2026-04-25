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

#include <internal/lib.h>  /* page_size */
#include <linux/compiler.h>
#include <errno.h>
#include <inttypes.h>

struct remap_addresses_key {
	u64 start_addr;
	pid_t pid;
};

struct aslr_tool {
	/** @tool: The tool implemented here and a pointer to a delegate to process the data. */
	struct delegate_tool tool;
	/** @machine: The machine with the input, not remapped, virtual address layout. */
	struct machine machine;
	/** @event_copy: Buffer used to create an event to pass to the delegate. */
	char event_copy[PERF_SAMPLE_MAX_SIZE];
	/** @remap_addresses: mapping from remap_addresses_key to remapped address. */
	struct hashmap remap_addresses;
	/** @top_addresses: mapping from process to max remapped address. */
	struct hashmap top_addresses;
	/** @first_kernel_mapping: flag indicating if we are still to process any kernel mapping. */
	bool first_kernel_mapping;
};

static const pid_t kernel_pid = -1;

/* Start remapping user processes from a small non-zero offset. */
static const u64 user_space_start = 0x200000;
static const u64 kernel_space_start = 0xffff800010000000;

static size_t remap_addresses__hash(long _key, void *ctx __maybe_unused)
{
	struct remap_addresses_key *key = (struct remap_addresses_key *)_key;

	return key->start_addr ^ (key->start_addr >> 12) ^ key->pid;
}

static bool remap_addresses__equal(long _key1, long _key2, void *ctx __maybe_unused)
{
	struct remap_addresses_key *key1 = (struct remap_addresses_key *)_key1;
	struct remap_addresses_key *key2 = (struct remap_addresses_key *)_key2;

	return key1->pid == key2->pid && key1->start_addr == key2->start_addr;
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
	u64 remap_addr = 0;
	u64 *remap_addr_ptr = NULL;
	u8 effective_cpumode = cpumode;

	if (!aslr_thread)
		return 0; /* No thread. */

	addr_location__init(&al);
	if (!thread__find_map(aslr_thread, cpumode, addr, &al)) {
		/*
		 * If lookup fails with specified cpumode, try fallback to the other space
		 * to be robust against bad cpumode in samples.
		 */
		effective_cpumode = (cpumode == PERF_RECORD_MISC_KERNEL) ?
				    PERF_RECORD_MISC_USER : PERF_RECORD_MISC_KERNEL;
		if (!thread__find_map(aslr_thread, effective_cpumode, addr, &al)) {
			pr_debug("Cannot find mmap for address %lx in either space, pid=%d\n",
				 addr, aslr_thread->pid_);
			addr_location__exit(&al);
			return 0; /* No mmap. */
		}
	}

	key.pid = effective_cpumode == PERF_RECORD_MISC_KERNEL ? kernel_pid : aslr_thread->pid_;
	key.start_addr = map__start(al.map);
	if (!hashmap__find(&aslr->remap_addresses, &key, &remap_addr_ptr)) {
		pr_debug("Cannot find a remapped entry for address %lx in mapping %lx(%lx) for pid=%d\n",
			 addr, map__start(al.map), map__size(al.map), key.pid);
		addr_location__exit(&al);
		return 0;
	}
	remap_addr = *remap_addr_ptr + (addr - map__start(al.map));
	addr_location__exit(&al);
	return remap_addr;
}

static u64 aslr_tool__remap_mapping(struct aslr_tool *aslr,
				    struct thread *aslr_thread,
				    u8 cpumode,
				    u64 start, u64 len)
{
	struct addr_location prev_al;
	struct remap_addresses_key key;
	u64 remap_addr = 0;
	u64 *remap_addr_ptr = NULL;
	u64 *max_addr_ptr = NULL;
	/* If mapping is contiguous to the previous process mapping. */
	bool is_contiguous = false;
	bool first_mapping = false; /* first process mapping. */

	if (!aslr_thread)
		return 0; /* No thread. */

	addr_location__init(&prev_al);
	if (thread__find_map(aslr_thread, cpumode, start-1, &prev_al)) {
		if (map__start(prev_al.map) + map__size(prev_al.map) == start) {
			is_contiguous = true;
		} else  {
			pr_debug("Previous mmap [%lx, %lx] overlaps current map [%lx, %lx]\n",
				 map__start(prev_al.map),
				 map__start(prev_al.map) + map__size(prev_al.map),
				 start, start+len);
		}
	}
	addr_location__exit(&prev_al);

	key.pid = cpumode == PERF_RECORD_MISC_KERNEL ? kernel_pid : aslr_thread->pid_;
	key.start_addr = start;
	if (hashmap__find(&aslr->remap_addresses, &key, &remap_addr_ptr))
		return *remap_addr_ptr;

	if (!hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr)) {
		/* First mapping in this process. Don't add a page gap. */
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

		remap_addr_ptr = malloc(sizeof(u64));

		if (!new_key || !remap_addr_ptr) {
			free(new_key);
			free(remap_addr_ptr);
			return 0;
		}
		*new_key = key;
		*remap_addr_ptr = remap_addr;
		if (hashmap__add(&aslr->remap_addresses, new_key, remap_addr_ptr) != 0) {
			pr_debug("Failed to add remap_addresses entry for pid=%d, mapping start=%lx, remapped start=%lx",
					key.pid, start, remap_addr);
			free(new_key);
			free(remap_addr_ptr);
			return 0;
		}
	}

	max_addr_ptr = malloc(sizeof(u64));

	if (!max_addr_ptr)
		return 0;
	*max_addr_ptr = remap_addr + len;
	hashmap__insert(&aslr->top_addresses, key.pid, max_addr_ptr,
				first_mapping ? HASHMAP_ADD : HASHMAP_UPDATE, NULL, NULL);
	return remap_addr;
}

static u64 aslr_tool__remap_ksymbol(struct aslr_tool *aslr,
				    struct thread *aslr_thread,
				    u64 addr, u32 len)
{
	struct remap_addresses_key key;
	u64 remap_addr = 0;
	u64 *max_addr_ptr = NULL;
	u64 *remap_addr_ptr = NULL;
	bool first_mapping = false;

	if (!aslr_thread)
		return 0; /* No thread. */

	key.pid = aslr_thread->pid_;
	key.start_addr = addr;
	if (hashmap__find(&aslr->remap_addresses, &key, &remap_addr_ptr))
		return *remap_addr_ptr;

	first_mapping = !hashmap__find(&aslr->top_addresses, key.pid, &max_addr_ptr);
	if (first_mapping)
		remap_addr = kernel_space_start;
	else
		remap_addr = *max_addr_ptr;
	remap_addr = round_up_to_page_size(remap_addr) + page_size;

	{
		struct remap_addresses_key *new_key = malloc(sizeof(*new_key));

		remap_addr_ptr = malloc(sizeof(u64));

		if (!new_key || !remap_addr_ptr) {
			free(new_key);
			free(remap_addr_ptr);
			return 0;
		}
		*new_key = key;
		*remap_addr_ptr = remap_addr;
		if (hashmap__add(&aslr->remap_addresses, new_key, remap_addr_ptr) < 0) {
			pr_debug("Failed to add remap_addresses entry for pid=%d, ksymbol=%lx, remapped address=%lx",
					key.pid, addr, remap_addr);
			free(new_key);
			free(remap_addr_ptr);
			return 0;
		}
	}

	max_addr_ptr = malloc(sizeof(u64));

	if (!max_addr_ptr)
		return 0;
	*max_addr_ptr = remap_addr + len;
	hashmap__insert(&aslr->top_addresses, key.pid, max_addr_ptr,
			first_mapping ? HASHMAP_ADD : HASHMAP_UPDATE, NULL, NULL);
	return remap_addr;
}


static int aslr_tool__process_mmap(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	union perf_event *new_event = (union perf_event *)aslr->event_copy;
	u8 cpumode   = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_mmap(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	thread = machine__findnew_thread(&aslr->machine, event->mmap.pid, event->mmap.tid);
	memcpy(&new_event->mmap, &event->mmap, event->mmap.header.size);
	/* Remaps the mmap.start. */
	new_event->mmap.start = aslr_tool__remap_mapping(aslr, thread, cpumode,
							   event->mmap.start, event->mmap.len);
	if (aslr->first_kernel_mapping && cpumode == PERF_RECORD_MISC_KERNEL) {
		/*
		 * If this is the first kernel image, we need to adjust the pgoff by a
		 * similar delta.
		 */
		new_event->mmap.pgoff = event->mmap.pgoff - event->mmap.start +
					new_event->mmap.start;
		aslr->first_kernel_mapping = false;
	}
	err = delegate->mmap(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static int aslr_tool__process_mmap2(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	union perf_event *new_event = (union perf_event *)aslr->event_copy;
	u8 cpumode   = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_mmap2(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	thread = machine__findnew_thread(&aslr->machine, event->mmap2.pid, event->mmap2.tid);
	memcpy(&new_event->mmap2, &event->mmap2, event->mmap2.header.size);
	/* Remaps the mmap.start. */
	new_event->mmap2.start = aslr_tool__remap_mapping(aslr, thread, cpumode,
							   event->mmap2.start, event->mmap2.len);
	if (aslr->first_kernel_mapping && cpumode == PERF_RECORD_MISC_KERNEL) {
		/*
		 * If this is the first kernel image, we need to adjust the pgoff by a
		 * similar delta.
		 */
		new_event->mmap2.pgoff = event->mmap2.pgoff - event->mmap2.start +
					 new_event->mmap2.start;
		aslr->first_kernel_mapping = false;
	}
	err = delegate->mmap2(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static int aslr_tool__process_comm(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_comm(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	return delegate->comm(delegate, event, sample, machine);
}

static int aslr_tool__process_fork(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_fork(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	return delegate->fork(delegate, event, sample, machine);
}

static int aslr_tool__process_exit(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_exit(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	return delegate->exit(delegate, event, sample, machine);
}

static int aslr_tool__process_text_poke(const struct perf_tool *tool,
					union perf_event *event,
					struct perf_sample *sample,
					struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	union perf_event *new_event = (union perf_event *)aslr->event_copy;
	u8 cpumode   = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread;
	int err;

	thread = machine__findnew_thread(&aslr->machine, sample->pid, sample->tid);
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
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	union perf_event *new_event = (union perf_event *)aslr->event_copy;
	struct thread *thread;
	int err;

	/* Create the thread, map, etc. in the ASLR before virtual address space. */
	err = perf_event__process_ksymbol(tool, event, sample, &aslr->machine);
	if (err)
		return err;

	thread = machine__findnew_thread(&aslr->machine, kernel_pid, 0);
	memcpy(&new_event->ksymbol, &event->ksymbol, event->ksymbol.header.size);
	/* Remaps the ksymbol.start */
	new_event->ksymbol.addr = aslr_tool__remap_ksymbol(aslr, thread,
							   event->ksymbol.addr, event->ksymbol.len);

	err = delegate->ksymbol(delegate, new_event, sample, machine);
	thread__put(thread);
	return err;
}

static inline int copy_u64(__u64 *in_array, __u64 *out_array,
			   size_t *i, size_t *j, const __u64 max_i)
{
	if (*i >= max_i - 1)
		return -EFAULT;
	out_array[(*j)++] = in_array[(*i)++];
	return 0;
}

static int aslr_tool__process_sample(const struct perf_tool *tool, union perf_event *event,
				struct perf_sample *sample,
				struct evsel *evsel, struct machine *machine)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	int ret;
	u64 sample_type = evsel->core.attr.sample_type;
	struct thread *thread = machine__findnew_thread(&aslr->machine, sample->pid, sample->tid);
	const __u64 max_i = event->header.size / sizeof(__u64);
	union perf_event *new_event = (union perf_event *)aslr->event_copy;
	struct perf_sample new_sample;
	__u64 *in_array, *out_array;
	u8 cpumode = sample->cpumode;
	u64 addr;
	size_t i = 0, j = 0;

	new_event->sample.header = event->sample.header;

	in_array = &event->sample.array[0];
	out_array = &new_event->sample.array[0];



	if (sample_type & PERF_SAMPLE_IDENTIFIER)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* id */
	if (sample_type & PERF_SAMPLE_IP) {
		i++;
		out_array[j++] = aslr_tool__remap_address(aslr, thread, cpumode,
							  sample->ip);
	}
	if (sample_type & PERF_SAMPLE_TID)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* pid, tid */
	if (sample_type & PERF_SAMPLE_TIME)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* time */
	if (sample_type & PERF_SAMPLE_ADDR) {
		i++; /* addr */
		out_array[j++] = aslr_tool__remap_address(aslr, thread, cpumode,
							  sample->addr);
	}
	if (sample_type & PERF_SAMPLE_ID)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* id */
	if (sample_type & PERF_SAMPLE_STREAM_ID)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* stream_id */
	if (sample_type & PERF_SAMPLE_CPU)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* cpu, res */
	if (sample_type & PERF_SAMPLE_PERIOD)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* period */
	if (sample_type & PERF_SAMPLE_READ) {
		if ((evsel->core.attr.read_format & PERF_FORMAT_GROUP) == 0) {
			if (copy_u64(in_array, out_array, &i, &j, max_i))
				return -EFAULT; /* value */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* time_enabled */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* time_running */
			if (evsel->core.attr.read_format & PERF_FORMAT_ID)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* id */
			if (evsel->core.attr.read_format & PERF_FORMAT_LOST)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* lost */
		} else {
			u64 nr;

			if (i > max_i)
				return -EFAULT;
			nr = out_array[j++] = in_array[i++];
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* time_enabled */
			if (evsel->core.attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* time_running */
			for (u64 cntr = 0; cntr < nr; cntr++) {
				if (copy_u64(in_array, out_array, &i, &j, max_i))
					return -EFAULT; /* value */
				if (evsel->core.attr.read_format & PERF_FORMAT_ID)
					if (copy_u64(in_array, out_array, &i, &j, max_i))
						return -EFAULT; /* id */
				if (evsel->core.attr.read_format & PERF_FORMAT_LOST)
					if (copy_u64(in_array, out_array, &i, &j, max_i))
						return -EFAULT; /* lost */
			}
		}
	}
	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* nr */

		for (u64 cntr = 0; cntr < sample->callchain->nr; cntr++) {
			if (i > max_i)
				return -EFAULT;
			i++;
			addr = sample->callchain->ips[cntr];
			if (addr >= PERF_CONTEXT_MAX) {
				/* Copy context values as is. */
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
				case PERF_CONTEXT_GUEST_KERNEL:
				case PERF_CONTEXT_GUEST_USER:
				case PERF_CONTEXT_USER_DEFERRED:
					break;
				default:
					pr_debug("invalid callchain context: %"PRIx64"\n", addr);
					/*
					 * It seems the callchain is corrupted.
					 * Discard sample.
					 */
					return 0;
				}
				continue;
			}
			out_array[j++] = aslr_tool__remap_address(aslr, thread, cpumode,
								  sample->callchain->ips[cntr]);
		}
	}
	if (sample_type & PERF_SAMPLE_RAW) {
		size_t bytes = sizeof(u32) + sample->raw_size;

		size_t u64_words = (bytes + 7) / 8;

		if ((i + u64_words) > max_i)
			return -EFAULT;
		memcpy(&out_array[j], &in_array[i], bytes);
		i += u64_words;
		j += u64_words;
		/*
		 * TODO: certain raw samples can be remapped, such as
		 * tracepoints by examining their fields.
		 */
		pr_debug("Dropping raw samples as possible ASLR leak\n");
		return 0;
	}
	if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* nr */
		if (evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_HW_INDEX)
			if (copy_u64(in_array, out_array, &i, &j, max_i))
				return -EFAULT; /* hw_idx */
		if (sample->branch_stack->nr > (ULLONG_MAX / 3))
			return -EFAULT;
		if (i + (sample->branch_stack->nr * 3) > max_i)
			return -EFAULT;
		for (u64 cntr = 0; cntr < sample->branch_stack->nr; cntr++) {
			out_array[j++] = aslr_tool__remap_address(aslr, thread, sample->cpumode,
								  in_array[i++]); /* from */
			out_array[j++] = aslr_tool__remap_address(aslr, thread, sample->cpumode,
								  in_array[i++]); /* to */
			if (copy_u64(in_array, out_array, &i, &j, max_i))
				return -EFAULT; /* flags */
		}
		if (evsel->core.attr.branch_sample_type & PERF_SAMPLE_BRANCH_COUNTERS) {
			if (i + sample->branch_stack->nr > max_i)
				return -EFAULT;
			memcpy(&out_array[j], &in_array[i], sample->branch_stack->nr * sizeof(u64));
			i += sample->branch_stack->nr;
			j += sample->branch_stack->nr;
			/* TODO: confirm branch counters don't leak ASLR information. */
			pr_debug("Dropping sample branch counters as possible ASLR leak\n");
			return 0;
		}
	}
	if (sample_type & PERF_SAMPLE_REGS_USER) {
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* abi */
		if (sample->user_regs->abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_user);

			if (i + nr > max_i)
				return -EFAULT;
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping regs user sample as possible ASLR leak\n");
		return 0;
	}
	if (sample_type & PERF_SAMPLE_STACK_USER) {
		u64 size;

		if (i > max_i)
			return -EFAULT;
		size = out_array[j++] = in_array[i++];
		if (size > 0) {
			memcpy(&out_array[j], &in_array[i], size);
			i += size / sizeof(u64);
			j += size / sizeof(u64);
			if (copy_u64(in_array, out_array, &i, &j, max_i))
				return -EFAULT; /* dyn_size */
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping stack user sample as possible ASLR leak\n");
		return 0;
	}
	if (sample_type & PERF_SAMPLE_WEIGHT_TYPE)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* perf_sample_weight */
	if (sample_type & PERF_SAMPLE_DATA_SRC)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* data_src */
	if (sample_type & PERF_SAMPLE_TRANSACTION)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* transaction */
	if (sample_type & PERF_SAMPLE_REGS_INTR) {
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* abi */
		if (sample->intr_regs->abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_intr);

			if (i + nr > max_i)
				return -EFAULT;
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
		}
		/* TODO: can this be less conservative? */
		pr_debug("Dropping interrupt register sample as possible ASLR leak\n");
		return 0;
	}
	if (sample_type & PERF_SAMPLE_PHYS_ADDR) {
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* phys_addr */
		/* TODO: can this be less conservative? */
		pr_debug("Dropping physical address sample as possible ASLR leak\n");
		return 0;
	}
	if (sample_type & PERF_SAMPLE_CGROUP)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* cgroup */
	if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* data_page_size */
	if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE)
		if (copy_u64(in_array, out_array, &i, &j, max_i))
			return -EFAULT; /* code_page_size */

	if (sample_type & PERF_SAMPLE_AUX) {
		u64 size;

		if (i > max_i)
			return -EFAULT;
		size = out_array[j++] = in_array[i++];
		if (i + (size / sizeof(u64)) > max_i)
			return -EFAULT;
		memcpy(&out_array[j], &in_array[i], size);
		i += size / sizeof(u64);
		j += size / sizeof(u64);
		/* TODO: can this be less conservative? */
		pr_debug("Dropping aux sample as possible ASLR leak\n");
		return 0;
	}

	if (evsel__is_offcpu_event(evsel)) {
		/* TODO: can this be less conservative? */
		pr_debug("Dropping off-CPU sample as possible ASLR leak\n");
		return 0;
	}

	perf_sample__init(&new_sample, /*all=*/ true);
	ret = evsel__parse_sample(evsel, new_event, &new_sample);
	if (ret)
		return ret;

	ret = delegate->sample(delegate, new_event, &new_sample, evsel, machine);
	perf_sample__exit(&new_sample);
	thread__put(thread);
	return ret;
}

static int aslr_tool__process_attr(const struct perf_tool *tool,
				   union perf_event *event,
				   struct evlist **pevlist)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct perf_tool *delegate = aslr->tool.delegate;
	union perf_event *new_event = (union perf_event *)aslr->event_copy;

	memcpy(&new_event->attr, &event->attr, event->attr.header.size);
	if (new_event->attr.attr.type == PERF_TYPE_BREAKPOINT)
		new_event->attr.attr.bp_addr = 0;  /* Conservatively remove addresses. */
	if (new_event->attr.attr.kprobe_addr >= 0xffff800000000000)
		new_event->attr.attr.kprobe_addr = 0;  /* Conservatively remove addresses. */

	return delegate->attr(delegate, new_event, pevlist);
}

static void aslr_tool__init(struct aslr_tool *aslr, struct perf_tool *delegate)
{
	delegate_tool__init(&aslr->tool, delegate);
	aslr->tool.tool.ordered_events = true;

	machine__init(&aslr->machine, "", HOST_KERNEL_ID);

	hashmap__init(&aslr->remap_addresses,
		      remap_addresses__hash, remap_addresses__equal,
		      /*ctx=*/NULL);
	hashmap__init(&aslr->top_addresses,
		      top_addresses__hash, top_addresses__equal,
		      /*ctx=*/NULL);
	aslr->first_kernel_mapping = true;

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
	/* auxtrace_info, auxtrace_error, time_conv, thread_map, cpu_map, */
	/* stat_config, stat, feature, finished_init, bpf_metadata, compressed, */
	/* auxtrace - no virtual addresses. */
}

struct perf_tool *aslr_tool__new(struct perf_tool *delegate)
{
	struct aslr_tool *aslr = malloc(sizeof(*aslr));

	if (!aslr)
		return NULL;

	aslr_tool__init(aslr, delegate);
	return &aslr->tool.tool;
}

void aslr_tool__delete(struct perf_tool *tool)
{
	struct delegate_tool *del_tool = container_of(tool, struct delegate_tool, tool);
	struct aslr_tool *aslr = container_of(del_tool, struct aslr_tool, tool);
	struct hashmap_entry *cur;
	size_t bkt;

	hashmap__for_each_entry(&aslr->remap_addresses, cur, bkt) {
		zfree(&cur->pkey);
		zfree(&cur->pvalue);
	}
	hashmap__for_each_entry(&aslr->top_addresses, cur, bkt) {
		zfree(&cur->pvalue);
	}

	hashmap__clear(&aslr->remap_addresses);
	hashmap__clear(&aslr->top_addresses);
	machine__exit(&aslr->machine);
	free(aslr);
}
