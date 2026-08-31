// SPDX-License-Identifier: GPL-2.0
/*
 * BPF monitor support: allows rv to control BPF monitors.
 *
 * Copyright (C) 2026 Red Hat Inc, Gabriele Monaco <gmonaco@redhat.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <getopt.h>
#include <errno.h>
#include <inttypes.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <linux/compiler.h>
#include <linux/math.h>

#include <bpf_monitor.h>
#include <in_kernel.h>
#include <utils.h>
#include <rv.h>

static char bpf_monitor_paths[][MAX_PATH] = {
	"./bpf_monitors/",
	"/usr/share/rv/bpf_monitors/",
	"", /* Marker */
};

/* Path used for development monitors, searched first */
#define DEVEL_PATH 0

#define MAX_ENUMS 64
#define MAX_LINKS 16
#define RV_TRACE_EVENT	0
#define RV_TRACE_ERROR	1
#define BPF_PIN_BASE_PATH "/sys/fs/bpf/rv"
#define RV_TRACE_STRUCT "rv_trace_entry"

enum da_field_id {
	FIELD_EVENT_TYPE,
	FIELD_ID,
	FIELD_CPU,
	FIELD_PID,
	FIELD_COMM,
	FIELD_IS_FINAL,
	FIELD_CURR_STATE,
	FIELD_EVENT,
	FIELD_NEXT_STATE,
	FIELD_MAX,
};

static const char *const field_names[] = {
	[FIELD_EVENT_TYPE] = "event_type",
	[FIELD_ID] = "id",
	[FIELD_CPU] = "cpu",
	[FIELD_PID] = "pid",
	[FIELD_COMM] = "comm",
	[FIELD_IS_FINAL] = "is_final",
	[FIELD_CURR_STATE] = "curr_state",
	[FIELD_EVENT] = "event",
	[FIELD_NEXT_STATE] = "next_state",
};

struct da_field {
	size_t offset;
	size_t size;
};

struct bpf_monitor_ctx {
	char monitor_name[MAX_DA_NAME_LEN];
	char state_names[MAX_ENUMS][MAX_DA_NAME_LEN];
	char event_names[MAX_ENUMS][MAX_DA_NAME_LEN];
	int num_states;
	int num_events;
	struct da_field field_metadata[FIELD_MAX];
};

/*
 * bpf_fill_monitor_paths - fill the path for development builds
 *
 * RV searches for BPF monitors on absolute paths on the system as well
 * as in the same directory of the rv binary. This is useful when running
 * rv from the kernel tree. This function resolves right location.
 */
static void bpf_fill_monitor_paths(void)
{
	char tmp_path[MAX_PATH], *dir;
	ssize_t len;

	len = readlink("/proc/self/exe", tmp_path, MAX_PATH);
	if (len > 0 && len != MAX_PATH) {
		tmp_path[len] = '\0';
		dir = dirname(tmp_path);
		snprintf(bpf_monitor_paths[DEVEL_PATH], MAX_PATH, "%s/bpf_monitors", dir);
	}
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !config.debug)
		return 0;
	return vfprintf(stderr, format, args);
}

/*
 * Helper functions for state/event name lookup
 */
static const char *get_state_name(struct bpf_monitor_ctx *ctx, uint32_t state)
{
	static char buf[16];

	if (state < ctx->num_states && ctx->state_names[state][0] != '\0')
		return ctx->state_names[state];

	snprintf(buf, sizeof(buf), "%u", state);
	return buf;
}

static const char *get_event_name(struct bpf_monitor_ctx *ctx, uint32_t event)
{
	static char buf[16];

	if (event < ctx->num_events && ctx->event_names[event][0] != '\0')
		return ctx->event_names[event];

	snprintf(buf, sizeof(buf), "%u", event);
	return buf;
}

/*
 * extract_field_metadata - extract field metadata from BTF for efficient parsing
 *
 * Introspect the rv_trace_entry structure via BTF and store field offsets and
 * sizes for direct memory access during event processing.
 *
 * Returns 0 on success, -1 on error
 */
static int extract_field_metadata(const struct btf *btf, struct bpf_monitor_ctx *ctx)
{
	const struct btf_type *trace_type;
	const struct btf_member *members;
	int type_id, vlen;

	type_id = btf__find_by_name_kind(btf, RV_TRACE_STRUCT, BTF_KIND_STRUCT);
	if (type_id <= 0) {
		debug_msg("bpf: could not find struct '%s' in BTF\n", RV_TRACE_STRUCT);
		return -1;
	}

	trace_type = btf__type_by_id(btf, type_id);
	if (!trace_type) {
		debug_msg("bpf: could not get type for '%s'\n", RV_TRACE_STRUCT);
		return -1;
	}

	members = btf_members(trace_type);
	vlen = btf_vlen(trace_type);

	for (int i = 0; i < vlen; i++) {
		const char *name = btf__name_by_offset(btf, members[i].name_off);
		size_t offset = btf_member_bit_offset(trace_type, i) / 8;
		size_t size = btf__resolve_size(btf, members[i].type);

		if (!name || (ssize_t)size < 0)
			continue;

		debug_msg("bpf: field '%s' at offset %zu, size %lld\n", name,
			  offset, (long long)size);

		for (int j = 0; j < FIELD_MAX; j++) {
			if (strcmp(name, field_names[j]) == 0) {
				ctx->field_metadata[j].offset = offset;
				ctx->field_metadata[j].size = size;
				if (j == FIELD_ID)
					config.has_id = true;
				break;
			}
		}
	}

	return 0;
}

/*
 * bpf_print_header - print trace output header
 */
static void bpf_print_header(void)
{
	printf("%16s-%-8s %5s %5s ", "<TASK>", "PID", "[CPU]", "TYPE");
	if (config.has_id)
		printf(" %8s", "ID");

	printf("%24s x %-24s -> %-24s %s\n",
		"STATE",
		"EVENT",
		"NEXT_STATE",
		"FINAL");

	printf("%16s %-8s %5s %5s ", " | ", " | ", " | ", " | ");

	if (config.has_id)
		printf(" %8s", " | ");
	printf("%24s   %-24s    %-24s %s\n", " | ", " | ", " | ", "|");
}

static inline uint64_t read_field(uint64_t *entry, enum da_field_id id,
				  const uint8_t *raw,
				  const struct bpf_monitor_ctx *ctx)
{
	const struct da_field *field = &ctx->field_metadata[id];

	switch (field->size) {
	case 1:
		return entry[id] = *(const uint8_t *)(raw + field->offset);
	case 2:
		return entry[id] = *(const uint16_t *)(raw + field->offset);
	case 4:
		return entry[id] = *(const uint32_t *)(raw + field->offset);
	case 8:
		return entry[id] = *(const uint64_t *)(raw + field->offset);
	}
	return 0;
}

/*
 * handle_event - ring buffer callback for trace events
 */
static int handle_event(void *ctx, void *data, size_t data_sz)
{
	struct bpf_monitor_ctx *mon_ctx = ctx;
	const uint8_t *raw = data;
	uint64_t entry[FIELD_MAX] = {0};
	const char *comm;

	if (should_stop())
		return 1;

	if (config.has_id)
		read_field(entry, FIELD_ID, raw, mon_ctx);
	read_field(entry, FIELD_PID, raw, mon_ctx);

	if (config.has_id && (config.my_pid == entry[FIELD_ID]))
		return 0;
	else if (config.my_pid == entry[FIELD_PID])
		return 0;

	read_field(entry, FIELD_EVENT_TYPE, raw, mon_ctx);
	read_field(entry, FIELD_CPU, raw, mon_ctx);
	comm = (const char *)(raw + mon_ctx->field_metadata[FIELD_COMM].offset);
	read_field(entry, FIELD_CURR_STATE, raw, mon_ctx);
	read_field(entry, FIELD_EVENT, raw, mon_ctx);

	printf("%16s-%-8"PRIu64" [%.3"PRIu64"] ", comm, entry[FIELD_PID], entry[FIELD_CPU]);
	if (entry[FIELD_EVENT_TYPE] == RV_TRACE_ERROR) {
		printf("error ");
		if (config.has_id)
			printf(" %8"PRIu64"", entry[FIELD_ID]);
		printf(" %24s x %-24s\n",
		       get_state_name(mon_ctx, entry[FIELD_CURR_STATE]),
		       get_event_name(mon_ctx, entry[FIELD_EVENT]));
	} else {
		printf("event ");
		read_field(entry, FIELD_IS_FINAL, raw, mon_ctx);
		read_field(entry, FIELD_NEXT_STATE, raw, mon_ctx);

		if (config.has_id)
			printf(" %8"PRIu64"", entry[FIELD_ID]);
		printf(" %24s x %-24s -> %-24s %c\n",
		       get_state_name(mon_ctx, entry[FIELD_CURR_STATE]),
		       get_event_name(mon_ctx, entry[FIELD_EVENT]),
		       get_state_name(mon_ctx, entry[FIELD_NEXT_STATE]),
		       entry[FIELD_IS_FINAL] ? 'Y' : 'N');
	}

	return 0;
}

/*
 * extract_enum_names - extract names from a BTF enum
 *
 * Reads enum member names from BTF and stores them in dest array.
 * Returns the number of enum members extracted (excluding the
 * {state/event}_max_NAME entry and trimming the _NAME padding).
 */
static int extract_enum_names(const struct btf *btf, const char *enum_kind,
			       char dest[][MAX_DA_NAME_LEN], struct bpf_monitor_ctx *ctx)
{
	const struct btf_type *enum_type;
	const struct btf_enum *enums;
	char enum_name[64];
	int type_id, vlen;
	int count = 0;

	snprintf(enum_name, sizeof(enum_name), "%ss_%s", enum_kind, ctx->monitor_name);
	type_id = btf__find_by_name_kind(btf, enum_name, BTF_KIND_ENUM);
	if (type_id <= 0) {
		err_msg("bpf: could not find enum '%s' in BTF\n", enum_name);
		return -1;
	}
	enum_type = btf__type_by_id(btf, type_id);
	if (!enum_type) {
		err_msg("bpf: could not get enum type for '%s'\n", enum_name);
		return -1;
	}

	enums = btf_enum(enum_type);
	vlen = btf_vlen(enum_type);

	snprintf(enum_name, sizeof(enum_name), "%s_max_%s", enum_kind, ctx->monitor_name);
	for (int i = 0; i < vlen && count < MAX_ENUMS; i++) {
		const char *name = btf__name_by_offset(btf, enums[i].name_off);
		size_t name_len;
		const char *padding;

		if (!name || !strcmp(name, enum_name))
			continue;

		padding = strrchr(name, '_');
		name_len = strlen(name);
		if (padding && !strcmp(ctx->monitor_name, padding + 1))
			name_len = (size_t)(padding - name);

		if (name_len >= MAX_DA_NAME_LEN)
			name_len = MAX_DA_NAME_LEN - 1;
		strncpy(dest[count], name, name_len);
		dest[count][name_len] = '\0';
		count++;
	}

	return count;
}

/*
 * extract_btf_info - extract BTF types information from the monitor
 *
 * Extract state and event names from enums using BTF and extract field
 * offsets for flexible event parsing.
 */
static int extract_btf_info(struct bpf_object *obj, struct bpf_monitor_ctx *ctx)
{
	const struct btf *btf;

	btf = bpf_object__btf(obj);
	if (!btf) {
		err_msg("bpf: no BTF found in BPF object\n");
		return -1;
	}

	if (extract_field_metadata(btf, ctx)) {
		err_msg("bpf: failed to extract field metadata\n");
		return -1;
	}

	ctx->num_states = extract_enum_names(btf, "state", ctx->state_names, ctx);
	ctx->num_events = extract_enum_names(btf, "event", ctx->event_names, ctx);
	if (ctx->num_states < 0 || ctx->num_events < 0) {
		err_msg("bpf: failed to extract states (%d) or events names (%d)\n",
			ctx->num_states, ctx->num_events);
		return -1;
	}

	return 0;
}

/*
 * find_bpf_object - search for BPF monitor object file in all directories
 */
static int find_bpf_object(const char *monitor_name, char *path_out, size_t path_len)
{
	char path[MAX_PATH];

	bpf_fill_monitor_paths();
	for (int i = 0; bpf_monitor_paths[i][0]; i++) {
		size_t size = snprintf(path, sizeof(path), "%s/%s.o",
				       bpf_monitor_paths[i], monitor_name);

		if (size < MAX_PATH && access(path, R_OK) == 0) {
			strncpy(path_out, path, path_len - 1);
			path_out[path_len - 1] = '\0';
			return 1;
		}
	}

	return 0;
}

/*
 * bpf_setup_ring_buffer - set up the ring buffer to trace events
 *
 * Find the ring buffer map, set up the events handler, and consume any
 * pending data to start fresh.
 */
static struct ring_buffer *bpf_setup_ring_buffer(struct bpf_object *obj,
						 struct bpf_monitor_ctx *ctx)
{
	struct ring_buffer *rb;
	struct bpf_map *map;
	char ringbuf_name[64];

	if (extract_btf_info(obj, ctx))
		return NULL;

	snprintf(ringbuf_name, sizeof(ringbuf_name), "da_ringbuf_%s", ctx->monitor_name);
	map = bpf_object__find_map_by_name(obj, ringbuf_name);
	if (!map) {
		err_msg("bpf: error finding ring buffer %s\n", ringbuf_name);
		return NULL;
	}

	rb = ring_buffer__new(bpf_map__fd(map), handle_event, ctx, NULL);
	if (!rb) {
		err_msg("bpf: error opening ring buffer: %s\n", strerror(errno));
		return NULL;
	}

	ring_buffer__consume(rb);

	return rb;
}

/*
 * reset_monitor_maps - clear all elements from monitor's maps
 */
static void reset_monitor_maps(struct bpf_object *obj)
{
	struct bpf_map *map;
	bool err = false;

	bpf_object__for_each_map(map, obj) {
		enum bpf_map_type type = bpf_map__type(map);
		int value_size = bpf_map__value_size(map);
		int fd = bpf_map__fd(map);

		if (fd < 0 || bpf_map__is_internal(map))
			continue;

		switch (type) {
		case BPF_MAP_TYPE_HASH:
		case BPF_MAP_TYPE_PERCPU_HASH: {
			void *key = malloc(bpf_map__key_size(map));

			if (!key) {
				err = true;
				break;
			}

			while (!err && bpf_map_get_next_key(fd, NULL, key) == 0)
				err |= bpf_map_delete_elem(fd, key);

			free(key);
			debug_msg("bpf: reset hash map %s\n", bpf_map__name(map));
			break;
		}

		case BPF_MAP_TYPE_PERCPU_ARRAY:
			/*
			 * Per-CPU maps require a buffer for all CPUs with each
			 * element padded to 8 bytes
			 */
			value_size = libbpf_num_possible_cpus() *
				     round_up(bpf_map__value_size(map), 8);
			fallthrough;
		case BPF_MAP_TYPE_ARRAY: {
			uint32_t max_entries = bpf_map__max_entries(map);
			void *zero_value = calloc(1, value_size);

			if (!zero_value) {
				err = true;
				break;
			}

			for (uint32_t idx = 0; !err && idx < max_entries; idx++)
				err |= bpf_map_update_elem(fd, &idx, zero_value, BPF_ANY);

			free(zero_value);
			debug_msg("bpf: zeroed array map %s\n", bpf_map__name(map));
			break;
		}
		default:
		}
	}
	if (err)
		err_msg("bpf: errors during maps reset, continuing anyway.\n");
}

/*
 * open_bpf_monitor - open and load a BPF monitor object
 *
 * If path is NULL, searches for the monitor by name. Otherwise uses the
 * provided path directly.
 *
 * Returns loaded BPF object on success, NULL on error.
 */
static struct bpf_object *open_bpf_monitor(const char *monitor_name, const char *path)
{
	struct bpf_object *obj = NULL;
	char _path[MAX_PATH];
	int res;

	LIBBPF_OPTS(bpf_object_open_opts, opts,
		.pin_root_path = BPF_PIN_BASE_PATH,
	);

	if (!path) {
		if (!find_bpf_object(monitor_name, _path, sizeof(_path))) {
			err_msg("bpf: error finding monitor %s\n", monitor_name);
			return NULL;
		}
		path = _path;
	}

	obj = bpf_object__open_file(path, &opts);
	if (!obj) {
		err_msg("bpf: error opening object file: %s\n", strerror(errno));
		return NULL;
	}

	res = bpf_object__load(obj);
	if (res) {
		err_msg("bpf: error loading object file: %s\n", strerror(-res));
		return NULL;
	}

	return obj;
}

/*
 * attach_bpf_handlers - attach and pin all BPF programs
 *
 * Attaches all non-struct_ops programs and stores links in the provided array.
 * Reuses existing pinned links when available.
 *
 * Returns number of attached programs on success, -1 on error.
 */
static int attach_bpf_handlers(const char *monitor_name, struct bpf_object *obj,
				struct bpf_link **links, int *link_count)
{
	struct bpf_program *prog;
	int res = 0;

	bpf_object__for_each_program(prog, obj) {
		struct bpf_link *link = NULL;
		char pin_path[MAX_PATH];
		const char *prog_name;
		bool reused = true;

		if (bpf_program__type(prog) == BPF_PROG_TYPE_STRUCT_OPS)
			continue;

		if (*link_count >= MAX_LINKS) {
			err_msg("bpf: too many programs to attach (%d)\n", *link_count);
			return -1;
		}

		prog_name = bpf_program__name(prog);
		snprintf(pin_path, sizeof(pin_path), "%s/%s_%s",
			 BPF_PIN_BASE_PATH, monitor_name, prog_name);

		if (access(pin_path, F_OK) == 0)
			link = bpf_link__open(pin_path);
		if (!link) {
			reused = false;
			unlink(pin_path);
			link = bpf_program__attach(prog);
			if (!link) {
				err_msg("bpf: error attaching program '%s': %s\n",
					prog_name, strerror(errno));
				return -1;
			}
		}
		links[(*link_count)++] = link;

		if (!reused) {
			res = bpf_link__pin(link, pin_path);
			if (res) {
				err_msg("bpf: failed to pin link '%s': %s\n",
					prog_name, strerror(-res));
				return -1;
			}
		}
	}
	return res;
}

/*
 * bpf_run_monitor - load and run a BPF monitor
 *
 * Returns 1 if monitor was found and executed, 0 if not found, -1 on error
 */
int bpf_run_monitor(char *monitor_name, int argc, char **argv)
{
	struct bpf_link *links[MAX_LINKS] = {0};
	struct bpf_monitor_ctx ctx = {0};
	struct ring_buffer *rb = NULL;
	struct bpf_object *obj = NULL;
	char structops_pin[MAX_PATH];
	int res, link_count = 0, retval = -1;

	/* If struct_ops is not registered this is not a BPF monitor */
	snprintf(structops_pin, sizeof(structops_pin), "%s/rv_%s_kern",
		 BPF_PIN_BASE_PATH, monitor_name);
	if (access(structops_pin, F_OK) != 0)
		return 0;

	if (__ikm_read_enable(monitor_name) == 1) {
		err_msg("bpf: monitor %s (BPF) is already enabled\n", monitor_name);
		return -1;
	}

	strncpy(ctx.monitor_name, monitor_name, sizeof(ctx.monitor_name) - 1);

	res = parse_arguments(monitor_name, argc, argv);
	if (res)
		mon_usage(1, monitor_name, "bpf: failed parsing arguments");

	ikm_set_reactor(monitor_name);

	libbpf_set_print(libbpf_print_fn);

	obj = open_bpf_monitor(monitor_name, NULL);
	if (!obj)
		goto cleanup;

	if (config.trace) {
		rb = bpf_setup_ring_buffer(obj, &ctx);
		if (!rb)
			goto cleanup;
	}

	reset_monitor_maps(obj);

	res = attach_bpf_handlers(monitor_name, obj, links, &link_count);
	if (res < 0)
		goto cleanup;

	res = ikm_enable(monitor_name);
	if (res < 0) {
		err_msg("bpf: error enabling the monitor: %s\n", monitor_name);
		goto cleanup;
	}

	if (config.trace)
		bpf_print_header();

	while (!should_stop()) {
		if (!config.trace) {
			sleep(1);
			continue;
		}
		res = ring_buffer__poll(rb, 100);
		if (res == -EINTR)
			break;
		if (res < 0) {
			err_msg("bpf: error polling ring buffer: %s\n", strerror(-res));
			goto cleanup;
		}
	}
	retval = 1;

cleanup:
	ikm_disable(monitor_name);
	reset_monitor_maps(obj);
	ikm_reset_reactor(monitor_name);

	for (int i = 0; i < link_count; i++) {
		bpf_link__unpin(links[i]);
		bpf_link__destroy(links[i]);
	}

	if (config.trace)
		ring_buffer__free(rb);

	bpf_object__close(obj);

	return retval;
}

/*
 * register_monitor_from_file - register a single BPF monitor via struct_ops
 *
 * Opens, loads, and attaches a BPF monitor's struct_ops map, then pins the
 * resulting link.
 *
 * Returns 0 on success, -1 on error
 */
static int register_monitor_from_file(const char *path, const char *name)
{
	struct bpf_object *obj = NULL;
	struct bpf_link *link = NULL;
	struct bpf_map *map;
	char map_name[MAX_DA_NAME_LEN + 8];
	char pin_path[MAX_PATH];
	int res, retval = -1;

	obj = open_bpf_monitor(name, path);
	if (!obj)
		goto cleanup;

	snprintf(map_name, sizeof(map_name), "rv_%s_kern", name);
	map = bpf_object__find_map_by_name(obj, map_name);
	if (!map) {
		err_msg("bpf: error finding struct_ops map for %s\n", name);
		goto cleanup;
	}

	link = bpf_map__attach_struct_ops(map);
	if (!link) {
		err_msg("bpf: error attaching struct_ops for %s: %s\n",
			name, strerror(errno));
		goto cleanup;
	}

	snprintf(pin_path, sizeof(pin_path), "%s/%s", BPF_PIN_BASE_PATH, map_name);
	res = bpf_link__pin(link, pin_path);
	if (res) {
		err_msg("bpf: error pinning link for %s: %s\n",
			name, strerror(-res));
		goto cleanup;
	}

	debug_msg("bpf: registered %s\n", name);
	retval = 0;

cleanup:
	bpf_link__destroy(link);
	bpf_object__close(obj);

	return retval;
}

/*
 * register_monitors_from_path - register all BPF monitors from a path
 *
 * Scans directory for *.o files and registers each one via struct_ops.
 *
 * Returns number of monitors registered from this path
 */
static int register_monitors_from_path(const char *path, bool *error)
{
	struct dirent *entry;
	DIR *dir;
	char *ext;
	int count = 0;

	dir = opendir(path);
	if (!dir)
		return 0;

	while ((entry = readdir(dir)) != NULL) {
		char name[MAX_DA_NAME_LEN], mon_path[MAX_PATH];
		size_t size;

		if (entry->d_name[0] == '.')
			continue;

		ext = strrchr(entry->d_name, '.');
		if (!ext || strcmp(ext, ".o") != 0)
			continue;

		size = snprintf(mon_path, sizeof(mon_path), "%s/%s",
				path, entry->d_name);
		if (size >= MAX_PATH) {
			err_msg("bpf: path too long: %s/%s\n", path, entry->d_name);
			*error = true;
			continue;
		}

		strncpy(name, entry->d_name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		ext = strrchr(name, '.');
		if (ext)
			*ext = '\0';

		if (register_monitor_from_file(mon_path, name) == 0)
			count++;
		else
			*error = true;
	}

	closedir(dir);
	return count;
}

/*
 * bpf_struct_ops_supported - check if the kernel supports BPF monitors
 */
static bool bpf_struct_ops_supported(void)
{
	struct btf *btf;
	int type_id;

	btf = btf__load_vmlinux_btf();
	if (!btf)
		return false;

	type_id = btf__find_by_name_kind(btf, "bpf_struct_ops_rv_monitor", BTF_KIND_STRUCT);
	btf__free(btf);

	if (type_id <= 0)
		return false;

	return true;
}

/*
 * bpf_register_monitors - register all BPF monitors via struct_ops
 *
 * Scans known paths for BPF monitor objects, loads them, and attaches their
 * struct_ops maps. The resulting links are pinned so monitors remain
 * registered with the kernel RV subsystem even after this process exits.
 *
 * Returns command exit status (SUCCESS/FAILURE)
 */
static int bpf_register_monitors(void)
{
	int count = 0;
	bool error = false;

	bpf_fill_monitor_paths();

	for (int i = 0; bpf_monitor_paths[i][0]; i++)
		count += register_monitors_from_path(bpf_monitor_paths[i], &error);

	printf("registered %d BPF monitor(s)\n", count);

	return error ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * is_struct_ops - return true if d_name matches rv_<mon>_kern
 *
 * Also store <mon> in name_out when returning true.
 */
static bool is_struct_ops(char *d_name, char *name_out)
{
	const int rvlen = strlen("rv_"), kernlen = strlen("_kern");
	int len = strlen(d_name);

	if (len > rvlen + kernlen && !strncmp(d_name, "rv_", rvlen) &&
	    !strcmp(d_name + len - kernlen, "_kern")) {
		len -= rvlen + kernlen;
		if (name_out) {
			if (len >= MAX_DA_NAME_LEN)
				len = MAX_DA_NAME_LEN - 1;
			strncpy(name_out, d_name + rvlen, len);
			name_out[len] = '\0';
		}
		return true;
	}
	return false;
}

enum unregister_pass {
	CHECK_ACTIVE,
	UNLINK_HANDLERS,
	UNLINK_STRUCT_OPS,
	MAX_PASSES,
};

/*
 * bpf_unregister_monitors - unregister all BPF monitors
 *
 * Removes all pinned links and maps from BPF_PIN_BASE_PATH, including the
 * struct_ops. This unregisters the monitors from the RV subsystem and cleans
 * up all associated resources.
 *
 * First iterate over the directory to check if any BPF monitor is active,
 * this is best effort. Then unlink all handlers and at last all struct_ops.
 *
 * Returns command exit status (SUCCESS/FAILURE)
 */
static int bpf_unregister_monitors(void)
{
	struct dirent *entry;
	DIR *dir;
	int count = 0;

	dir = opendir(BPF_PIN_BASE_PATH);
	if (!dir) {
		err_msg("bpf: cannot open %s: %s\n",
			BPF_PIN_BASE_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	for (int pass = CHECK_ACTIVE; pass < MAX_PASSES; pass++) {
		rewinddir(dir);
		while ((entry = readdir(dir)) != NULL) {
			char pin_path[MAX_PATH];
			char mon_name[MAX_DA_NAME_LEN];
			int is_ops;

			if (entry->d_name[0] == '.')
				continue;

			is_ops = is_struct_ops(entry->d_name, mon_name);

			switch (pass) {
			case CHECK_ACTIVE:
				if (is_ops && __ikm_read_enable(mon_name) == 1) {
					err_msg("bpf: monitor %s is enabled, cannot unregister\n",
						mon_name);
					closedir(dir);
					return EXIT_FAILURE;
				}
				continue;
			case UNLINK_HANDLERS:
				if (is_ops)
					continue;
				fallthrough;
			case UNLINK_STRUCT_OPS:
				snprintf(pin_path, sizeof(pin_path), "%s/%s",
					 BPF_PIN_BASE_PATH, entry->d_name);

				if (unlink(pin_path)) {
					err_msg("bpf: failed to unlink %s: %s\n", pin_path,
						strerror(errno));
				} else if (is_ops) {
					count++;
					debug_msg("bpf: unregistered %s\n", mon_name);
				}
				break;
			}
		}
	}

	closedir(dir);

	printf("unregistered %d BPF monitor(s)\n", count);
	return EXIT_SUCCESS;
}

/*
 * rv_bpf - handle BPF monitor registration commands
 */
void rv_bpf(int argc, char **argv)
{
	static const char *const usage[] = {
		"",
		"  usage: rv bpf [-h] [-v] {register,unregister}",
		"",
		"\tmanage BPF monitor registration",
		"",
		"\t-h/--help: print this menu",
		"\t-v/--verbose: print debug messages",
		"",
		"\tregister:   register all available BPF monitors",
		"\tunregister: unregister all BPF monitors",
		NULL,
	};
	int print_help = 0, retval = EXIT_SUCCESS;

	while (1) {
		static struct option long_options[] = {
			{"help",	no_argument,		0, 'h'},
			{"verbose",	no_argument,		0, 'v'},
			{0, 0, 0, 0}
		};
		int option_index = 0;
		int c = getopt_long(argc, argv, "hv", long_options, &option_index);

		/* detect the end of the options */
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_help = 1;
			retval = EXIT_SUCCESS;
			break;
		case 'v':
			config.debug = 1;
			break;
		default:
			print_help = 1;
			retval = EXIT_FAILURE;
			break;
		}
	}

	/* requires at least one argument after options */
	if (optind >= argc && !print_help) {
		print_help = 1;
		retval = EXIT_FAILURE;
	}

	if (print_help) {
		fprintf(stderr, "rv version %s\n", VERSION);
		for (int i = 0; usage[i]; i++)
			fprintf(stderr, "%s\n", usage[i]);
		exit(retval);
	}

	if (!bpf_struct_ops_supported()) {
		err_msg("bpf: kernel does not support BPF monitors\n");
		exit(EXIT_FAILURE);
	}

	if (!strcmp(argv[optind], "register")) {
		retval = bpf_register_monitors();
		exit(retval);
	}

	if (!strcmp(argv[optind], "unregister")) {
		retval = bpf_unregister_monitors();
		exit(retval);
	}

	/* invalid sub-command */
	fprintf(stderr, "rv version %s\n", VERSION);
	for (int i = 0; usage[i]; i++)
		fprintf(stderr, "%s\n", usage[i]);
	exit(EXIT_FAILURE);
}
