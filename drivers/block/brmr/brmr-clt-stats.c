// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

//#include <linux/module.h>
//#include <linux/blkdev.h>
//#include <linux/hdreg.h>

#include "brmr-clt.h"
#include "rmr.h"
#include "rmr-pool.h"


int brmr_clt_init_stats(struct brmr_clt_stats *stats)
{
	stats->pcpu_stats = alloc_percpu(typeof(*stats->pcpu_stats));
	if (unlikely(!stats->pcpu_stats))
		return -ENOMEM;

	return 0;
}

void brmr_clt_free_stats(struct brmr_clt_stats *stats)
{
	free_percpu(stats->pcpu_stats);
}

int brmr_clt_reset_submitted_req(struct brmr_clt_stats *stats, bool enable)
{
	struct brmr_stats_pcpu *s;
	int cpu;

	if (unlikely(!enable))
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		s = per_cpu_ptr(stats->pcpu_stats, cpu);
		memset(&s->submitted_requests, 0,
		       sizeof(s->submitted_requests));
	}

	return 0;
}

int brmr_clt_reset_req_sizes(struct brmr_clt_stats *stats, bool enable)
{
	struct brmr_stats_pcpu *s;
	int cpu;

	if (unlikely(!enable))
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		s = per_cpu_ptr(stats->pcpu_stats, cpu);
		memset(&s->request_sizes, 0,
		       sizeof(s->request_sizes));
	}

	return 0;
}

static void brmr_update_submitted_requests(struct brmr_stats_pcpu *s,
					   size_t size, int split, int d)
{
	s->submitted_requests.dir[d].total_sectors += (size >> SECTOR_SHIFT);
	if (split)
		s->submitted_requests.dir[d].cnt_split++;
	else
		s->submitted_requests.dir[d].cnt_whole++;
}

#define MAX_LEN (128*1024)
#define NUM_CLASSES 16
#define CLASSIFY_SHIFT (ilog2(MAX_LEN)-ilog2(NUM_CLASSES))

/**
   classifies length linearly in 16 classes:

   input length in bytes
   
   <    0x2000 (8K)
   >=   0x2000 (8K)
   >=   0x4000 (16K)
   >=   0x6000 (24K)
   >=   0x8000 (32K)
   >=   0xa000 (40K)
   >=   0xc000 (48K)
   >=   0xe000 (56K)
   >=  0x10000 (64K)
   >=  0x12000 (72K)
   >=  0x14000 (80K)
   >=  0x16000 (88K)
   >=  0x18000 (96K)
   >=  0x1a000 (104K)
   >=  0x1c000 (112K)
   >=  0x1e000 (120K)

   Maximum value is 128K-1.
   However everything larger is classified as class 15 as well.
*/
static inline int classify(long length)
{
	return length < MAX_LEN ? (length >> CLASSIFY_SHIFT) : NUM_CLASSES-1;
}

static void brmr_update_request_sizes(struct brmr_stats_pcpu *s,
				      size_t size, int split, int d)
{
	int size_class = classify(size);
	switch (split) {
	case 0:
		s->request_sizes.dir[d].cnt_whole[size_class]++;
		break;
	case 1:
		s->request_sizes.dir[d].cnt_left[size_class]++;
		break;
	case 2:
		s->request_sizes.dir[d].cnt_right[size_class]++;
		break;
	default:
		WARN_ONCE(true,"unexpected value for split");
	}
}

void brmr_update_stats(struct brmr_clt_stats *stats, size_t size, int split, int d)
{
	struct brmr_stats_pcpu *s;

	s = this_cpu_ptr(stats->pcpu_stats);

	brmr_update_submitted_requests(s, size, split, d);
	brmr_update_request_sizes(s, size, split, d);
}

ssize_t brmr_clt_stats_rq_to_str(struct brmr_clt_stats *stats, char *page, size_t len)
{
	struct brmr_stats_rq sum;
	struct brmr_stats_rq *r;
	int cpu; int d;

	memset(&sum, 0, sizeof(sum));

	for_each_possible_cpu(cpu) {
		r = &per_cpu_ptr(stats->pcpu_stats, cpu)->submitted_requests;

		for (d=READ; d<=WRITE; d++) {
			sum.dir[d].cnt_whole      += r->dir[d].cnt_whole;
			sum.dir[d].cnt_split      += r->dir[d].cnt_split;
			sum.dir[d].total_sectors  += r->dir[d].total_sectors;
		}
	}

	return scnprintf(page, len, "%llu %llu %llu %llu %llu %llu\n",
			 sum.dir[READ].cnt_whole, sum.dir[READ].cnt_split,
			 sum.dir[READ].total_sectors,
			 sum.dir[WRITE].cnt_whole, sum.dir[WRITE].cnt_split,
			 sum.dir[WRITE].total_sectors);
}

ssize_t brmr_clt_stats_sizes_to_str(struct brmr_clt_stats *stats, char *page, size_t len)
{
	struct brmr_stats_sizes *sum;
	struct brmr_stats_sizes *per_cpu;
	int cpu; int d; int i; int cnt = 0;

	sum = kzalloc(sizeof(*sum), GFP_KERNEL);
	if (unlikely(!sum))
		return -ENOMEM;

	for (i = 0; i < STATS_SIZES_NUM; i++) {
		for_each_possible_cpu(cpu) {
			per_cpu = &per_cpu_ptr(stats->pcpu_stats, cpu)
				->request_sizes;

			for (d=READ; d<=WRITE; d++) {
				sum->dir[d].cnt_whole[i]
					+= per_cpu->dir[d].cnt_whole[i];
				sum->dir[d].cnt_left[i]
					+= per_cpu->dir[d].cnt_left[i];
				sum->dir[d].cnt_right[i]
					+= per_cpu->dir[d].cnt_right[i];
			}
		}
	}

	cnt += scnprintf(page + cnt, len - cnt,
		"         READ        "
		"        whole                left               right               "
		"\n");
	if (len - cnt <= 0)
		goto free_return;

	cnt += scnprintf(page + cnt, len - cnt,
			 "<=   8 Kbytes: %19llu %19llu %19llu\n",
			 sum->dir[READ].cnt_whole[0],
			 sum->dir[READ].cnt_left[0],
			 sum->dir[READ].cnt_right[0]);

	for (i = 1; i < STATS_SIZES_NUM; i++) {

		cnt += scnprintf(page + cnt, len - cnt,
				 ">  %3d Kbytes: %19llu %19llu %19llu\n",
				 (i)<<3,
				 sum->dir[READ].cnt_whole[i],
				 sum->dir[READ].cnt_left[i],
				 sum->dir[READ].cnt_right[i]);

		if (len - cnt <= 0)
			goto free_return;
	}

	cnt += scnprintf(page + cnt, len - cnt,
		"\n        WRITE        "
		"        whole                left               right               "
		"\n");
	if (len - cnt <= 0)
		goto free_return;

	cnt += scnprintf(page + cnt, len - cnt,
			 "<=   8 Kbytes: %19llu %19llu %19llu\n",
			 sum->dir[WRITE].cnt_whole[0],
			 sum->dir[WRITE].cnt_left[0],
			 sum->dir[WRITE].cnt_right[0]);

	for (i = 1; i < STATS_SIZES_NUM; i++) {

		cnt += scnprintf(page + cnt, len - cnt,
				 ">  %3d Kbytes: %19llu %19llu %19llu\n",
				 (i)<<3,
				 sum->dir[WRITE].cnt_whole[i],
				 sum->dir[WRITE].cnt_left[i],
				 sum->dir[WRITE].cnt_right[i]);

		if (len - cnt <= 0)
			goto free_return;
	}

free_return:
	kfree(sum);

	return cnt;
}

int brmr_clt_reset_sts_resource(struct brmr_clt_stats *stats, bool enable)
{
	struct brmr_stats_pcpu *s;
	int cpu;

	if (unlikely(!enable))
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		s = per_cpu_ptr(stats->pcpu_stats, cpu);
		memset(&s->sts_resource, 0,
		       sizeof(s->sts_resource));
	}

	return 0;
}

void brmr_clt_update_sts_resource(struct brmr_clt_stats *stats, int which)
{
	struct brmr_stats_pcpu *s;

	s = this_cpu_ptr(stats->pcpu_stats);
	switch (which) {
	case 0:
		s->sts_resource.get_iu++;
		break;
	case 1:
		s->sts_resource.get_iu2++;
		break;
	case 2:
		s->sts_resource.clt_request1++;
		break;
	case 3:
		s->sts_resource.clt_request++;
		break;
	default:
		WARN_ONCE(true,"unexpected value for which");
	}
}

ssize_t brmr_stats_sts_resource_to_str(
	struct brmr_clt_stats *stats, char *page, size_t len)
{
	struct brmr_stats_sts_resource sum;
	struct brmr_stats_sts_resource *r;
	int cpu;

	memset(&sum, 0, sizeof(sum));

	for_each_possible_cpu(cpu) {
		r = &per_cpu_ptr(stats->pcpu_stats, cpu)->sts_resource;

		sum.get_iu       += r->get_iu;
		sum.get_iu2      += r->get_iu2;
		sum.clt_request1 += r->clt_request1;
		sum.clt_request  += r->clt_request;
	}

	return scnprintf(page, len, "%llu %llu %llu %llu\n",
			 sum.get_iu, sum.get_iu2,
			 sum.clt_request1, sum.clt_request);
}

ssize_t brmr_stats_sts_resource_per_cpu_to_str(
	struct brmr_clt_stats *stats, char *page, size_t len)
{
	struct brmr_stats_sts_resource *r;
	int cpu; int cnt = 0;

	for_each_possible_cpu(cpu) {
		r = &per_cpu_ptr(stats->pcpu_stats, cpu)->sts_resource;

		cnt += scnprintf(page+cnt, len, "%d %llu %llu %llu %llu\n",
				 cpu, r->get_iu, r->get_iu2,
				 r->clt_request1, r->clt_request);
		if (len - cnt <= 0)
			goto return_cnt;
	}

return_cnt:
	return cnt;
}

