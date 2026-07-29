// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <linux/zalloc.h>
#include "util/evsel.h"
#include "util/evlist.h"
#include "util/session.h"
#include "auxtrace.h"
#include "color.h"
#include "powerpc-htm.h"
#include <errno.h>
#include "debug.h"
#include "sample.h"

#include <linux/unaligned.h>

struct perf_session;

struct powerpc_htm {
	struct auxtrace		auxtrace;
	struct auxtrace_queues	queues;
	struct auxtrace_heap	heap;
	u32			auxtrace_type;
	struct perf_session	*session;
	struct machine		*machine;

	/*
	 * Capacity: number of distinct HTM targets (node/chip/core tuples)
	 * recorded, read from auxtrace_info->priv[POWERPC_HTM_NUM_EVENTS] at
	 * init time.  All three arrays below are allocated to this size.
	 */
	int	nr_targets;

	/*
	 * Per-run first-write tracking for htm.bin.* and translation.* files.
	 * Each entry is a packed u32: node<<16 | chip<<8 | core.
	 * First write for a given key -> O_TRUNC; subsequent writes -> O_APPEND.
	 */
	u32	*htm_bin_targets;
	int	nr_htm_bin_targets;
	u32	*translation_targets;
	int	nr_translation_targets;

	/*
	 * CPU -> attr.config table, populated from auxtrace_info->priv[] at
	 * init time.  htm_info_fill() (record side) stores the CPU and config
	 * for each htm evsel; we read them back here to map each
	 * event->auxtrace.cpu to the correct (node, chip, core) config.
	 */
	struct {
		int	cpu;
		u64	config;
	} *cpu_configs;
	int	nr_cpu_configs;
};

/*
 * Look up attr.config by the CPU number carried in event->auxtrace.cpu.
 * Returns true and sets *config if found, false if not found.
 * Using a bool+output-param avoids the ambiguity of returning 0 as both
 * a sentinel (not found) and a theoretically valid config value.
 */
static bool htm_config_for_cpu(struct powerpc_htm *htm, int cpu, u64 *config)
{
	int i;

	for (i = 0; i < htm->nr_cpu_configs; i++) {
		if (htm->cpu_configs[i].cpu == cpu) {
			*config = htm->cpu_configs[i].config;
			return true;
		}
	}
	return false;
}

static void powerpc_htm_dump_event(u64 len)
{
	const char *color = PERF_COLOR_BLUE;

	if (dump_trace) {
		color_fprintf(stdout, color,
			". ... HTM PMU data: size %" PRIu64 " bytes\n", len);
	}
}

#define HTM_MEM_ENTRY_SIZE 32

static inline u32 htm_pack_target(u32 node, u32 chip, u32 core)
{
	return (node << 16) | (chip << 8) | core;
}

static bool htm_target_seen(u32 *targets, int *nr, int capacity, u32 key)
{
	int i;

	for (i = 0; i < *nr; i++) {
		if (targets[i] == key)
			return true;
	}

	if (*nr < capacity)
		targets[(*nr)++] = key;
	else {
		pr_warning("htm: too many targets (max %d), appending to existing file\n",
			   capacity);
		return true;  /* treat as seen: use O_APPEND not O_TRUNC */
	}

	return false;
}

/*
 * Write HTM data to a file.
 *
 * mem_maps == 0: AUX bus-trace path  -> htm.bin.nX.pX.cX
 * mem_maps != 0: memory config path  -> translation.nX.pX.cX
 *
 * htm_target_seen() decides O_TRUNC (first write this run) vs O_APPEND
 * (subsequent writes), keyed on what this process has already written --
 * not on whether the file exists on disk.
 */
static int write_htm(struct powerpc_htm *htm, void *data, size_t size,
		     u32 node, u32 chip, u32 core, int mem_maps)
{
	u32 target_key = htm_pack_target(node, chip, core);
	char target_file[128];
	ssize_t written;
	int flags;
	int fd;

	if (!data || !size)
		return -EINVAL;

	flags = O_CREAT | O_WRONLY | O_NOFOLLOW | O_CLOEXEC;

	if (mem_maps) {
		uint8_t *byte_ptr = (uint8_t *)data;
		size_t entries;
		size_t payload;

		if (size < HTM_MEM_ENTRY_SIZE) {
			pr_err("Malformed memory mapping entry trace segment\n");
			return -EINVAL;
		}

		/* Entry count is at offset 0x10; add 1 for the 32-byte header */
		entries = get_unaligned_be64(byte_ptr + 0x10) + 1;
		payload = entries * HTM_MEM_ENTRY_SIZE;

		if (payload != size) {
			pr_err("Bad memory mapping data, invalid number of entries\n");
			return -EINVAL;
		}

		snprintf(target_file, sizeof(target_file),
			 "translation.n%d.p%d.c%d", node, chip, core);
		flags |= htm_target_seen(htm->translation_targets,
					 &htm->nr_translation_targets,
					 htm->nr_targets,
					 target_key) ? O_APPEND : O_TRUNC;
		fd = open(target_file, flags, 0644);
		if (fd == -1) {
			pr_err("Failed to open %s: %s\n", target_file, strerror(errno));
			return -errno;
		}

		written = write(fd, data, payload);
		close(fd);

		if ((size_t)written != payload) {
			pr_err("Failed to write memory config: expected %zu bytes, wrote %zd\n",
			       payload, written);
			return -EIO;
		}

		return 0;
	}

	/* AUX bus-trace path */
	snprintf(target_file, sizeof(target_file),
		 "htm.bin.n%d.p%d.c%d", node, chip, core);
	flags |= htm_target_seen(htm->htm_bin_targets,
				 &htm->nr_htm_bin_targets,
				 htm->nr_targets,
				 target_key) ? O_APPEND : O_TRUNC;
	fd = open(target_file, flags, 0644);
	if (fd == -1) {
		pr_err("Failed to open %s: %s\n", target_file, strerror(errno));
		return -errno;
	}

	written = write(fd, data, size);
	close(fd);

	if ((size_t)written != size) {
		pr_err("Failed to write htm trace data: expected %zu bytes, wrote %zd\n",
		       size, written);
		return -EIO;
	}

	return 0;
}

static int powerpc_htm_process_event(struct perf_session *session,
				     union perf_event *event,
				     struct perf_sample *sample,
				     const struct perf_tool *tool __maybe_unused)
{
	struct powerpc_htm *htm;
	struct evsel *evsel;
	u32 node, chip, core;
	u64 ev_config;

	if (!session || !session->auxtrace || !event || !sample)
		return 0;

	if (event->header.type != PERF_RECORD_SAMPLE || !sample->raw_data)
		return 0;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	evsel = evlist__event2evsel(session->evlist, event);

	if (!evsel || strcmp(evsel__pmu_name(evsel), "htm") != 0)
		return 0;

	ev_config = evsel->core.attr.config;
	node = (ev_config >> 4)  & 0xff;
	chip = (ev_config >> 12) & 0xff;
	core = (ev_config >> 20) & 0xff;

	/*
	 * raw_size includes 4 bytes of u64 alignment padding added by the
	 * kernel.  Subtract sizeof(u32) to recover the true payload byte count.
	 * Guard against underflow: if raw_size is too small, skip silently.
	 */
	if (sample->raw_size <= sizeof(uint32_t))
		return 0;
	if (write_htm(htm, sample->raw_data,
		      sample->raw_size - sizeof(uint32_t),
		      node, chip, core, 1) < 0) {
		pr_err("Failed to write memory translation block\n");
		return -EIO;
	}

	return 0;
}

static int powerpc_htm_process_auxtrace_event(struct perf_session *session,
					      union perf_event *event,
					      const struct perf_tool *tool __maybe_unused)
{
	struct powerpc_htm *htm;
	struct auxtrace_buffer *buffer;
	off_t data_offset;
	u32 node, chip, core;
	u64 ev_config;
	int fd;
	int err;

	if (!session || !session->auxtrace)
		return 0;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	fd = perf_data__fd(session->data);

	if (perf_data__is_pipe(session->data)) {
		data_offset = 0;
	} else {
		data_offset = lseek(fd, 0, SEEK_CUR);
		if (data_offset == -1)
			return -errno;
	}

	/*
	 * Queue the buffer and get back a pointer to it.  We immediately load
	 * and write the data so htm.bin.* exists on disk before subsequent
	 * patches invoke htmdecode during the same session pass.
	 */
	err = auxtrace_queues__add_event(&htm->queues, session, event,
					 data_offset, &buffer);
	if (err)
		return err;

	if (!buffer)
		return 0;

	/*
	 * Map event->auxtrace.cpu -> attr.config using the table built from
	 * auxtrace_info->priv[] at init time.  This is reliable because
	 * htm_info_fill() stored the exact (cpu, config) pair for each evsel
	 * at record time -- no CPU map or evlist iteration needed here.
	 */
	if (!htm_config_for_cpu(htm, (int)event->auxtrace.cpu, &ev_config)) {
		pr_err("htm: no config found for auxtrace cpu %u\n",
		       event->auxtrace.cpu);
		return 0;
	}

	node = (ev_config >> 4)  & 0xff;
	chip = (ev_config >> 12) & 0xff;
	core = (ev_config >> 20) & 0xff;

	if (!auxtrace_buffer__get_data(buffer, fd)) {
		pr_err("Failed to read AUX buffer data\n");
		return -ENOMEM;
	}

	if (dump_trace)
		powerpc_htm_dump_event(buffer->size);

	err = write_htm(htm, buffer->data, buffer->size, node, chip, core, 0);
	auxtrace_buffer__put_data(buffer);

	return err < 0 ? err : 0;
}

static int powerpc_htm_flush(struct perf_session *session __maybe_unused,
			     const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void powerpc_htm_free_events(struct perf_session *session)
{
	struct powerpc_htm *htm;

	if (!session || !session->auxtrace)
		return;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	auxtrace_queues__free(&htm->queues);
}

static void powerpc_htm_free(struct perf_session *session)
{
	struct powerpc_htm *htm;

	if (!session || !session->auxtrace)
		return;

	htm = container_of(session->auxtrace, struct powerpc_htm, auxtrace);
	powerpc_htm_free_events(session);
	session->auxtrace = NULL;
	free(htm->cpu_configs);
	free(htm->htm_bin_targets);
	free(htm->translation_targets);
	free(htm);
}

int powerpc_htm_process_auxtrace_info(union perf_event *event,
				      struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct powerpc_htm *htm;
	u64 num_events;
	u64 i;
	int err;

	if (auxtrace_info->header.size < sizeof(struct perf_record_auxtrace_info) +
					 HTM_AUXTRACE_PRIV_FIXED)
		return -EINVAL;

	htm = zalloc(sizeof(struct powerpc_htm));
	if (!htm)
		return -ENOMEM;

	/* Reject duplicate AUXTRACE_INFO: would overwrite session->auxtrace and leak */
	if (session->auxtrace) {
		pr_err("htm: duplicate PERF_RECORD_AUXTRACE_INFO, ignoring\n");
		free(htm);
		return -EINVAL;
	}

	htm->auxtrace_type = auxtrace_info->priv[POWERPC_HTM_PMU_TYPE];
	num_events = auxtrace_info->priv[POWERPC_HTM_NUM_EVENTS];

	/*
	 * Validate num_events against the actual header size before using it
	 * to index priv[] or allocate arrays.  Each event contributes 2 u64
	 * priv entries (cpu + config); reject if the header is too small.
	 */
	if (num_events > (auxtrace_info->header.size -
			  sizeof(struct perf_record_auxtrace_info) -
			  HTM_AUXTRACE_PRIV_FIXED) / (2 * sizeof(u64))) {
		pr_err("htm: num_events %llu exceeds auxtrace_info payload\n",
		       (unsigned long long)num_events);
		free(htm);
		return -EINVAL;
	}

	err = auxtrace_queues__init(&htm->queues);
	if (err) {
		free(htm);
		return err;
	}

	/*
	 * All three arrays are sized to num_events -- the exact count of HTM
	 * targets written by htm_info_fill() at record time.  num_events has
	 * been validated above so the cast to int and size_t are safe.
	 */
	htm->nr_targets   = (int)num_events;
	htm->cpu_configs  = calloc((size_t)num_events, sizeof(*htm->cpu_configs));
	htm->htm_bin_targets    = calloc((size_t)num_events, sizeof(*htm->htm_bin_targets));
	htm->translation_targets = calloc((size_t)num_events, sizeof(*htm->translation_targets));
	if (!htm->cpu_configs || !htm->htm_bin_targets || !htm->translation_targets) {
		free(htm->cpu_configs);
		free(htm->htm_bin_targets);
		free(htm->translation_targets);
		auxtrace_queues__free(&htm->queues);
		free(htm);
		return -ENOMEM;
	}

	/*
	 * Read (cpu, config) pairs from priv[].  These were written by
	 * htm_info_fill() at record time -- one pair per htm evsel in evlist
	 * order.  Keying by CPU lets process_auxtrace_event() look up the
	 * correct attr.config for each AUX buffer using event->auxtrace.cpu.
	 */
	for (i = 0; i < num_events; i++) {
		htm->cpu_configs[i].cpu =
			(int)auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + i * 2];
		htm->cpu_configs[i].config =
			auxtrace_info->priv[POWERPC_HTM_EVENT_DATA + i * 2 + 1];
		htm->nr_cpu_configs++;
	}

	htm->session = session;
	htm->machine = &session->machines.host;
	htm->auxtrace.process_event = powerpc_htm_process_event;
	htm->auxtrace.process_auxtrace_event = powerpc_htm_process_auxtrace_event;
	htm->auxtrace.flush_events = powerpc_htm_flush;
	htm->auxtrace.free_events = powerpc_htm_free_events;
	htm->auxtrace.free = powerpc_htm_free;
	session->auxtrace = &htm->auxtrace;

	return 0;
}
