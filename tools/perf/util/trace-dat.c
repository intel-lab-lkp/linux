// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2026, IBM Corporation
 * Author: Tanushree Shah <tshah@linux.ibm.com>
 *
 * trace-dat.c
 *
 * This file implements the trace.dat format writer for perf tool.
 * It collects trace events from multiple CPUs and writes them in
 * the trace-cmd compatible format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "api/fs/tracing_path.h"
#include "trace-dat.h"
#include "trace-event.h"
#include "session.h"
#include "header.h"
#include "../perf.h"
#include "debug.h"

/* ftrace ring buffer constants for trace.dat flyrecord section
 *
 * Each page has a 16-byte header (timestamp + commit size), followed by
 * variable-length records. Each record has a 4-byte header word encoding:
 *   Bits 0-4:	 Type/Length field (5 bits, masked by TYPE_LEN_MASK)
 *   Bits 5-31:  Time delta from page base timestamp (27 bits, masked by TIME_MASK)
 */
#define TRACE_DAT_RECORD_HEADER_SIZE 16		/* Page header: 8-byte ts + 8-byte commit */
#define TRACE_DAT_RECORD_TYPE_LEN_MASK 0x1F		/* Extract lower 5 bits for type/length */
#define TRACE_DAT_RECORD_TIME_SHIFT	5		/* Shift to extract time delta */
#define TRACE_DAT_RECORD_TIME_MASK 0x07FFFFFF	/* Mask for 27-bit time delta */
#define TRACE_DAT_WORD_SIZE	4		/* Records aligned to 4-byte boundaries */
#define TRACE_DAT_WORD_ALIGN_MASK 3

/* Initial capacity for per-CPU event buffer (grows by doubling) */
#define INITIAL_EVENT_CAPACITY 1024
/* Initial capacity for page record array (grows by doubling) */
#define INITIAL_PAGE_RECORD_CAPACITY 64
/* Buffer size for reading trace_clock string from debugfs/tracefs */
#define CLOCK_BUFFER_SIZE 256

bool trace_dat_write_failed;
FILE *trace_dat_fp;
int trace_dat_page_size;
int trace_dat_nr_cpus;
long trace_dat_options_offset;
long trace_dat_header_info_offset;
long trace_dat_events_format_offset;
long trace_dat_ftrace_format_offset;
long trace_dat_kallsyms_offset;
long trace_dat_cmdline_offset;
long trace_dat_next_options_offset;


/**
 * struct cpu_event - Single trace event from a CPU
 * @ts: Timestamp of the event
 * @raw: Raw event data
 * @raw_size: Size of raw event data in bytes
 */
struct cpu_event {
	unsigned long long ts;
	void *raw;
	unsigned int raw_size;
};

/**
 * struct cpu_events - Collection of trace events for a single CPU
 * @events: Array of events
 * @count: Number of events currently stored
 * @capacity: Maximum number of events that can be stored
 */
struct cpu_events {
	struct cpu_event  *events;
	int count;
	int capacity;
};

static struct cpu_events *trace_cpu_data;
static long *buffer_opt_cpu_offsets_pos;
static long opt_payload_start;

/* Allocate per-cpu event buffers for tracepoint data collection */
int trace_dat__init_cpu_buffers(int nr_cpus)
{
	trace_cpu_data = calloc(nr_cpus, sizeof(struct cpu_events));
	if (!trace_cpu_data)
		return -ENOMEM;
	buffer_opt_cpu_offsets_pos = calloc(nr_cpus, sizeof(long));
	if (!buffer_opt_cpu_offsets_pos) {
		free(trace_cpu_data);
		trace_cpu_data = NULL;
		return -ENOMEM;
	}
	trace_dat_nr_cpus = nr_cpus;
	return 0;
}

/* Store raw tracepoint event data in per-cpu buffer for trace.dat
 * flyrecord
 */
int trace_dat__collect_cpu_event(int cpu, unsigned long long ts,
				 void *raw, unsigned int raw_size)
{
	struct cpu_events *cpu_events;
	void *raw_copy;

	if (!trace_cpu_data || cpu < 0 || cpu >= trace_dat_nr_cpus)
		return -EINVAL;

	if (!raw || raw_size == 0)
		return -EINVAL;

	cpu_events = &trace_cpu_data[cpu];

	if (cpu_events->count >= cpu_events->capacity) {
		int new_capacity = cpu_events->capacity ?
			cpu_events->capacity * 2 : INITIAL_EVENT_CAPACITY;
		struct cpu_event *tmp;

		tmp = realloc(cpu_events->events, new_capacity * sizeof(*cpu_events->events));
		if (!tmp)
			return -ENOMEM;
		cpu_events->events = tmp;
		cpu_events->capacity = new_capacity;
	}

	raw_copy = malloc(raw_size);
	if (!raw_copy)
		return -ENOMEM;

	memcpy(raw_copy, raw, raw_size);
	cpu_events->events[cpu_events->count].ts = ts;
	cpu_events->events[cpu_events->count].raw = raw_copy;

	cpu_events->events[cpu_events->count].raw_size = raw_size;
	cpu_events->count++;

	return 0;
}

/* Write a single page of trace records */
static int trace_dat__write_page(FILE *fp, struct tep_handle *pevent, unsigned long long base_ts,
			char **records, int *rec_sizes, int nr_recs)
{
	unsigned long long commit = 0;
	unsigned long long ts_out;
	unsigned long long commit_out;
	int offset = TRACE_DAT_RECORD_HEADER_SIZE;
	int i;
	char *page;

	page = calloc(1, trace_dat_page_size);
	if (!page)
		return -ENOMEM;

	for (i = 0; i < nr_recs; i++) {
		memcpy(page + offset, records[i], rec_sizes[i]);
		offset += rec_sizes[i];
		commit += rec_sizes[i];
	}

	/* Byte-swap page header for cross-arch compatibility */
	ts_out = to_file_u64(pevent, base_ts);
	commit_out = to_file_u64(pevent, commit);

	memcpy(page, &ts_out, sizeof(ts_out));
	memcpy(page + sizeof(ts_out), &commit_out, sizeof(commit_out));

	if (!fwrite(page, 1, trace_dat_page_size, fp)) {
		free(page);
		return -EIO;
	}
	free(page);

	return 0;
}

/* Write all trace data for a single CPU as trace.dat flyrecord pages */
static int trace_dat__write_cpu_dat(FILE *fp, struct tep_handle *pevent,
		int cpu, unsigned long long *file_offset_out)
{
	struct cpu_events *cpu_events = &trace_cpu_data[cpu];
	unsigned long long base_ts;
	unsigned long long file_offset;
	char **page_records = NULL;
	int *page_rec_sizes = NULL;
	int page_cap = 0;
	int nr_page_recs = 0;
	int page_size_used = 0;
	int ret = 0;
	int i, j;
	unsigned long long page_base_ts;

	file_offset = ftell(fp);
	*file_offset_out = file_offset;

	if (cpu_events->count == 0) {
		char *empty_page = calloc(1, trace_dat_page_size);

		if (!empty_page)
			return -ENOMEM;
		if (!fwrite(empty_page, 1, trace_dat_page_size, fp)) {
			free(empty_page);
			return -EIO;
		}
		free(empty_page);
		return 0;
	}

	base_ts = cpu_events->events[0].ts;
	page_base_ts = base_ts;

	for (i = 0; i < cpu_events->count; i++) {
		struct cpu_event *event = &cpu_events->events[i];
		unsigned long long time_delta = event->ts - base_ts;
		unsigned int data_len = event->raw_size;
		unsigned int words;
		unsigned int type_len;
		unsigned int payload_offset;
		unsigned int hdr_word;
		char *extend = NULL;
		int extend_size = 0;
		char *data_rec;
		int data_rec_size;
		int needed_size;

		/* Emit TIME_EXTEND when delta does not fit in 27 bits */
		if (time_delta > TRACE_DAT_RECORD_TIME_MASK) {
			unsigned int extend_hdr;
			unsigned int delta_upper;

			extend_size = TRACE_DAT_RECORD_TIME_EXTEND_SIZE;
			extend = calloc(1, extend_size);
			if (!extend) {
				ret = -ENOMEM;
				goto out_free;
			}

			if (tep_is_file_bigendian(pevent)) {
				extend_hdr = (time_delta & TRACE_DAT_RECORD_TIME_MASK) |
				     (TRACE_DAT_RECORD_TYPE_TIME_EXTEND << 27);
				delta_upper = time_delta >> 27;
			} else {
				extend_hdr = ((time_delta & TRACE_DAT_RECORD_TIME_MASK) <<
					TRACE_DAT_RECORD_TIME_SHIFT) |
					TRACE_DAT_RECORD_TYPE_TIME_EXTEND;
				delta_upper = time_delta >> 27;
			}
			extend_hdr  = to_file_u32(pevent, extend_hdr);   /* still needed */
			delta_upper = to_file_u32(pevent, delta_upper);   /* still needed */

			memcpy(extend, &extend_hdr, TRACE_DAT_WORD_SIZE);
			memcpy(extend + TRACE_DAT_WORD_SIZE, &delta_upper,
				TRACE_DAT_WORD_SIZE);

			time_delta = 0;
		}
		words = (data_len + TRACE_DAT_WORD_ALIGN_MASK) / TRACE_DAT_WORD_SIZE;
		/*
		 * Encode event length per ftrace ring buffer format:
		 * - Small events (≤28 words): type_len = word count
		 * - Large events (≥29 words): type_len = 0, followed by byte length
		 */
		if (words <= TRACE_DAT_RECORD_TYPE_LEN_MAX) {
			type_len = words;
			payload_offset = TRACE_DAT_WORD_SIZE;
			data_rec_size = TRACE_DAT_WORD_SIZE + data_len;
		} else {
			type_len = 0;
			payload_offset = 2 * TRACE_DAT_WORD_SIZE;
			data_rec_size = 2 * TRACE_DAT_WORD_SIZE + data_len;
		}

		if (data_rec_size % TRACE_DAT_WORD_SIZE)
			data_rec_size += TRACE_DAT_WORD_SIZE -
				(data_rec_size % TRACE_DAT_WORD_SIZE);

		/* Reject records that cannot fit in a single page payload */
		if (data_rec_size > trace_dat_page_size - TRACE_DAT_RECORD_HEADER_SIZE) {
			free(extend);
			ret = -E2BIG;
			goto out_free;
		}

		needed_size = extend_size + data_rec_size;

		/* Check page fit BEFORE allocating data record */
		if (page_size_used + needed_size >
			trace_dat_page_size - TRACE_DAT_RECORD_HEADER_SIZE) {
			ret = trace_dat__write_page(fp, pevent, page_base_ts,
					page_records, page_rec_sizes,
					nr_page_recs);

			for (j = 0; j < nr_page_recs; j++)
				free(page_records[j]);

			nr_page_recs = 0;
			page_size_used = 0;
			base_ts = event->ts;
			page_base_ts = event->ts;

			if (ret < 0) {
				free(extend);
				goto out_free;
			}

			/*
			 * New page base matches this event timestamp, so no
			 * TIME_EXTEND is needed anymore.
			 */
			free(extend);
			extend = NULL;
			extend_size = 0;
			time_delta = 0;
			base_ts = event->ts;
		}

		if (tep_is_file_bigendian(pevent))
			hdr_word = ((unsigned int)time_delta & TRACE_DAT_RECORD_TIME_MASK) |
				(type_len << 27);
		else
			hdr_word = ((unsigned int)time_delta << TRACE_DAT_RECORD_TIME_SHIFT) |
				type_len;
		hdr_word = to_file_u32(pevent, hdr_word);

		data_rec = calloc(1, data_rec_size);
		if (!data_rec) {
			free(extend);
			ret = -ENOMEM;
			goto out_free;
		}

		memcpy(data_rec, &hdr_word, TRACE_DAT_WORD_SIZE);

		/* Large events: write actual byte length after header */
		if (type_len == 0) {
			unsigned int data_len_out = to_file_u32(pevent, data_len);

			memcpy(data_rec + TRACE_DAT_WORD_SIZE, &data_len_out, TRACE_DAT_WORD_SIZE);
		}

		memcpy(data_rec + payload_offset, event->raw, data_len);

		if (nr_page_recs + (extend ? 1 : 0) >= page_cap) {
			char **new_records;
			int *new_sizes;
			int needed = nr_page_recs + (extend ? 1 : 0) + 1;
			int new_capacity = page_cap ? page_cap * 2 :
				INITIAL_PAGE_RECORD_CAPACITY;

			while (new_capacity < needed)
				new_capacity *= 2;

			new_records = malloc(new_capacity * sizeof(*new_records));
			new_sizes = malloc(new_capacity * sizeof(*new_sizes));
			if (!new_records || !new_sizes) {
				free(new_records);
				free(new_sizes);
				free(extend);
				free(data_rec);
				ret = -ENOMEM;
				goto out_free;
			}

			if (nr_page_recs > 0) {
				memcpy(new_records, page_records,
						nr_page_recs * sizeof(*new_records));
				memcpy(new_sizes, page_rec_sizes,
						nr_page_recs * sizeof(*new_sizes));
			}

			free(page_records);
			free(page_rec_sizes);

			page_records = new_records;
			page_rec_sizes = new_sizes;
			page_cap = new_capacity;
		}

		if (extend) {
			page_records[nr_page_recs] = extend;
			page_rec_sizes[nr_page_recs] = extend_size;
			nr_page_recs++;
			page_size_used += extend_size;
		}

		page_records[nr_page_recs] = data_rec;
		page_rec_sizes[nr_page_recs] = data_rec_size;
		nr_page_recs++;
		page_size_used += data_rec_size;
		base_ts = event->ts;
	}

	if (nr_page_recs > 0) {
		ret = trace_dat__write_page(fp, pevent, page_base_ts,
				page_records, page_rec_sizes, nr_page_recs);
	}
out_free:
	for (j = 0; j < nr_page_recs; j++)
		free(page_records[j]);
	free(page_records);
	free(page_rec_sizes);
	return ret;
}

/* Write the strings section containing section name lookup table */
int trace_dat__write_strings_section(struct tep_handle *pevent)
{
	unsigned long long section_size = 0;
	unsigned short section_id = to_file_u16(pevent, TRACE_DAT_SECTION_STRINGS);
	unsigned short flags = to_file_u16(pevent, 0);
	static const char * const section_names[] = {
		"headers",		/* offset 0 - strid for section 16  */
		"ftrace event formats", /* offset 8 - strid for section 17  */
		"events format",	/* offset 29 - strid for section 18  */
		"kallsyms",		/* offset 43 - strid for section 19  */
		"cmdlines",		/* offset 52 - strid for section 21  */
		"strings",		/* offset 61 - strid for section 15  */
		"options",		/* offset 69 - strid for options 1   */
		"options",		/* offset 77 - strid for options 2   */
		"buffer-flyrecord",	/* offset 85 - strid for flyrecord 3 */
		NULL
	};

	/* string_id points to "strings" string itself */
	unsigned int string_id = to_file_u32(pevent, STRID_STRINGS);
	int i;

	if (!trace_dat_fp)
		return -EBADF;

	for (i = 0; section_names[i] != NULL; i++)
		section_size += strlen(section_names[i]) + 1;
	section_size = to_file_u64(pevent, section_size);

	/* write section header */
	if (!fwrite(&section_id, sizeof(unsigned short), 1, trace_dat_fp) ||
		       !fwrite(&flags, sizeof(unsigned short), 1, trace_dat_fp) ||
		       !fwrite(&string_id, sizeof(unsigned int), 1, trace_dat_fp) ||
		       !fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp)) {
		pr_warning("Failed to write strings section\n");
		trace_dat_write_failed = true;
	}

	/* write strings */
	for (i = 0; section_names[i] != NULL; i++)
		if (!fwrite(section_names[i], 1, strlen(section_names[i]) + 1, trace_dat_fp)) {
			pr_warning("Failed to write strings section\n");
			trace_dat_write_failed = true;
		}
	return 0;
}

/* Writes options section containing CPUCOUNT, TRACECLOCK, EVENT_FORMAT, HEADER_INFO,
 * FTRACE_EVENTS, KALLSYMS, CMDLINES options, ending with DONE option pointing to next section.
 */
int trace_dat__write_options_section1(struct tep_handle *pevent)
{
	unsigned short section_id = to_file_u16(pevent, TRACE_DAT_SECTION_OPTIONS);
	unsigned short flags = to_file_u16(pevent, 0);
	unsigned int string_id = to_file_u32(pevent, STRID_OPTIONS_1);
	unsigned long long section_size = 0;
	long section_size_pos;
	long payload_start;
	unsigned long long section_start;
	unsigned short opt_id;
	unsigned int opt_size;
	unsigned int raw_opt_size;
	unsigned int nr_cpus;
	char clock_buf[CLOCK_BUFFER_SIZE];
	FILE *clock_file;
	size_t bytes_read;
	char *path;
	unsigned long long next_offset;
	unsigned long long events_off;
	unsigned long long header_off;
	unsigned long long ftrace_off;
	unsigned long long kallsyms_off;
	unsigned long long cmdline_off;
	long end_pos;

	if (!trace_dat_fp)
		return -EBADF;

	/* fill options_offset in initial format */
	section_start = to_file_u64(pevent, ftell(trace_dat_fp));

	if (fseek(trace_dat_fp, trace_dat_options_offset, SEEK_SET) < 0 ||
	   !fwrite(&section_start, sizeof(unsigned long long), 1, trace_dat_fp) ||
	   fseek(trace_dat_fp, 0, SEEK_END) < 0)
		return -EIO;

	/* write section header */
	if (!fwrite(&section_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&flags, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&string_id, sizeof(unsigned int), 1, trace_dat_fp))
		return -EIO;
	section_size_pos = ftell(trace_dat_fp);
	if (!fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp))
		return -EIO;

	payload_start = ftell(trace_dat_fp);

	/* CPUCOUNT option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_CPUCOUNT);
	opt_size = to_file_u32(pevent, sizeof(unsigned int));
	nr_cpus = to_file_u32(pevent, trace_dat_nr_cpus);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&nr_cpus, sizeof(unsigned int), 1, trace_dat_fp))
		return -EIO;

	/* TRACECLOCK option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_TRACECLOCK);

	path = get_tracing_file("trace_clock");
	if (path) {
		clock_file = fopen(path, "r");
		put_tracing_file(path);
	} else {
		clock_file = NULL;
	}
	if (clock_file) {
		bytes_read = fread(clock_buf, 1, sizeof(clock_buf) - 1, clock_file);
		fclose(clock_file);
		clock_buf[bytes_read] = '\0';
	} else {
		strcpy(clock_buf, "local\n");
		bytes_read = strlen(clock_buf);
	}
	raw_opt_size = bytes_read + 1;
	opt_size = to_file_u32(pevent, bytes_read + 1);
	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(clock_buf, 1, raw_opt_size, trace_dat_fp))
		return -EIO;

	/* EVENT option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_EVENT);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	events_off = to_file_u64(pevent, trace_dat_events_format_offset);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	   !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	   !fwrite(&events_off, sizeof(unsigned long long),
		   1, trace_dat_fp))
		return -EIO;

	/* HEADER option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_HEADER);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	header_off = to_file_u64(pevent, trace_dat_header_info_offset);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&header_off, sizeof(unsigned long long),
		    1, trace_dat_fp))
		return -EIO;

	/* FTRACE option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_FTRACE);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	ftrace_off = to_file_u64(pevent, trace_dat_ftrace_format_offset);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&ftrace_off, sizeof(unsigned long long),
		   1, trace_dat_fp))
		return -EIO;

	/* KALLSYMS option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_KALLSYMS);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	kallsyms_off = to_file_u64(pevent, trace_dat_kallsyms_offset);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&kallsyms_off, sizeof(unsigned long long),
		    1, trace_dat_fp))
		return -EIO;

	/* CMDLINE option */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_CMDLINE);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	cmdline_off = to_file_u64(pevent, trace_dat_cmdline_offset);

	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&cmdline_off, sizeof(unsigned long long),
		    1, trace_dat_fp))
		return -EIO;

	/* DONE option id - next_options_offset filled later */
	opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_DONE);
	opt_size = to_file_u32(pevent, sizeof(unsigned long long));
	next_offset = to_file_u64(pevent, 0);

	trace_dat_next_options_offset = ftell(trace_dat_fp);
	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&next_offset, sizeof(unsigned long long), 1, trace_dat_fp))
		return -EIO;

	/* fill section size */
	end_pos = ftell(trace_dat_fp);

	section_size = to_file_u64(pevent, end_pos - payload_start);
	if (fseek(trace_dat_fp, section_size_pos, SEEK_SET) < 0 ||
	    !fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp) ||
	    fseek(trace_dat_fp, end_pos, SEEK_SET) < 0)
		return -EIO;

	return 0;

}

/* Writes options section containing BUFFER option with flyrecord section
 * (flyrecord section offset, clock type, page size, CPU count,
 * per-CPU offsets/sizes) and DONE option.
 */
int trace_dat__write_options_section2(struct tep_handle *pevent)
{
	unsigned short section_id = to_file_u16(pevent, TRACE_DAT_SECTION_OPTIONS);
	unsigned short flags = to_file_u16(pevent, 0);
	unsigned int string_id = to_file_u32(pevent, STRID_OPTIONS_2);
	unsigned long long section_size = 0;
	long section_size_pos;
	long payload_start;
	int cpu;
	unsigned short opt_id = to_file_u16(pevent, TRACE_DAT_OPTION_BUFFER);
	unsigned int opt_size = 0;  /* filled after payload written */
	long opt_size_pos;
	unsigned long long data_offset = 0;
	unsigned int page_size = to_file_u32(pevent, (unsigned int)trace_dat_page_size);
	unsigned int nr_cpus   = to_file_u32(pevent, trace_dat_nr_cpus);
	const char *clock = "local";
	unsigned long long next;
	long end_pos;
	unsigned long long cpu_offset;
	unsigned long long cpu_size;
	unsigned short done_id;
	unsigned int done_size;

	if (!trace_dat_fp)
		return -EINVAL;

	/* fill done1 next offset - points to this section */
	next = to_file_u64(pevent, ftell(trace_dat_fp));

	if (fseek(trace_dat_fp, trace_dat_next_options_offset + 2 + 4, SEEK_SET) < 0 ||
	    !fwrite(&next, sizeof(unsigned long long), 1, trace_dat_fp) ||
	    fseek(trace_dat_fp, 0, SEEK_END) < 0)
		return -EIO;

	/* write section header */
	if (!fwrite(&section_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&flags, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&string_id, sizeof(unsigned int), 1, trace_dat_fp))
		return -EIO;
	section_size_pos = ftell(trace_dat_fp);
	if (!fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp))
		return -EIO;

	payload_start = ftell(trace_dat_fp);

	/* BUFFER option */
	if (!fwrite(&opt_id, sizeof(unsigned short), 1, trace_dat_fp))
		return -EIO;
	opt_size_pos = ftell(trace_dat_fp);
	if (!fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp))
		return -EIO;
	opt_payload_start = ftell(trace_dat_fp);

	/* data_offset placeholder */
	if (!fwrite(&data_offset, sizeof(unsigned long long), 1, trace_dat_fp) ||
	    !fwrite("\0", 1, 1, trace_dat_fp) ||
	    !fwrite(clock, 1, strlen(clock) + 1, trace_dat_fp) ||
	    !fwrite(&page_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&nr_cpus, sizeof(unsigned int), 1, trace_dat_fp))
		return -EIO;

	/* per cpu: cpu_id + offset placeholder + size */
	for (cpu = 0; cpu < trace_dat_nr_cpus; cpu++) {
		unsigned int cpu_out          = to_file_u32(pevent, cpu);
		cpu_offset = 0;  /* filled in write_flyrecord */
		cpu_size = 0;  /* filled in write_flyrecord */

		if (!fwrite(&cpu_out, sizeof(unsigned int), 1, trace_dat_fp))
			return -EIO;
		buffer_opt_cpu_offsets_pos[cpu] = ftell(trace_dat_fp);
		if (!fwrite(&cpu_offset, sizeof(unsigned long long), 1, trace_dat_fp) ||
		    !fwrite(&cpu_size, sizeof(unsigned long long), 1, trace_dat_fp))
			return -EIO;
	}

	/* fill opt_size */
	end_pos = ftell(trace_dat_fp);

	opt_size = to_file_u32(pevent, end_pos - opt_payload_start);
	if (fseek(trace_dat_fp, opt_size_pos, SEEK_SET) < 0 ||
	    !fwrite(&opt_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    fseek(trace_dat_fp, end_pos, SEEK_SET) < 0)
		return -EIO;

	/* DONE id=0 */
	done_id = to_file_u16(pevent, TRACE_DAT_OPTION_DONE);
	done_size = to_file_u32(pevent, sizeof(unsigned long long));
	/* No additional options sections follow this one */
	next = to_file_u64(pevent, 0);

	if (!fwrite(&done_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&done_size, sizeof(unsigned int), 1, trace_dat_fp) ||
	    !fwrite(&next, sizeof(unsigned long long), 1, trace_dat_fp))
		return -EIO;

	/* fill section size */
	end_pos = ftell(trace_dat_fp);

	section_size = to_file_u64(pevent, end_pos - payload_start);
	if (fseek(trace_dat_fp, section_size_pos, SEEK_SET) < 0 ||
	    !fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp) ||
	    fseek(trace_dat_fp, end_pos, SEEK_SET) < 0)
		return -EIO;

	return 0;

}

int trace_dat__write_flyrecord_section(struct tep_handle *pevent)
{
	unsigned short section_id = to_file_u16(pevent, TRACE_DAT_SECTION_FLYRECORD);
	unsigned short flags = to_file_u16(pevent, 0);
	unsigned int string_id = to_file_u32(pevent, STRID_BUFFER_FLYRECORD);
	unsigned long long section_size = 0;
	long section_size_pos;
	long flyrecord_start;
	unsigned long long flyrecord_out;
	long after_header;
	long padding_needed;
	unsigned long long *cpu_offsets;
	unsigned long long *cpu_sizes;
	int cpu;
	int ret = 0;
	char *pad;
	unsigned long long start;
	long end_pos;

	if (!trace_dat_fp)
		return -EINVAL;

	cpu_offsets = calloc(trace_dat_nr_cpus, sizeof(unsigned long long));
	cpu_sizes   = calloc(trace_dat_nr_cpus, sizeof(unsigned long long));
	if (!cpu_offsets || !cpu_sizes) {
		ret = -ENOMEM;
		goto cleanup;
	}
	flyrecord_start = ftell(trace_dat_fp);
	if (flyrecord_start < 0) {
		ret = -EIO;
		goto cleanup;
	}

	/* section header */
	if (!fwrite(&section_id, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&flags, sizeof(unsigned short), 1, trace_dat_fp) ||
	    !fwrite(&string_id, sizeof(unsigned int), 1, trace_dat_fp)) {
		ret = -EIO;
		goto cleanup;
	}
	section_size_pos = ftell(trace_dat_fp);
	if (!fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp)) {
		ret = -EIO;
		goto cleanup;
	}

	/* Align to page boundary */
	after_header   = ftell(trace_dat_fp);
	padding_needed = (trace_dat_page_size -
			 (after_header % trace_dat_page_size)) % trace_dat_page_size;

	if (padding_needed > 0) {
		pad = calloc(1, padding_needed);
		if (!pad) {
			ret = -ENOMEM;
			goto cleanup;
		}

		if (!fwrite(pad, 1, padding_needed, trace_dat_fp)) {
			free(pad);
			ret = -EIO;
			goto cleanup;
		}
		free(pad);
	}

	/* write per-cpu trace data */
	for (cpu = 0; cpu < trace_dat_nr_cpus; cpu++) {
		start = ftell(trace_dat_fp);

		ret = trace_dat__write_cpu_dat(trace_dat_fp, pevent, cpu, &cpu_offsets[cpu]);

		if (ret < 0) {
			pr_err("Failed to write CPU %d data\n", cpu);
			goto cleanup;
		}
		cpu_sizes[cpu]	 = ftell(trace_dat_fp) - start;
	}

	/* fill section size */
	end_pos = ftell(trace_dat_fp);

	section_size = to_file_u64(pevent, end_pos - after_header);
	if (fseek(trace_dat_fp, section_size_pos, SEEK_SET) < 0 ||
	    !fwrite(&section_size, sizeof(unsigned long long), 1, trace_dat_fp)) {
		ret = -EIO;
		goto cleanup;
	}
	if (fseek(trace_dat_fp, end_pos, SEEK_SET) < 0) {
		ret = -EIO;
		goto cleanup;
	}

	/* fill cpu offsets and sizes in BUFFER option */
	for (cpu = 0; cpu < trace_dat_nr_cpus; cpu++) {
		cpu_offsets[cpu] = to_file_u64(pevent, cpu_offsets[cpu]);
		cpu_sizes[cpu]   = to_file_u64(pevent, cpu_sizes[cpu]);
		if (fseek(trace_dat_fp, buffer_opt_cpu_offsets_pos[cpu], SEEK_SET) < 0 ||
		    !fwrite(&cpu_offsets[cpu], sizeof(unsigned long long), 1, trace_dat_fp) ||
		    !fwrite(&cpu_sizes[cpu], sizeof(unsigned long long), 1, trace_dat_fp)) {
			ret = -EIO;
			goto cleanup;
		}
	}

	/* fill data offset in buffer option */
	flyrecord_out = to_file_u64(pevent, flyrecord_start);
	if (fseek(trace_dat_fp, opt_payload_start, SEEK_SET) < 0 ||
	    !fwrite(&flyrecord_out, sizeof(unsigned long long), 1, trace_dat_fp)) {
		ret = -EIO;
		goto cleanup;
	}

	if (fseek(trace_dat_fp, 0, SEEK_END) < 0) {
		ret = -EIO;
		goto cleanup;
	}

cleanup:
	free(cpu_offsets);
	free(cpu_sizes);
	return ret;
}

/* Free all per-CPU event buffers */
void trace_dat__free_cpu_buffers(void)
{
	int cpu;

	if (!trace_cpu_data)
		return;

	for (cpu = 0; cpu < trace_dat_nr_cpus; cpu++) {
		int i;

		if (!trace_cpu_data[cpu].events)
			continue;

		for (i = 0; i < trace_cpu_data[cpu].count; i++)
			free(trace_cpu_data[cpu].events[i].raw);
		free(trace_cpu_data[cpu].events);
	}
	free(trace_cpu_data);
	trace_cpu_data = NULL;
	free(buffer_opt_cpu_offsets_pos);
	buffer_opt_cpu_offsets_pos = NULL;
	trace_dat_nr_cpus = 0;
}
