// SPDX-License-Identifier: GPL-2.0
#include "build_id_tool.h"

#include <linux/ctype.h>
#include <linux/mman.h>
#include <linux/zalloc.h>

#include "addr_location.h"
#include "build-id.h"
#include "debug.h"
#include "dso.h"
#include "evlist.h"
#include "machine.h"
#include "map.h"
#include "namespaces.h"
#include "session.h"
#include "string2.h"
#include "strlist.h"
#include "symbol.h"
#include "synthetic-events.h"
#include "thread.h"
#include "vdso.h"

static int build_id_tool__repipe(const struct perf_tool *tool,
				 union perf_event *event,
				 struct perf_sample *sample,
				 struct machine *machine)
{
	struct build_id_tool *bit = container_of(tool, struct build_id_tool, dtool.tool);
	struct perf_tool *delegate = bit->dtool.delegate;

	if (event->header.type == PERF_RECORD_MMAP2)
		return delegate->mmap2(delegate, event, sample, machine);

	return delegate->mmap(delegate, event, sample, machine);
}

static int dso__read_build_id(struct dso *dso)
{
	struct nscookie nsc;
	struct build_id bid = { .size = 0, };

	if (dso__has_build_id(dso))
		return 0;

	mutex_lock(dso__lock(dso));
	nsinfo__mountns_enter(dso__nsinfo(dso), &nsc);
	if (filename__read_build_id(dso__long_name(dso), &bid) > 0)
		dso__set_build_id(dso, &bid);
	else if (dso__nsinfo(dso)) {
		char *new_name = dso__filename_with_chroot(dso, dso__long_name(dso));

		if (new_name && filename__read_build_id(new_name, &bid) > 0)
			dso__set_build_id(dso, &bid);
		free(new_name);
	}
	nsinfo__mountns_exit(&nsc);
	mutex_unlock(dso__lock(dso));

	return dso__has_build_id(dso) ? 0 : -1;
}

static struct dso *findnew_dso(int pid, int tid, const char *filename,
			       const struct dso_id *id, struct machine *machine)
{
	struct thread *thread;
	struct nsinfo *nsi = NULL;
	struct nsinfo *nnsi;
	struct dso *dso;
	bool vdso;

	thread = machine__findnew_thread(machine, pid, tid);
	if (thread == NULL) {
		pr_err("cannot find or create a task %d/%d.\n", tid, pid);
		return NULL;
	}

	vdso = is_vdso_map(filename);
	nsi = nsinfo__get(thread__nsinfo(thread));

	if (vdso) {
		/* The vdso maps are always on the host and not the
		 * container.  Ensure that we don't use setns to look
		 * them up.
		 */
		nnsi = nsinfo__copy(nsi);
		if (nnsi) {
			nsinfo__put(nsi);
			nsinfo__clear_need_setns(nnsi);
			nsi = nnsi;
		}
		dso = machine__findnew_vdso(machine, thread);
	} else {
		dso = machine__findnew_dso_id(machine, filename, id);
	}

	if (dso) {
		mutex_lock(dso__lock(dso));
		dso__set_nsinfo(dso, nsi);
		mutex_unlock(dso__lock(dso));
	} else
		nsinfo__put(nsi);

	thread__put(thread);
	return dso;
}

static struct strlist *build_id_tool__parse_known_build_ids(const char *known_build_ids_string)
{
	struct str_node *pos, *tmp;
	struct strlist *known_build_ids;
	int bid_len;

	known_build_ids = strlist__new(known_build_ids_string, NULL);
	if (known_build_ids == NULL)
		return NULL;
	strlist__for_each_entry_safe(pos, tmp, known_build_ids) {
		const char *build_id, *dso_name;

		build_id = skip_spaces(pos->s);
		dso_name = strchr(build_id, ' ');
		if (dso_name == NULL) {
			strlist__remove(known_build_ids, pos);
			continue;
		}
		bid_len = dso_name - pos->s;
		dso_name = skip_spaces(dso_name);
		if (bid_len % 2 != 0 || bid_len >= SBUILD_ID_SIZE) {
			strlist__remove(known_build_ids, pos);
			continue;
		}
		for (int ix = 0; 2 * ix + 1 < bid_len; ++ix) {
			if (!isxdigit(build_id[2 * ix]) ||
			    !isxdigit(build_id[2 * ix + 1])) {
				strlist__remove(known_build_ids, pos);
				break;
			}
		}
	}
	return known_build_ids;
}

static bool build_id_tool__lookup_known_build_id(struct build_id_tool *bit,
						 struct dso *dso)
{
	struct str_node *pos;

	strlist__for_each_entry(pos, bit->known_build_ids) {
		struct build_id bid;
		const char *build_id, *dso_name;
		size_t bid_len;

		build_id = skip_spaces(pos->s);
		dso_name = strchr(build_id, ' ');
		bid_len = dso_name - pos->s;
		if (bid_len > sizeof(bid.data))
			bid_len = sizeof(bid.data);
		dso_name = skip_spaces(dso_name);
		if (strcmp(dso__long_name(dso), dso_name))
			continue;
		for (size_t ix = 0; 2 * ix + 1 < bid_len; ++ix) {
			bid.data[ix] = (hex(build_id[2 * ix]) << 4 |
					hex(build_id[2 * ix + 1]));
		}
		bid.size = bid_len / 2;
		dso__set_build_id(dso, &bid);
		return true;
	}
	return false;
}

static int build_id_tool__inject_build_id(const struct perf_tool *tool,
					  struct perf_sample *sample,
					  struct machine *machine,
					  const struct evsel *evsel,
					  __u16 misc,
					  const char *filename,
					  struct dso *dso, u32 flags)
{
	struct build_id_tool *bit = container_of(tool, struct build_id_tool, dtool.tool);
	int err;

	if (is_anon_memory(filename) || flags & MAP_HUGETLB)
		return 0;
	if (is_no_dso_memory(filename))
		return 0;

	if (bit->known_build_ids != NULL &&
	    build_id_tool__lookup_known_build_id(bit, dso))
		return 1;

	if (dso__read_build_id(dso) < 0) {
		pr_debug("no build_id found for %s\n", filename);
		return -1;
	}

	err = perf_event__synthesize_build_id(tool, sample, machine,
					      build_id_tool__repipe,
					      evsel, misc, dso__bid(dso),
					      filename);
	if (err) {
		pr_err("Can't synthesize build_id event for %s\n", filename);
		return -1;
	}

	return 0;
}

static int tool__inject_mmap2_build_id(const struct perf_tool *tool,
				       struct perf_sample *sample,
				       struct machine *machine,
				       const struct evsel *evsel,
				       __u16 misc,
				       __u32 pid, __u32 tid,
				       __u64 start, __u64 len, __u64 pgoff,
				       struct dso *dso,
				       __u32 prot, __u32 flags,
				       const char *filename)
{
	int err;

	/* Return to repipe anonymous maps. */
	if (is_anon_memory(filename) || flags & MAP_HUGETLB)
		return 1;
	if (is_no_dso_memory(filename))
		return 1;

	if (dso__read_build_id(dso)) {
		pr_debug("no build_id found for %s\n", filename);
		return -1;
	}

	err = perf_event__synthesize_mmap2_build_id(tool, sample, machine,
						    build_id_tool__repipe,
						    evsel,
						    misc, pid, tid,
						    start, len, pgoff,
						    dso__bid(dso),
						    prot, flags,
						    filename);
	if (err) {
		pr_err("Can't synthesize build_id event for %s\n", filename);
		return -1;
	}
	return 0;
}

static int mark_dso_hit(struct build_id_tool *bit,
			const struct perf_tool *tool,
			struct perf_sample *sample,
			struct machine *machine,
			const struct evsel *mmap_evsel,
			struct map *map, bool sample_in_dso)
{
	struct dso *dso;
	u16 misc = sample->cpumode;

	if (!map)
		return 0;

	if (!sample_in_dso) {
		u16 guest_mask = PERF_RECORD_MISC_GUEST_KERNEL |
			PERF_RECORD_MISC_GUEST_USER;

		if ((misc & guest_mask) != 0) {
			misc &= PERF_RECORD_MISC_HYPERVISOR;
			misc |= __map__is_kernel(map)
				? PERF_RECORD_MISC_GUEST_KERNEL
				: PERF_RECORD_MISC_GUEST_USER;
		} else {
			misc &= PERF_RECORD_MISC_HYPERVISOR;
			misc |= __map__is_kernel(map)
				? PERF_RECORD_MISC_KERNEL
				: PERF_RECORD_MISC_USER;
		}
	}
	dso = map__dso(map);
	if (bit->style == BID_RWS__INJECT_HEADER_LAZY) {
		if (dso && !dso__hit(dso)) {
			dso__set_hit(dso);
			build_id_tool__inject_build_id(tool, sample, machine,
						       mmap_evsel, misc, dso__long_name(dso), dso,
						       map__flags(map));
		}
	} else if (bit->style == BID_RWS__MMAP2_BUILDID_LAZY) {
		if (!map__hit(map)) {
			const struct build_id null_bid = { .size = 0 };
			const struct build_id *bid = dso ? dso__bid(dso) : &null_bid;
			const char *filename = dso ? dso__long_name(dso) : "";

			map__set_hit(map);
			perf_event__synthesize_mmap2_build_id(tool, sample, machine,
							      build_id_tool__repipe,
							      mmap_evsel,
							      misc,
							      sample->pid, sample->tid,
							      map__start(map),
							      map__end(map) - map__start(map),
							      map__pgoff(map),
							      bid,
							      map__prot(map),
							      map__flags(map),
							      filename);
		}
	}
	return 0;
}

struct mark_dso_hit_args {
	struct build_id_tool *bit;
	const struct perf_tool *tool;
	struct perf_sample *sample;
	struct machine *machine;
	const struct evsel *mmap_evsel;
};

static int mark_dso_hit_callback(struct callchain_cursor_node *node, void *data)
{
	struct mark_dso_hit_args *args = data;
	struct map *map = node->ms.map;

	return mark_dso_hit(args->bit, args->tool, args->sample, args->machine,
			    args->mmap_evsel, map, /*sample_in_dso=*/false);
}

static int build_id_tool__sample(const struct perf_tool *tool, union perf_event *event,
				 struct perf_sample *sample,
				 struct evsel *evsel,
				 struct machine *machine)
{
	struct build_id_tool *bit = container_of(tool, struct build_id_tool, dtool.tool);
	struct addr_location al;
	struct thread *thread;
	struct mark_dso_hit_args args = {
		.bit = bit,
		.tool = tool,
		.sample = sample,
		.machine = machine,
		.mmap_evsel = bit->mmap_evsel,
	};

	addr_location__init(&al);
	thread = machine__findnew_thread(machine, sample->pid, sample->tid);
	if (thread == NULL) {
		pr_err("problem processing %d event, skipping it.\n",
		       event->header.type);
		goto repipe;
	}

	if (thread__find_map(thread, sample->cpumode, sample->ip, &al)) {
		mark_dso_hit(bit, tool, sample, machine, args.mmap_evsel, al.map,
			     /*sample_in_dso=*/true);
	}

	sample__for_each_callchain_node(thread, evsel, sample, PERF_MAX_STACK_DEPTH,
					/*symbols=*/false, mark_dso_hit_callback, &args);

	thread__put(thread);
repipe:
	addr_location__exit(&al);
	return bit->dtool.delegate->sample(bit->dtool.delegate, event, sample, evsel, machine);
}

static int build_id_tool__common_mmap(const struct perf_tool *tool,
				      union perf_event *event,
				      struct perf_sample *sample,
				      struct machine *machine,
				      __u32 pid, __u32 tid,
				      __u64 start, __u64 len, __u64 pgoff,
				      __u32 flags, __u32 prot,
				      const char *filename,
				      const struct dso_id *dso_id)
{
	struct build_id_tool *bit = container_of(tool, struct build_id_tool, dtool.tool);
	struct perf_tool *delegate = bit->dtool.delegate;
	struct dso *dso = NULL;
	bool dso_sought = false;
	int err;

	if (event->header.misc & PERF_RECORD_MISC_MMAP_BUILD_ID) {
		dso = findnew_dso(pid, tid, filename, dso_id, machine);
		dso_sought = true;
		if (dso) {
			/* mark it not to inject build-id */
			dso__set_hit(dso);
		}
	}
	if (bit->style == BID_RWS__INJECT_HEADER_ALL) {
		if (!dso_sought) {
			dso = findnew_dso(pid, tid, filename, dso_id, machine);
			dso_sought = true;
		}

		if (dso && !dso__hit(dso)) {
			struct evsel *evsel = (struct evsel *)bit->mmap_evsel;

			if (evsel) {
				dso__set_hit(dso);
				build_id_tool__inject_build_id(tool, sample, machine, evsel,
							       /*misc=*/sample->cpumode,
							       filename, dso, flags);
			}
		}
	} else {
		/* Create the thread, map, etc. by calling delegate. */
		if (event->header.type == PERF_RECORD_MMAP2)
			err = delegate->mmap2(delegate, event, sample, machine);
		else
			err = delegate->mmap(delegate, event, sample, machine);

		if (err) {
			dso__put(dso);
			return err;
		}
	}
	if ((bit->style == BID_RWS__MMAP2_BUILDID_ALL) &&
	    !(event->header.misc & PERF_RECORD_MISC_MMAP_BUILD_ID)) {
		struct evsel *evsel = (struct evsel *)bit->mmap_evsel;

		if (evsel && !dso_sought) {
			dso = findnew_dso(pid, tid, filename, dso_id, machine);
			dso_sought = true;
		}
		if (evsel && dso &&
		    !tool__inject_mmap2_build_id(tool, sample, machine, evsel,
						 sample->cpumode | PERF_RECORD_MISC_MMAP_BUILD_ID,
						 pid, tid, start, len, pgoff,
						 dso,
						 prot, flags,
						 filename)) {
			/* Injected mmap2 so no need to repipe further. */
			dso__put(dso);
			return 0;
		}
	}
	dso__put(dso);

	if (bit->style == BID_RWS__INJECT_HEADER_ALL ||
	    bit->style == BID_RWS__MMAP2_BUILDID_ALL) {
		if (event->header.type == PERF_RECORD_MMAP2)
			return delegate->mmap2(delegate, event, sample, machine);
		else
			return delegate->mmap(delegate, event, sample, machine);
	}

	return 0;
}

static int build_id_tool__mmap(const struct perf_tool *tool,
			       union perf_event *event,
			       struct perf_sample *sample,
			       struct machine *machine)
{
	return build_id_tool__common_mmap(
		tool, event, sample, machine,
		event->mmap.pid, event->mmap.tid,
		event->mmap.start, event->mmap.len, event->mmap.pgoff,
		/*flags=*/0, PROT_EXEC,
		event->mmap.filename, /*dso_id=*/NULL);
}

static int build_id_tool__mmap2(const struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample,
				struct machine *machine)
{
	struct dso_id id = dso_id_empty;

	if (event->header.misc & PERF_RECORD_MISC_MMAP_BUILD_ID) {
		build_id__init(&id.build_id, event->mmap2.build_id, event->mmap2.build_id_size);
	} else {
		id.maj = event->mmap2.maj;
		id.min = event->mmap2.min;
		id.ino = event->mmap2.ino;
		id.ino_generation = event->mmap2.ino_generation;
		id.mmap2_valid = true;
		id.mmap2_ino_generation_valid = true;
	}

	return build_id_tool__common_mmap(
		tool, event, sample, machine,
		event->mmap2.pid, event->mmap2.tid,
		event->mmap2.start, event->mmap2.len, event->mmap2.pgoff,
		event->mmap2.flags, event->mmap2.prot,
		event->mmap2.filename, &id);
}

static int build_id_tool__finished_init(const struct perf_tool *tool,
					struct perf_session *session,
					union perf_event *event)
{
	struct build_id_tool *bit = container_of(tool, struct build_id_tool, dtool.tool);
	struct evsel *pos;

	evlist__for_each_entry(session->evlist, pos) {
		if (pos->core.attr.mmap) {
			bit->mmap_evsel = pos;
			break;
		}
	}

	return bit->dtool.delegate->finished_init(bit->dtool.delegate, session, event);
}

struct build_id_tool *build_id_tool__new(enum build_id_rewrite_style style,
					 const char *known_build_ids_string,
					 struct perf_tool *delegate)
{
	struct build_id_tool *bit = zalloc(sizeof(*bit));

	if (!bit)
		return NULL;

	delegate_tool__init(&bit->dtool, delegate);
	bit->style = style;
	if (known_build_ids_string) {
		bit->known_build_ids = build_id_tool__parse_known_build_ids(known_build_ids_string);
		if (!bit->known_build_ids) {
			free(bit);
			return NULL;
		}
	}

	if (style == BID_RWS__INJECT_HEADER_LAZY ||
	    style == BID_RWS__MMAP2_BUILDID_LAZY) {
		bit->dtool.tool.sample = build_id_tool__sample;
		/*
		 * to make sure the mmap records are ordered correctly
		 * and so that the correct especially due to jitted code
		 * mmaps. We cannot generate the buildid hit list and
		 * inject the jit mmaps at the same time for now.
		 */
		bit->dtool.tool.ordering_requires_timestamps = true;
	}

	if (style != BID_RWS__NONE) {
		bit->dtool.tool.mmap = build_id_tool__mmap;
		bit->dtool.tool.mmap2 = build_id_tool__mmap2;
		bit->dtool.tool.finished_init = build_id_tool__finished_init;
	}

	return bit;
}

void build_id_tool__delete(struct build_id_tool *bit)
{
	if (bit) {
		strlist__delete(bit->known_build_ids);
		free(bit);
	}
}
