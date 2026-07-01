// SPDX-License-Identifier: GPL-2.0
/*
 * HTM support
 */

#include "../../../util/record.h"
#include "evlist.h"
#include "evsel.h"
#include "session.h"
#include "debug.h"
#include <internal/xyarray.h>
#include <linux/string.h>
#include "color.h"
#include <inttypes.h>
#include "powerpc-htm.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include "sample.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <regex.h>
#include <ctype.h>
#include <errno.h>

/* mask the 64th bit of a physical address */
#define PHYS_ADDR_MASK        0x7FFFFFFFFFFFFFFFUL
const char *lpar_cfg = "/proc/powerpc/lparcfg";

struct perf_session;

struct powerpc_htm {
	struct auxtrace			auxtrace;
	struct auxtrace_queues		queues;
	struct auxtrace_heap		heap;
	u32				auxtrace_type;
	struct perf_session		*session;
	struct machine			*machine;
	u32				pmu_type;
	char				htmbin_file[64];
	char				trans_file[64];
	int				htm_mem_entries;
	int				mem_maps;
};

struct htm_mem {
	uint64_t phy_real;
	uint64_t logical_real;
	uint32_t lp_index;
	uint8_t mem_tier;
	uint8_t mem_type;
	uint16_t res;
	uint64_t size;
};

struct mem_entries {
	unsigned long long phy_addr;
	unsigned long logical_addr;
	u32 lp_index;
	u64 size;
};

struct addr_map {
	char event[64];
	unsigned long long phys_addr;
	unsigned long logical_addr;
};

static int run_htmdecode(const char *input_file, const char *output_file)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == -1) {
		pr_err("fork() failed: %s\n", strerror(errno));
		return -errno;
	}

	if (pid == 0) {
		/* Child process */
		int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd == -1) {
			pr_err("Failed to open output file: %s\n", strerror(errno));
			exit(1);
		}

		/* Redirect stdout to output file */
		dup2(fd, STDOUT_FILENO);
		close(fd);

		/* Execute htmdecode - execlp searches PATH automatically */
		execlp("htmdecode", "htmdecode", "-o", "-j", "-w", "1",
			"-f", input_file, NULL);

		/* If execlp returns, it failed */
		pr_err("Failed to execute htmdecode: %s\n", strerror(errno));
		if (errno == ENOENT)
			pr_err("htmdecode not found in PATH\n");

		exit(127);  /* Standard "command not found" exit code */
	}

	/* Parent process - wait for child */
	if (waitpid(pid, &status, 0) == -1) {
		pr_err("waitpid() failed: %s\n", strerror(errno));
		return -errno;
	}

	/* Check exit status */
	if (WIFEXITED(status)) {
		int exit_code = WEXITSTATUS(status);

		if (exit_code == 127) {
			pr_err("htmdecode not found in PATH\n");
			return -ENOENT;
		} else if (exit_code != 0) {
			pr_err("htmdecode failed with exit code %d\n", exit_code);
			return -EINVAL;
		}
	} else if (WIFSIGNALED(status)) {
		pr_err("htmdecode killed by signal %d\n", WTERMSIG(status));
		return -EINTR;
	}

	return 0;
}

static void *safe_realloc(void *ptr, size_t new_size)
{
	void *tmp = realloc(ptr, new_size);

	if (!tmp) {
		pr_err("realloc failed: %s\n", strerror(errno));
		return NULL;
	}

	return tmp;
}

static int add_map_entry(struct addr_map **arr, size_t *count, size_t *cap, struct addr_map entry)
{
	if (*count >= *cap) {
		size_t new_cap = (*cap == 0) ? 1024 : (*cap * 2);
		void *tmp = safe_realloc(*arr, new_cap * sizeof(struct addr_map));

		if (!tmp)
			return -1;  // allocation failed
		*arr = tmp;
		*cap = new_cap;

	}

	(*arr)[(*count)++] = entry;
	return 0;
}

/*
 * This effectively maps a physical address to its logical address
 * within the selected LP partition.
 */
static unsigned long find_logical_addr(unsigned long long given_addr,
				struct mem_entries *mem_entries_array,
				size_t n_entries,
				u32 lp_filter)
{
	for (size_t i = 0; i < n_entries; i++) {
		unsigned long long start = mem_entries_array[i].phy_addr & PHYS_ADDR_MASK;
		unsigned long long end   = start + mem_entries_array[i].size;

		/* Skip entries with invalid logical_start sentinel */
		if (mem_entries_array[i].logical_addr == UINT64_MAX) {
			pr_debug("  SKIP i=%zu: logical_start sentinel=0x%016llx\n",
			i, (unsigned long long)mem_entries_array[i].logical_addr);
			continue;
		}

		/*
		 * Check if 'given_addr' falls within the physical memory range of this entry
		 * and belongs to the LP partition indicated by 'lp_filter'.
		 * If so, calculate:
		 * 'offset' and the 'logical address
		 */
		if (start <= given_addr && given_addr < end &&
			mem_entries_array[i].lp_index == lp_filter) {
			unsigned long long offset = given_addr - start;
			unsigned long logical = mem_entries_array[i].logical_addr + offset;

			pr_debug("DEBUG: Condition hit at i=%zu given_addr=0x%llx start=0x%llx end=0x%llx lp_index=%u\n",
				i, given_addr,
				start, end,
				(unsigned int)mem_entries_array[i].lp_index);
			pr_debug("logical = 0x%016lx\n", logical);
			return logical;
		}
	}

	return 0;
}

/*
 * Parse the HTM trace file line by line, extracting memory addresses and labels.
 * Map each memory addresses to a corresponding logical address for a given
 * lp_filter. Store the results in a dynamically growing map of entries.
 */
static struct addr_map *process_trace_file(const char *trace_file,
				struct mem_entries *mem_entries_array,
				size_t n_entries,
				u32 lp_filter,
				size_t *count_out)
{
	regex_t addr_regex, label_regex;
	struct addr_map *maps;
	size_t count, cap;
	char *line = NULL;
	size_t len;
	regmatch_t pmatch[2];
	const char *ptr;
	unsigned long logical_addr;
	size_t total_phys = 0;
	size_t total_phys_to_logical = 0;

	FILE *fp = fopen(trace_file, "r");

	if (!fp) {
		pr_err("Failed to open trace file %s: %s\n", trace_file, strerror(errno));
		return NULL;
	}

	if (regcomp(&addr_regex, "addr:0x[0-9A-Fa-f]+", REG_EXTENDED) != 0) {
		pr_err("Failed to compile addr_regex\n");
		return NULL;
	}

	if (regcomp(&label_regex,
		   "^[[:space:]]*[0-9A-Fa-f]+ : [^[:space:]]+[[:space:]]+([^[:space:]]+)",
		   REG_EXTENDED) != 0) {
		pr_err("Failed to compile label_regex\n");
		return NULL;
	}

	maps = NULL;
	count = 0;
	cap = 0;

	while (getline(&line, &len, fp) != -1) {
		if (regexec(&label_regex, line, 2, pmatch, 0) == 0) {
			char label[64] = {0};
			int start = pmatch[1].rm_so;
			int end   = pmatch[1].rm_eo;
			int line_len   = end - start;

			if (line_len < 0)
				line_len = 0;
			if ((size_t)line_len > sizeof(label) - 1)
				line_len = (int)(sizeof(label) - 1);

			/* Use snprintf to copy exactly len characters and
			 * always null terminate
			 */
			snprintf(label, sizeof(label), "%.*s", line_len, line + start);
			ptr = line;
			while (regexec(&addr_regex, ptr, 1, pmatch, 0) == 0) {
				unsigned long long phys_addr = 0;
				struct addr_map entry = {0};

				if (sscanf(ptr + pmatch[0].rm_so + strlen("addr:"),
					  "%llx", &phys_addr) != 1) {
					pr_debug("Failed to parse phys addr from trace line\n");
					continue;
				}

				total_phys++;
				pr_debug("Total Phys[%zu]: 0x%016llx\n", total_phys, phys_addr);
				logical_addr = find_logical_addr(phys_addr,
							mem_entries_array,
							n_entries,
							lp_filter);
				if (logical_addr == 0) {
					ptr += pmatch[0].rm_eo;
					continue;
				} else {
					total_phys_to_logical++;
					pr_debug("  Phys: 0x%016llx to Logical: 0x%016lx\n",
					phys_addr,
					logical_addr);
				}
				pr_debug("Total physical to logical found    : %zu\n",
					total_phys_to_logical);
				snprintf(entry.event, sizeof(entry.event), "%s", label);
				entry.phys_addr   = phys_addr;
				entry.logical_addr = logical_addr;

				add_map_entry(&maps, &count, &cap, entry);

				ptr += pmatch[0].rm_eo;
			}
		}
	}

	free(line);
	fclose(fp);
	regfree(&addr_regex);
	regfree(&label_regex);

	*count_out = count;
	return maps;
}

static int create_mem_maps(struct powerpc_htm *htm)
{
	off_t file_size;
	void *htmdata, *mapped_data;
	int fd;
	struct stat file_info;
	struct htm_mem *mem;
	char tracefile[128];
	int ret;
	u32 lp_filter = 0;
	size_t n_entries = htm->htm_mem_entries;
	struct mem_entries *mem_entries_array;
	size_t num_maps = 0;
	struct addr_map *maps;
	FILE *fp;
	char lp_line[256];

	snprintf(tracefile, sizeof(tracefile), "%s.out", htm->htmbin_file);

	ret = run_htmdecode(htm->htmbin_file, tracefile);
	if (ret) {
		if (ret == -ENOENT)
			pr_info("htmdecode not found. Install htmdecode to decode traces.\n");
		else
			pr_info("htmdecode failed with error %d\n", ret);
		return ret;
	}

	/* get the lp index */
	fp = fopen(lpar_cfg, "r");
	if (!fp) {
		pr_err("Failed to open %s: %s\n", lpar_cfg, strerror(errno));
		return -errno;
	}

	while (fgets(lp_line, sizeof(lp_line), fp)) {
		if (strncmp(lp_line, "partition_id=", 13) == 0) {
			lp_filter = strtoul(lp_line + 13, NULL, 10);
			break;
		}
	}

	fclose(fp);

	if (lp_filter == 0)
		pr_info("partition_id not found in %s\n", lpar_cfg);
	else
		pr_info("Using partition_id = %" PRIu32 "\n", lp_filter);

	fd = open(htm->trans_file, O_RDONLY);
	if (fd == -1) {
		pr_err("Failed to open %s: %s\n", htm->trans_file, strerror(errno));
		return -1;
	}

	if (fstat(fd, &file_info) == -1) {
		close(fd);
		pr_err("fstat failed on %s: %s\n", htm->trans_file, strerror(errno));
		return -1;
	}

	file_size = file_info.st_size;

	mapped_data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped_data == MAP_FAILED) {
		close(fd);
		pr_err("mmap failed on %s: %s\n", htm->trans_file, strerror(errno));
		return -1;
	}

	htmdata = mapped_data + 0x20;
	mem = (struct htm_mem *)htmdata;

	if (!mem || !htm->htm_mem_entries) {
		pr_info("No memory mapping entries captured in HTM translation\n");
		munmap(mapped_data, file_size);
		close(fd);
		return -1;
	}

	mem_entries_array = malloc(n_entries * sizeof(struct mem_entries));
	if (!mem_entries_array) {
		pr_err("Failed to allocate memory for mem entries: %s\n", strerror(errno));
		munmap(mapped_data, file_size);
		close(fd);
		return -ENOMEM;
	}

	/* get the HTM memory map data and store it in mem_entries_array
	 * to use it later on for physical->logical mapping
	 */
	for (u64 i = 0; i < n_entries; i++, mem++) {
		mem_entries_array[i].phy_addr = bswap_64(mem->phy_real);
		mem_entries_array[i].logical_addr = bswap_64(mem->logical_real);
		mem_entries_array[i].lp_index = bswap_32(mem->lp_index);
		mem_entries_array[i].size = bswap_64(mem->size);
	}

	munmap(mapped_data, file_size);
	close(fd);

	maps = process_trace_file(tracefile, mem_entries_array, n_entries, lp_filter, &num_maps);
	if (!maps) {
		pr_err("Error processing physical addresses from trace file\n");
		free(mem_entries_array);
		return -EINVAL;
	}

	for (size_t i = 0; i < num_maps; i++) {
		pr_debug("Event: %-20s | Phys: 0x%016llx | Logical: 0x%lx\n",
			maps[i].event,
			maps[i].phys_addr,
			(unsigned long)maps[i].logical_addr);
	}

	free(maps);
	free(mem_entries_array);

	return 0;
}

/*
 * Check if HTM events have more data to collect.
 *
 * This function reads the HTM event counts. When the kernel driver
 * has more data available, it returns a non-zero count. When all
 * data has been collected, it returns zero.
 *
 * Returns: 1 if more data exists, 0 if collection is complete
 */
int arch_perf_record__need_read(struct evlist *evlist)
{
	struct evsel *evsel;
	int found_htm = 0;

	/* there was an error during record__open */
	if (!evlist)
		return 0;

	/* First, check if any HTM events exist */
	evlist__for_each_entry(evlist, evsel) {
		if (strstr(evsel->name, "htm") != NULL)
			found_htm = 1;
	}

	if (!found_htm)
		return 0;

	/* Read HTM event counts to check if more data is available */
	evlist__for_each_entry(evlist, evsel) {
		struct xyarray *xy = evsel->core.sample_id;

		if (strstr(evsel->name, "htm") == NULL)
			continue;

		if (xy == NULL || evsel->core.fd == NULL)
			continue;
		if (xyarray__max_x(evsel->core.fd) != xyarray__max_x(xy) ||
			xyarray__max_y(evsel->core.fd) != xyarray__max_y(xy)) {
			pr_debug("Unmatched FD vs. sample ID: skip reading LOST count\n");
			continue;
		}

		for (int x = 0; x < xyarray__max_x(xy); x++) {
			for (int y = 0; y < xyarray__max_y(xy); y++) {
				struct perf_counts_values count;

				if (!strcmp(evsel->name, "dummy:u"))
					continue;

				if (strstr(evsel->name, "htm")) {
					perf_evsel__read(&evsel->core, x, y, &count);
					y = xyarray__max_y(xy);
					x = xyarray__max_x(xy);
				}
				if (!count.val)
					return 0;
			}
		}
	}

	return 1;
}

static void powerpc_htm_dump_event(size_t len)
{
	const char *color = PERF_COLOR_BLUE;

	if (dump_trace) {
		color_fprintf(stdout, color,
			". ... HTM PMU data: size %zu bytes\n",
			len);
	}
}

static int write_htm(void *data, size_t size, struct powerpc_htm *htm)
{
	FILE *fp;
	u64 *num_entries;
	size_t entries;
	size_t written;
	int ret = -1;

	if (htm->mem_maps) {
		fp = fopen(htm->trans_file, "ab");
		if (!fp) {
			pr_err("Failed to open %s: %s\n", htm->trans_file, strerror(errno));
			return ret;
		}
		num_entries = data + 0x10;
		entries = be64_to_cpu(*num_entries);
		entries++;
		written = fwrite(data, 32, entries, fp);
		if (written != entries) {
			pr_err("Failed to write data: expected %zu, wrote %zu\n", entries, written);
			fclose(fp);
			return ret;
		}
		fclose(fp);
		htm->htm_mem_entries += entries;
		return 0;
	}

	fp = fopen(htm->htmbin_file, "a");
	if (!fp) {
		pr_err("Failed to open %s: %s\n", htm->htmbin_file, strerror(errno));
		return ret;
	}
	written = fwrite(data, size, 1, fp);
	if (!written) {
		pr_err("Failed to htm trace data\n");
		fclose(fp);
		return ret;
	}
	fclose(fp);

	return 0;
}

static int powerpc_htm_process_event(struct perf_session *session __maybe_unused,
				 union perf_event *event __maybe_unused,
				 struct perf_sample *sample __maybe_unused,
				 const struct perf_tool *tool __maybe_unused)
{
	struct powerpc_htm *htm = container_of(session->auxtrace, struct powerpc_htm,
			auxtrace);

	if ((event->header.type == PERF_RECORD_SAMPLE) && sample->raw_data) {
		int *content = (int *)sample->raw_data;
		struct evsel *evsel = evlist__event2evsel(session->evlist, event);
		int config = (evsel->core.attr.config) & 0xF;
		struct auxtrace_buffer *buffer = NULL;
		struct auxtrace_queues *queues = &htm->queues;
		unsigned int i = 0;
		int j = 0;

		if (strstr(evsel->name, "htm") == NULL)
			return 0;

		for (i = 0; i < queues->nr_queues; i++) {
			buffer = auxtrace_buffer__next(&queues->queue_array[i], buffer);
			for (; buffer;) {
				if (j >= *content)
					htm->mem_maps = 1;
				if (write_htm(buffer->data, buffer->size, htm))
					return -1;
				j++;
				buffer = auxtrace_buffer__next(&queues->queue_array[i], buffer);
			}
		}
		/* Only for power bus traces, we decode traces */
		if (config == 1)
			create_mem_maps(htm);
	}

	return 0;
}

static int powerpc_htm_process_auxtrace_event(struct perf_session *session __maybe_unused,
					  union perf_event *event,
					  const struct perf_tool *tool __maybe_unused)
{
	powerpc_htm_dump_event(event->auxtrace.size);

	return 0;
}

static int powerpc_htm_flush(struct perf_session *session __maybe_unused,
			 const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void powerpc_htm_free_events(struct perf_session *session)
{
	struct powerpc_htm *htm = container_of(session->auxtrace, struct powerpc_htm,
					     auxtrace);
	struct auxtrace_queues *queues = &htm->queues;
	unsigned int i;

	for (i = 0; i < queues->nr_queues; i++)
		zfree(&queues->queue_array[i].priv);

	auxtrace_queues__free(queues);
}

static void powerpc_htm_free(struct perf_session *session)
{
	struct powerpc_htm *htm = container_of(session->auxtrace, struct powerpc_htm,
					     auxtrace);

	powerpc_htm_free_events(session);
	session->auxtrace = NULL;
	free(htm);
}
static const char * const powerpc_htm_info_fmts[] = {
	[POWERPC_HTM_TYPE]		= "  PMU Type           %"PRId64"\n",
};

static void powerpc_htm_print_info(__u64 *arr)
{
	if (!dump_trace)
		return;

	fprintf(stdout, powerpc_htm_info_fmts[POWERPC_HTM_TYPE], arr[POWERPC_HTM_TYPE]);
}

int powerpc_htm_process_auxtrace_info(union perf_event *event,
				  struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct evsel *evsel = evlist__event2evsel(session->evlist, event);
	u32 nodeindex, nodalchipindex, coreindexonchip;
	int config = (evsel->core.attr.config);
	size_t min_sz = sizeof(u64) * POWERPC_HTM_TYPE;
	struct powerpc_htm *htm;
	int err;
	FILE *fp;

	nodeindex = (config >> 4) & 0xff;
	nodalchipindex = (config >> 12) & 0xff;
	coreindexonchip = (config >> 20) & 0xff;

	if (auxtrace_info->header.size < sizeof(struct perf_record_auxtrace_info) +
					min_sz)
		return -EINVAL;

	htm = zalloc(sizeof(struct powerpc_htm));
	if (!htm)
		return -ENOMEM;

	err = auxtrace_queues__init(&htm->queues);
	if (err)
		goto err_free;

	htm->session = session;
	htm->machine = &session->machines.host; /* No kvm support */
	htm->auxtrace_type = auxtrace_info->type;
	htm->pmu_type = auxtrace_info->priv[POWERPC_HTM_TYPE];

	htm->auxtrace.process_event = powerpc_htm_process_event;
	htm->auxtrace.process_auxtrace_event = powerpc_htm_process_auxtrace_event;
	htm->auxtrace.flush_events = powerpc_htm_flush;
	htm->auxtrace.free_events = powerpc_htm_free_events;
	htm->auxtrace.free = powerpc_htm_free;
	session->auxtrace = &htm->auxtrace;

	snprintf(htm->htmbin_file, sizeof(htm->htmbin_file), "htm.bin.n%d.p%d.c%d", nodeindex, nodalchipindex, coreindexonchip);
	fp = fopen(htm->htmbin_file, "w");
	if (!fp) {
		pr_err("Failed to create %s: %s\n", htm->htmbin_file, strerror(errno));
		return -errno;
	}
	fclose(fp);

	snprintf(htm->trans_file, sizeof(htm->trans_file), "translation.n%d.p%d.c%d", nodeindex, nodalchipindex, coreindexonchip);
	fp = fopen(htm->trans_file, "w");
	if (!fp) {
		pr_err("Failed to create %s: %s\n", htm->trans_file, strerror(errno));
		return -errno;
	}
	fclose(fp);

	powerpc_htm_print_info(&auxtrace_info->priv[0]);

	err = auxtrace_queues__process_index(&htm->queues, session);
	if (err)
		goto err_free_queues;

	return 0;

err_free_queues:
	auxtrace_queues__free(&htm->queues);
	session->auxtrace = NULL;

err_free:
	free(htm);
	return err;
}
