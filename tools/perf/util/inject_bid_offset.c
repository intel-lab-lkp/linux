// SPDX-License-Identifier: GPL-2.0
#include "inject_bid_offset.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/zalloc.h>

#include "addr_location.h"
#include "debug.h"
#include "dso.h"
#include "event.h"
#include "evlist.h"
#include "evsel.h"
#include "machine.h"
#include "map.h"
#include "session.h"
#include "synthetic-events.h"
#include "thread.h"
#include "tool.h"

struct inject_bid_offset_tool {
	struct delegate_tool tool;
	char event_copy[PERF_SAMPLE_MAX_SIZE] __aligned(8);
};

int perf_event__rewrite_attr_for_build_id_offset(struct perf_event_attr *attr)
{
	if (attr->sample_type & (PERF_SAMPLE_BUILD_ID_OFFSET |
				 PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET)) {
		/*
		 * Expect to add build ID information from virtual address, if
		 * it is already present then things would be confused so fail.
		 */
		return -1;
	}
	if (attr->sample_type & PERF_SAMPLE_IP) {
		attr->sample_type &= ~PERF_SAMPLE_IP;
		attr->sample_type |= PERF_SAMPLE_BUILD_ID_OFFSET;
	}
	if (attr->sample_type & PERF_SAMPLE_CALLCHAIN) {
		attr->sample_type &= ~PERF_SAMPLE_CALLCHAIN;
		attr->sample_type |= PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET;
	}
	return 0;
}

int perf_event__rewrite_attr_for_sample_ip(struct perf_event_attr *attr)
{
	if (attr->sample_type & (PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN)) {
		/*
		 * Expect to remove build ID information for virtual address, if
		 * it is already present then things would be confused so fail.
		 */
		return -1;
	}
	if (attr->sample_type & PERF_SAMPLE_BUILD_ID_OFFSET) {
		attr->sample_type &= ~PERF_SAMPLE_BUILD_ID_OFFSET;
		attr->sample_type |= PERF_SAMPLE_IP;
	}
	if (attr->sample_type & PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET) {
		attr->sample_type &= ~PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET;
		attr->sample_type |= PERF_SAMPLE_CALLCHAIN;
	}
	return 0;
}

static void perf_event__inject_sample_buildid_array(struct thread *thread,
						    u64 ip, u8 cpumode,
						    __u64 *array)
{
	struct perf_build_id bid = { .size = 0 };
	u64 offset = ip;
	struct addr_location al;
	struct dso *dso;
	const struct build_id *dso_bid;

	struct perf_sample ps = { .ip = ip, .cpumode = cpumode };

	addr_location__init(&al);

	if (!thread)
		goto write_bid;

	if (!thread__find_map(thread, &ps, &al))
		goto write_bid;

	dso = al.map ? dso__get(map__dso(al.map)) : NULL;
	if (!dso)
		goto write_bid;
	dso_bid = dso__bid(dso);
	if (!dso_bid || dso_bid->size == 0 ||
	    map__mapping_type(al.map) != MAPPING_TYPE__DSO) {
		dso__put(dso);
		goto write_bid;
	}

	bid.size = dso_bid->size;
	if (bid.size > sizeof(bid.data))
		bid.size = sizeof(bid.data);
	memcpy(bid.data, &dso_bid->data, bid.size);
	offset = map__dso_map_ip(al.map, offset);
	dso__put(dso);

write_bid:
	compiletime_assert(sizeof(struct perf_build_id) == 3 * sizeof(u64),
			   "Unexpected perf_build_id size");
	memcpy(&array[0], &bid, 3 * sizeof(u64));
	array[3] = offset;
	addr_location__exit(&al);
}

static int inject_bid_offset_tool__process_build_id(const struct perf_tool *tool,
						    union perf_event *event,
						    struct perf_sample *sample __maybe_unused,
						    struct machine *machine __maybe_unused)
{
	return tool->build_id(tool, NULL, event);
}

static void mark_dso_hit(const struct perf_tool *tool,
			 struct perf_sample *sample, struct machine *machine,
			 struct thread *thread, u64 ip, u8 cpumode)
{
	struct addr_location al;
	struct dso *dso;

	struct perf_sample ps = { .ip = ip, .cpumode = cpumode };

	addr_location__init(&al);

	if (thread__find_map(thread, &ps, &al)) {
		dso = al.map ? dso__get(map__dso(al.map)) : NULL;
		if (dso) {
			if (!dso__hit(dso)) {
				const struct build_id *bid = dso__bid(dso);

				dso__set_hit(dso);
				if (bid && bid->size > 0) {
					perf_event__synthesize_build_id(
						tool, sample, machine,
						inject_bid_offset_tool__process_build_id,
						dso__kernel(dso) ?
							PERF_RECORD_MISC_KERNEL :
							PERF_RECORD_MISC_USER,
						bid, dso->long_name);
				}
			}
			dso__put(dso);
		}
	}
	addr_location__exit(&al);
}

static int inject_bid_offset_tool__sample(const struct perf_tool *tool,
					  union perf_event *event,
					  struct perf_sample *sample,
					  struct machine *machine)
{
	struct delegate_tool *dt =
		container_of(tool, struct delegate_tool, tool);
	struct inject_bid_offset_tool *ibo =
		container_of(dt, struct inject_bid_offset_tool, tool);
	union perf_event *ev;
	struct perf_sample new_sample;
	struct evsel *evsel = sample->evsel;
	__u64 i = 0, j = 0;
	__u64 *in_array, *out_array;
	__u64 sample_type = evsel->core.attr.sample_type;
	const __u64 max_i = (event->header.size - sizeof(event->header)) / sizeof(__u64);
	struct thread *thread;
	size_t max_size = event->header.size;
	int orig_sample_size, ret;

	if ((sample_type & (PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN)) == 0)
		return ibo->tool.delegate->sample(ibo->tool.delegate, event,
						  sample, machine);

	if (symbol_conf.guest_code && !machine__is_host(machine))
		thread = machine__findnew_guest_code(machine, sample->pid);
	else
		thread = machine__findnew_thread(machine, sample->pid,
						 sample->tid);

	if (sample_type & PERF_SAMPLE_IP)
		max_size += sizeof(struct perf_build_id) + sizeof(u64) -
			    sizeof(u64);

	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		max_size +=
			sample->callchain->nr * (sizeof(struct perf_build_id) +
						 sizeof(u64) - sizeof(u64));
	}

	if (max_size >= PERF_SAMPLE_MAX_SIZE) {
		pr_debug("Insufficient space to copy event\n");
		thread__put(thread);
		return -E2BIG;
	}

	ev = (union perf_event *)ibo->event_copy;
	ev->sample.header =
		(struct perf_event_header){ .type = event->header.type,
					    .misc = event->header.misc,
					    .size = max_size };

	in_array = &event->sample.array[0];
	out_array = &ev->sample.array[0];

	if (sample_type & PERF_SAMPLE_IDENTIFIER) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_IP) {
		if (i >= max_i)
			goto err;
		i++;
		if (evsel && thread)
			mark_dso_hit(ibo->tool.delegate, sample, machine,
				     thread, sample->ip, sample->cpumode);
	}
	if (sample_type & PERF_SAMPLE_TID) {
		union {
			u64 val64;
			u32 val32[2];
		} u;

		if (i >= max_i)
			goto err;
		u.val32[0] = sample->pid;
		u.val32[1] = sample->tid;
		out_array[j++] = u.val64;
		i++;
	}
	if (sample_type & PERF_SAMPLE_TIME) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_ADDR) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_ID) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_STREAM_ID) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_CPU) {
		union {
			u64 val64;
			u32 val32[2];
		} u;

		if (i >= max_i)
			goto err;
		u.val32[0] = sample->cpu;
		u.val32[1] = 0;
		out_array[j++] = u.val64;
		i++;
	}
	if (sample_type & PERF_SAMPLE_PERIOD) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_READ) {
		if ((evsel->core.attr.read_format & PERF_FORMAT_GROUP) == 0) {
			if (i >= max_i)
				goto err;
			out_array[j++] = in_array[i++];
			if (evsel->core.attr.read_format &
			    PERF_FORMAT_TOTAL_TIME_ENABLED) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
			if (evsel->core.attr.read_format &
			    PERF_FORMAT_TOTAL_TIME_RUNNING) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
			if (evsel->core.attr.read_format & PERF_FORMAT_ID) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
			if (evsel->core.attr.read_format & PERF_FORMAT_LOST) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
		} else {
			u64 nr;

			if (i >= max_i)
				goto err;
			nr = out_array[j++] = in_array[i++];
			if (evsel->core.attr.read_format &
			    PERF_FORMAT_TOTAL_TIME_ENABLED) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
			if (evsel->core.attr.read_format &
			    PERF_FORMAT_TOTAL_TIME_RUNNING) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
			}
			for (u64 cntr = 0; cntr < nr; cntr++) {
				if (i >= max_i)
					goto err;
				out_array[j++] = in_array[i++];
				if (evsel->core.attr.read_format &
				    PERF_FORMAT_ID) {
					if (i >= max_i)
						goto err;
					out_array[j++] = in_array[i++];
				}
				if (evsel->core.attr.read_format &
				    PERF_FORMAT_LOST) {
					if (i >= max_i)
						goto err;
					out_array[j++] = in_array[i++];
				}
			}
		}
	}
	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		if (i >= max_i || sample->callchain->nr > max_i - (i + 1))
			goto err;
		i++;
		if (evsel && thread) {
			u8 cpumode = sample->cpumode;

			for (u64 x = 0; x < sample->callchain->nr; x++) {
				u64 ip = sample->callchain->ips[x];

				if (ip >= PERF_CONTEXT_MAX) {
					switch (ip) {
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
						cpumode = PERF_RECORD_MISC_GUEST_KERNEL;
						break;
					case PERF_CONTEXT_GUEST_USER:
						cpumode = PERF_RECORD_MISC_GUEST_USER;
						break;
					case PERF_CONTEXT_USER_DEFERRED:
						cpumode = PERF_RECORD_MISC_USER;
						x++;
						break;
					default:
						break;
					}
					continue;
				}
				mark_dso_hit(ibo->tool.delegate, sample,
					     machine, thread, ip, cpumode);
			}
		}
		i += sample->callchain->nr;
	}
	if (sample_type & PERF_SAMPLE_RAW) {
		size_t bytes = sizeof(u32) + sample->raw_size;
		u64 words = DIV_ROUND_UP(bytes, sizeof(u64));
		u32 *out_raw = (u32 *)&out_array[j];

		if (i > max_i || words > max_i - i)
			goto err;
		out_array[j + words - 1] = 0;
		*out_raw = sample->raw_size;
		memcpy(out_raw + 1, sample->raw_data, sample->raw_size);
		i += words;
		j += words;
	}
	if (sample_type & PERF_SAMPLE_BRANCH_STACK) {
		u64 nr = sample->branch_stack->nr;

		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
		if (evsel__has_branch_hw_idx(evsel)) {
			if (i >= max_i)
				goto err;
			out_array[j++] = in_array[i++];
		}
		if (i > max_i || (nr * 3) > max_i - i)
			goto err;
		memcpy(&out_array[j], &in_array[i],
		       nr * 3 * sizeof(u64));
		i += nr * 3;
		j += nr * 3;
		if (sample->branch_stack_cntr) {
			if (i > max_i || nr > max_i - i)
				goto err;
			memcpy(&out_array[j], &in_array[i],
			       nr * sizeof(u64));
			i += nr;
			j += nr;
		}
	}
	if (sample_type & PERF_SAMPLE_REGS_USER) {
		u64 abi;

		if (i >= max_i)
			goto err;
		abi = out_array[j++] = in_array[i++];
		if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_user);

			if (i > max_i || nr > max_i - i)
				goto err;
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
			if (abi & PERF_SAMPLE_REGS_ABI_SIMD) {
				u64 nr_vectors, vector_qwords, nr_pred, pred_qwords, simd_nr;

				if (i > max_i || 4 > max_i - i)
					goto err;
				nr_vectors = out_array[j++] = in_array[i++];
				vector_qwords = out_array[j++] = in_array[i++];
				nr_pred = out_array[j++] = in_array[i++];
				pred_qwords = out_array[j++] = in_array[i++];
				simd_nr = nr_vectors * vector_qwords + nr_pred * pred_qwords;
				if (i > max_i || simd_nr > max_i - i)
					goto err;
				memcpy(&out_array[j], &in_array[i], simd_nr * sizeof(u64));
				i += simd_nr;
				j += simd_nr;
			}
		}
	}
	if (sample_type & PERF_SAMPLE_STACK_USER) {
		u64 size;

		if (i >= max_i)
			goto err;
		size = out_array[j++] = in_array[i++];
		if (size > 0) {
			u64 words = DIV_ROUND_UP(size, sizeof(u64));

			if (i > max_i || words > max_i - i)
				goto err;
			memcpy(&out_array[j], &in_array[i], words * sizeof(u64));
			i += words;
			j += words;
			if (i >= max_i)
				goto err;
			out_array[j++] = in_array[i++];
		}
	}
	if (sample_type & PERF_SAMPLE_WEIGHT_TYPE) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_DATA_SRC) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_TRANSACTION) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_REGS_INTR) {
		u64 abi;

		if (i >= max_i)
			goto err;
		abi = out_array[j++] = in_array[i++];
		if (abi != PERF_SAMPLE_REGS_ABI_NONE) {
			u64 nr = hweight64(evsel->core.attr.sample_regs_intr);

			if (i > max_i || nr > max_i - i)
				goto err;
			memcpy(&out_array[j], &in_array[i], nr * sizeof(u64));
			i += nr;
			j += nr;
			if (abi & PERF_SAMPLE_REGS_ABI_SIMD) {
				u64 nr_vectors, vector_qwords, nr_pred, pred_qwords, simd_nr;

				if (i > max_i || 4 > max_i - i)
					goto err;
				nr_vectors = out_array[j++] = in_array[i++];
				vector_qwords = out_array[j++] = in_array[i++];
				nr_pred = out_array[j++] = in_array[i++];
				pred_qwords = out_array[j++] = in_array[i++];
				simd_nr = nr_vectors * vector_qwords + nr_pred * pred_qwords;
				if (i > max_i || simd_nr > max_i - i)
					goto err;
				memcpy(&out_array[j], &in_array[i], simd_nr * sizeof(u64));
				i += simd_nr;
				j += simd_nr;
			}
		}
	}
	if (sample_type & PERF_SAMPLE_PHYS_ADDR) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_CGROUP) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE) {
		if (i >= max_i)
			goto err;
		out_array[j++] = in_array[i++];
	}
	if (sample_type & PERF_SAMPLE_AUX) {
		u64 size, words;

		if (i >= max_i)
			goto err;
		size = out_array[j++] = in_array[i++];
		words = DIV_ROUND_UP(size, sizeof(u64));
		if (i > max_i || words > max_i - i)
			goto err;
		memcpy(&out_array[j], &in_array[i], words * sizeof(u64));
		i += words;
		j += words;
	}

	if (sample_type & PERF_SAMPLE_IP) {
		perf_event__inject_sample_buildid_array(
			thread, sample->ip, sample->cpumode, &out_array[j]);
		j += 4;
	}
	if (sample_type & PERF_SAMPLE_CALLCHAIN) {
		u8 cpumode = sample->cpumode;

		out_array[j++] = sample->callchain->nr;
		for (u64 x = 0; x < sample->callchain->nr; x++) {
			u64 ip = sample->callchain->ips[x];

			if (ip >= PERF_CONTEXT_MAX) {
				switch (ip) {
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
					cpumode = PERF_RECORD_MISC_GUEST_KERNEL;
					break;
				case PERF_CONTEXT_GUEST_USER:
					cpumode = PERF_RECORD_MISC_GUEST_USER;
					break;
				case PERF_CONTEXT_USER_DEFERRED:
					cpumode = PERF_RECORD_MISC_USER;
					memset(&out_array[j], 0, 3 * sizeof(u64));
					out_array[j + 3] = ip;
					j += 4;
					if (x + 1 < sample->callchain->nr) {
						x++;
						memset(&out_array[j], 0, 3 * sizeof(u64));
						out_array[j + 3] = sample->callchain->ips[x];
						j += 4;
					}
					continue;
				default:
					break;
				}
				memset(&out_array[j], 0, 3 * sizeof(u64));
				out_array[j + 3] = ip;
			} else {
				perf_event__inject_sample_buildid_array(
					thread, ip, cpumode, &out_array[j]);
			}
			j += 4;
		}
	}

	max_size = sizeof(event->header) + j * sizeof(__u64);
	if (max_size >= PERF_SAMPLE_MAX_SIZE)
		goto err;
	ev->sample.header.size = max_size;

	orig_sample_size = evsel->sample_size;
	if (perf_event__rewrite_attr_for_build_id_offset(&evsel->core.attr))
		goto err;
	evsel->sample_size = __evsel__sample_size(evsel->core.attr.sample_type);
	perf_sample__init(&new_sample, /*all=*/true);
	ret = __evsel__parse_sample(evsel, ev, &new_sample, /*needs_swap=*/false);
	if (!ret) {
		new_sample.evsel = evsel;
		ret = ibo->tool.delegate->sample(ibo->tool.delegate, ev,
						 &new_sample, machine);
	}
	perf_sample__exit(&new_sample);
	evsel->core.attr.sample_type = sample_type;
	evsel->sample_size = orig_sample_size;
	thread__put(thread);
	return ret;

err:
	thread__put(thread);
	return -EFAULT;
}

static int
inject_bid_offset_tool__mmap(const struct perf_tool *tool __maybe_unused,
			     union perf_event *event,
			     struct perf_sample *sample,
			     struct machine *machine)
{
	perf_event__process_mmap(tool, event, sample, machine);
	return 0; // Drop mmap events from output stream
}
static int
inject_bid_offset_tool__mmap2(const struct perf_tool *tool __maybe_unused,
			      union perf_event *event,
			      struct perf_sample *sample,
			      struct machine *machine)
{
	perf_event__process_mmap2(tool, event, sample, machine);
	return 0; // Drop mmap2 events from output stream
}

struct perf_tool *inject_bid_offset_tool__new(struct perf_tool *delegate)
{
	struct inject_bid_offset_tool *ibo = zalloc(sizeof(*ibo));

	if (!ibo)
		return NULL;

	delegate_tool__init(&ibo->tool, delegate);
	ibo->tool.tool.sample = inject_bid_offset_tool__sample;
	ibo->tool.tool.mmap = inject_bid_offset_tool__mmap;
	ibo->tool.tool.mmap2 = inject_bid_offset_tool__mmap2;

	return &ibo->tool.tool;
}

void inject_bid_offset_tool__delete(struct perf_tool *tool)
{
	struct delegate_tool *dt;

	if (!tool)
		return;
	dt = container_of(tool, struct delegate_tool, tool);
	free(container_of(dt, struct inject_bid_offset_tool, tool));
}
