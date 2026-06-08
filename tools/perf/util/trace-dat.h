/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2026, IBM Corporation
 * Author: Tanushree Shah <tshah@linux.ibm.com>
 */

#ifndef __PERF_TRACE_DAT_H
#define __PERF_TRACE_DAT_H

#include <stdio.h>

/* trace.dat file format version */
#define TRACE_DAT_VERSION '7'

/*
 * Section IDs for trace.dat format
 */
#define TRACE_DAT_SECTION_OPTIONS   0
#define TRACE_DAT_SECTION_FLYRECORD 3
#define TRACE_DAT_SECTION_STRINGS   15
#define TRACE_DAT_SECTION_HEADER    16
#define TRACE_DAT_SECTION_FTRACE    17
#define TRACE_DAT_SECTION_EVENTS    18
#define TRACE_DAT_SECTION_KALLSYMS  19
#define TRACE_DAT_SECTION_CMDLINE   21

/*
 * Option IDs for trace.dat options sections
 */
#define TRACE_DAT_OPTION_DONE       0
#define TRACE_DAT_OPTION_BUFFER     3
#define TRACE_DAT_OPTION_TRACECLOCK 4
#define TRACE_DAT_OPTION_CPUCOUNT   8
#define TRACE_DAT_OPTION_HEADER     16
#define TRACE_DAT_OPTION_FTRACE     17
#define TRACE_DAT_OPTION_EVENT      18
#define TRACE_DAT_OPTION_KALLSYMS   19
#define TRACE_DAT_OPTION_CMDLINE    21

/*
 * String offsets in the strings section
 * These point to null-terminated strings used as section names
 */
#define STRID_HEADERS          0
#define STRID_FTRACE_FORMATS   8
#define STRID_EVENT_FORMATS    29
#define STRID_KALLSYMS         43
#define STRID_CMDLINES         52
#define STRID_STRINGS          61
#define STRID_OPTIONS_1        69
#define STRID_OPTIONS_2        77
#define STRID_BUFFER_FLYRECORD 85

struct perf_session;

extern FILE *trace_dat_fp;
extern int trace_dat_page_size;
extern int trace_dat_nr_cpus;
extern long trace_dat_options_offset;
extern long trace_dat_header_info_offset;
extern long trace_dat_events_format_offset;
extern long trace_dat_ftrace_format_offset;
extern long trace_dat_kallsyms_offset;
extern long trace_dat_cmdline_offset;
extern long trace_dat_next_options_offset;

/* collect and manage per-cpu tracepoint event buffers */
int trace_dat__init_cpu_buffers(int nr_cpus);
int trace_dat__collect_cpu_event(int cpu, unsigned long long ts,
		       void *raw, unsigned int raw_size);
void trace_dat__free_cpu_buffers(void);

/* write trace.dat file sections */
int trace_dat__write_options_section1(void);
int trace_dat__write_options_section2(void);
int trace_dat__write_flyrecord_section(void);
int trace_dat__write_strings_section(void);

#endif /* __PERF_TRACE_DAT_H */
