// SPDX-License-Identifier: GPL-2.0
/*
 * CXL HMU support
 * Copyright (c) 2024 Huawei
 *
 * Based on:
 * HiSilicon PCIe Trace and Tuning (PTT) support
 * Copyright (c) 2022 HiSilicon Technologies Co., Ltd.
 */

#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/types.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <unistd.h>

#include "auxtrace.h"
#include "color.h"
#include "debug.h"
#include "evlist.h"
#include "evsel.h"
#include "cxl-hmu.h"
#include "machine.h"
#include "record.h"
#include "session.h"
#include "tool.h"
#include "tsc.h"
#include <internal/lib.h>

#define KiB(x) ((x) * 1024)
#define MiB(x) ((x) * 1024 * 1024)

struct chmu_recording {
	struct auxtrace_record	itr;
	struct perf_pmu *chmu_pmu;
	struct evlist *evlist;
};

static size_t
chmu_info_priv_size(struct auxtrace_record *itr __maybe_unused,
			struct evlist *evlist __maybe_unused)
{
	return CXL_HMU_AUXTRACE_PRIV_SIZE;
}

static int chmu_info_fill(struct auxtrace_record *itr,
			      struct perf_session *session,
			      struct perf_record_auxtrace_info *auxtrace_info,
			      size_t priv_size)
{
	struct chmu_recording *pttr =
			container_of(itr, struct chmu_recording, itr);
	struct perf_pmu *chmu_pmu = pttr->chmu_pmu;

	if (priv_size != CXL_HMU_AUXTRACE_PRIV_SIZE)
		return -EINVAL;

	if (!session->evlist->core.nr_mmaps)
		return -EINVAL;

	auxtrace_info->type = PERF_AUXTRACE_CXL_HMU;
	auxtrace_info->priv[0] = chmu_pmu->type;

	return 0;
}

static int chmu_set_auxtrace_mmap_page(struct record_opts *opts)
{
	bool privileged = perf_event_paranoid_check(-1);

	if (!opts->full_auxtrace)
		return 0;

	if (opts->full_auxtrace && !opts->auxtrace_mmap_pages) {
		if (privileged) {
			opts->auxtrace_mmap_pages = MiB(16) / page_size;
		} else {
			opts->auxtrace_mmap_pages = KiB(128) / page_size;
			if (opts->mmap_pages == UINT_MAX)
				opts->mmap_pages = KiB(256) / page_size;
		}
	}

	/* Validate auxtrace_mmap_pages */
	if (opts->auxtrace_mmap_pages) {
		size_t sz = opts->auxtrace_mmap_pages * (size_t)page_size;
		size_t min_sz = KiB(8);

		if (sz < min_sz || !is_power_of_2(sz)) {
			pr_err("Invalid mmap size for CXL_HMU: must be at least %zuKiB and a power of 2\n",
			       min_sz / 1024);
			return -EINVAL;
		}
	}

	return 0;
}

static int chmu_recording_options(struct auxtrace_record *itr,
				      struct evlist *evlist,
				      struct record_opts *opts)
{
	struct chmu_recording *pttr =
			container_of(itr, struct chmu_recording, itr);
	struct perf_pmu *chmu_pmu = pttr->chmu_pmu;
	struct evsel *evsel, *chmu_evsel = NULL;
	struct evsel *tracking_evsel;
	int err;

	pttr->evlist = evlist;
	evlist__for_each_entry(evlist, evsel) {
		if (evsel->core.attr.type == chmu_pmu->type) {
			if (chmu_evsel) {
				pr_err("There may be only one cxl_hmu x event\n");
				return -EINVAL;
			}
			evsel->core.attr.freq = 0;
			evsel->core.attr.sample_period = 1;
			evsel->needs_auxtrace_mmap = true;
			chmu_evsel = evsel;
			opts->full_auxtrace = true;
		}
	}

	err = chmu_set_auxtrace_mmap_page(opts);
	if (err)
		return err;
	/*
	 * To obtain the auxtrace buffer file descriptor, the auxtrace event
	 * must come first.
	 */
	evlist__to_front(evlist, chmu_evsel);
	evsel__set_sample_bit(chmu_evsel, TIME);

	/* Add dummy event to keep tracking */
	err = parse_event(evlist, "dummy:u");
	if (err)
		return err;

	tracking_evsel = evlist__last(evlist);
	evlist__set_tracking_event(evlist, tracking_evsel);

	tracking_evsel->core.attr.freq = 0;
	tracking_evsel->core.attr.sample_period = 1;
	evsel__set_sample_bit(tracking_evsel, TIME);

	return 0;
}

static u64 chmu_reference(struct auxtrace_record *itr __maybe_unused)
{
	return rdtsc();
}

static void chmu_recording_free(struct auxtrace_record *itr)
{
	struct chmu_recording *pttr =
	  container_of(itr, struct chmu_recording, itr);

	free(pttr);
}

struct auxtrace_record *chmu_recording_init(int *err,
						struct perf_pmu *chmu_pmu)
{
	struct chmu_recording *pttr;

	if (!chmu_pmu) {
		*err = -ENODEV;
		return NULL;
	}

	pttr = zalloc(sizeof(*pttr));
	if (!pttr) {
		*err = -ENOMEM;
		return NULL;
	}

	pttr->chmu_pmu = chmu_pmu;
	pttr->itr.recording_options = chmu_recording_options;
	pttr->itr.info_priv_size = chmu_info_priv_size;
	pttr->itr.info_fill = chmu_info_fill;
	pttr->itr.free = chmu_recording_free;
	pttr->itr.reference = chmu_reference;
	pttr->itr.read_finish = auxtrace_record__read_finish;
	pttr->itr.alignment = 0;

	*err = 0;
	return &pttr->itr;
}

struct cxl_hmu {
	struct auxtrace auxtrace;
	u32 auxtrace_type;
	struct perf_session *session;
	struct machine *machine;
	u32 pmu_type;
};

struct cxl_hmu_queue {
	struct cxl_hmu *hmu;
	struct auxtrace_buffer *buffer;
};

static void cxl_hmu_dump(struct cxl_hmu *hmu __maybe_unused,
			  unsigned char *buf, size_t len)
{
	const char *color = PERF_COLOR_BLUE;
	size_t pos = 0;
	size_t packet_offset = 0, hotlist_entries_in_packet;

	len = round_down(len, 8);
	color_fprintf(stdout, color, ". ... CXL_HMU data: size %zu bytes\n",
		      len);

	while (len > 0) {
		if (!packet_offset) {
			hotlist_entries_in_packet = ((uint64_t *)(buf + pos))[0] & 0xFFFF;
			color_fprintf(stdout, PERF_COLOR_BLUE,
				      "Header 0: units: %x counter_width %x\n",
				      hotlist_entries_in_packet,
				      (((uint64_t *)(buf + pos))[0] >> 16) & 0xFF);
		} else if (packet_offset == 1) {
			color_fprintf(stdout, PERF_COLOR_BLUE,
				      "Header 1 : %lx\n", ((uint64_t *)(buf + pos))[0]);
		} else {
			color_fprintf(stdout, PERF_COLOR_BLUE,
				      "%016lx\n", ((uint64_t *)(buf + pos))[0]);
		}
		pos += 8;
		len -= 8;
		packet_offset++;
		if (packet_offset == hotlist_entries_in_packet + 2)
			packet_offset = 0;
	}
}

static void cxl_hmu_dump_event(struct cxl_hmu *hmu, unsigned char *buf,
			       size_t len)
{
	printf(".\n");

	cxl_hmu_dump(hmu, buf, len);
}

static int cxl_hmu_process_event(struct perf_session *session __maybe_unused,
				  union perf_event *event __maybe_unused,
				  struct perf_sample *sample __maybe_unused,
				  const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static int cxl_hmu_process_auxtrace_event(struct perf_session *session,
					  union perf_event *event,
					  const struct perf_tool *tool __maybe_unused)
{
	struct cxl_hmu *hmu = container_of(session->auxtrace, struct cxl_hmu,
					    auxtrace);
	int fd = perf_data__fd(session->data);
	int size = event->auxtrace.size;
	void *data = malloc(size);
	off_t data_offset;
	int err;

	if (!data) {
		printf("no data\n");
		return -errno;
	}

	if (perf_data__is_pipe(session->data)) {
		data_offset = 0;
	} else {
		data_offset = lseek(fd, 0, SEEK_CUR);
		if (data_offset == -1) {
			free(data);
			printf("failed to seek\n");
			return -errno;
		}
	}

	err = readn(fd, data, size);
	if (err != (ssize_t)size) {
		free(data);
		printf("failed to rread\n");
		return -errno;
	}

	if (dump_trace)
		cxl_hmu_dump_event(hmu, data, size);

	free(data);
	return 0;
}

static int cxl_hmu_flush(struct perf_session *session __maybe_unused,
			 const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void cxl_hmu_free_events(struct perf_session *session __maybe_unused)
{
}

static void cxl_hmu_free(struct perf_session *session)
{
	struct cxl_hmu *hmu = container_of(session->auxtrace, struct cxl_hmu,
					    auxtrace);

	session->auxtrace = NULL;
	free(hmu);
}

static bool cxl_hmu_evsel_is_auxtrace(struct perf_session *session,
				       struct evsel *evsel)
{
	struct cxl_hmu *hmu = container_of(session->auxtrace, struct cxl_hmu, auxtrace);

	return evsel->core.attr.type == hmu->pmu_type;
}

static void cxl_hmu_print_info(__u64 type)
{
	if (!dump_trace)
		return;

	fprintf(stdout, "  PMU Type           %" PRId64 "\n", (s64) type);
}

int cxl_hmu_process_auxtrace_info(union perf_event *event,
				   struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct cxl_hmu *hmu;

	if (auxtrace_info->header.size < CXL_HMU_AUXTRACE_PRIV_SIZE +
				sizeof(struct perf_record_auxtrace_info))
		return -EINVAL;

	hmu = zalloc(sizeof(*hmu));
	if (!hmu)
		return -ENOMEM;

	hmu->session = session;
	hmu->machine = &session->machines.host; /* No kvm support */
	hmu->auxtrace_type = auxtrace_info->type;
	hmu->pmu_type = auxtrace_info->priv[0];

	hmu->auxtrace.process_event = cxl_hmu_process_event;
	hmu->auxtrace.process_auxtrace_event = cxl_hmu_process_auxtrace_event;
	hmu->auxtrace.flush_events = cxl_hmu_flush;
	hmu->auxtrace.free_events = cxl_hmu_free_events;
	hmu->auxtrace.free = cxl_hmu_free;
	hmu->auxtrace.evsel_is_auxtrace = cxl_hmu_evsel_is_auxtrace;
	session->auxtrace = &hmu->auxtrace;

	cxl_hmu_print_info(auxtrace_info->priv[0]);

	return 0;
}
