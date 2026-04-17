// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2010 Felix Fietkau <nbd@openwrt.org>
 */
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/debugfs.h>
#include <linux/ieee80211.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <net/mac80211.h>
#include "rc80211_minstrel_ht.h"

struct minstrel_debugfs_info {
	size_t len;
	size_t size;
	char buf[];
};

#define MINSTREL_DEBUGFS_ROW_MAX	128

static size_t minstrel_debugfs_remaining(struct minstrel_debugfs_info *ms,
					 char *p)
{
	return ms->size - (p - ms->buf);
}

static void minstrel_dbg_append(struct minstrel_debugfs_info *ms, char **pp,
				const char *fmt, ...)
{
	size_t rem;
	va_list args;
	int len;

	rem = minstrel_debugfs_remaining(ms, *pp);
	if (!rem)
		return;

	va_start(args, fmt);
	len = vscnprintf(*pp, rem, fmt, args);
	va_end(args);

	*pp += len;
}

static void minstrel_dbg_putc(struct minstrel_debugfs_info *ms, char **pp,
			      char ch)
{
	if (minstrel_debugfs_remaining(ms, *pp) <= 1)
		return;

	*(*pp)++ = ch;
}

static struct minstrel_debugfs_info *
minstrel_debugfs_info_alloc(struct minstrel_ht_sta *mi)
{
	size_t rows = ARRAY_SIZE(mi->groups) * MCS_GROUP_RATES + 8;
	size_t buf_size = rows * MINSTREL_DEBUGFS_ROW_MAX;
	struct minstrel_debugfs_info *ms;

	ms = kvzalloc(sizeof(*ms) + buf_size, GFP_KERNEL);
	if (!ms)
		return NULL;
	ms->size = buf_size;
	return ms;
}

static ssize_t
minstrel_stats_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	struct minstrel_debugfs_info *ms;

	ms = file->private_data;
	return simple_read_from_buffer(buf, len, ppos, ms->buf, ms->len);
}

static int
minstrel_stats_release(struct inode *inode, struct file *file)
{
	kvfree(file->private_data);
	return 0;
}

static bool
minstrel_ht_is_sample_rate(struct minstrel_ht_sta *mi, int idx)
{
	int type, i;

	for (type = 0; type < ARRAY_SIZE(mi->sample); type++)
		for (i = 0; i < MINSTREL_SAMPLE_RATES; i++)
			if (mi->sample[type].cur_sample_rates[i] == idx)
				return true;
	return false;
}

static char *
minstrel_ht_stats_dump(struct minstrel_ht_sta *mi, int i,
		       struct minstrel_debugfs_info *ms, char *p)
{
	const struct mcs_group *mg;
	unsigned int j, tp_max, tp_avg, eprob, tx_time;
	char htmode = '2';
	char gimode = 'L';
	u32 gflags;

	if (!mi->supported[i])
		return p;

	mg = &minstrel_mcs_groups[i];
	gflags = mg->flags;

	if (gflags & IEEE80211_TX_RC_40_MHZ_WIDTH)
		htmode = '4';
	else if (gflags & IEEE80211_TX_RC_80_MHZ_WIDTH)
		htmode = '8';
	if (gflags & IEEE80211_TX_RC_SHORT_GI)
		gimode = 'S';

	for (j = 0; j < MCS_GROUP_RATES; j++) {
		struct minstrel_rate_stats *mrs = &mi->groups[i].rates[j];
		int idx = MI_RATE(i, j);
		unsigned int duration;

		if (!(mi->supported[i] & BIT(j)))
			continue;

		if (gflags & IEEE80211_TX_RC_MCS) {
			minstrel_dbg_append(ms, &p, "HT%c0  ", htmode);
			minstrel_dbg_append(ms, &p, "%cGI  ", gimode);
			minstrel_dbg_append(ms, &p, "%d  ", mg->streams);
		} else if (gflags & IEEE80211_TX_RC_VHT_MCS) {
			minstrel_dbg_append(ms, &p, "VHT%c0 ", htmode);
			minstrel_dbg_append(ms, &p, "%cGI ", gimode);
			minstrel_dbg_append(ms, &p, "%d  ", mg->streams);
		} else if (i == MINSTREL_OFDM_GROUP) {
			minstrel_dbg_append(ms, &p, "OFDM       ");
			minstrel_dbg_append(ms, &p, "1 ");
		} else {
			minstrel_dbg_append(ms, &p, "CCK    ");
			minstrel_dbg_append(ms, &p, "%cP  ",
					    j < 4 ? 'L' : 'S');
			minstrel_dbg_append(ms, &p, "1 ");
		}

		minstrel_dbg_putc(ms, &p,
				  (idx == mi->max_tp_rate[0]) ? 'A' : ' ');
		minstrel_dbg_putc(ms, &p,
				  (idx == mi->max_tp_rate[1]) ? 'B' : ' ');
		minstrel_dbg_putc(ms, &p,
				  (idx == mi->max_tp_rate[2]) ? 'C' : ' ');
		minstrel_dbg_putc(ms, &p,
				  (idx == mi->max_tp_rate[3]) ? 'D' : ' ');
		minstrel_dbg_putc(ms, &p,
				  (idx == mi->max_prob_rate) ? 'P' : ' ');
		minstrel_dbg_putc(ms, &p,
				  minstrel_ht_is_sample_rate(mi, idx) ? 'S' : ' ');

		if (gflags & IEEE80211_TX_RC_MCS) {
			minstrel_dbg_append(ms, &p, "  MCS%-2u",
					    (mg->streams - 1) * 8 + j);
		} else if (gflags & IEEE80211_TX_RC_VHT_MCS) {
			minstrel_dbg_append(ms, &p, "  MCS%-1u/%1u",
					    j, mg->streams);
		} else {
			int r;

			if (i == MINSTREL_OFDM_GROUP)
				r = minstrel_ofdm_bitrates[j % 8];
			else
				r = minstrel_cck_bitrates[j % 4];

			minstrel_dbg_append(ms, &p, "   %2u.%1uM", r / 10,
					    r % 10);
		}

		minstrel_dbg_append(ms, &p, "  %3u  ", idx);

		/* tx_time[rate(i)] in usec */
		duration = mg->duration[j];
		duration <<= mg->shift;
		tx_time = DIV_ROUND_CLOSEST(duration, 1000);
		minstrel_dbg_append(ms, &p, "%6u  ", tx_time);

		tp_max = minstrel_ht_get_tp_avg(mi, i, j, MINSTREL_FRAC(100, 100));
		tp_avg = minstrel_ht_get_tp_avg(mi, i, j, mrs->prob_avg);
		eprob = MINSTREL_TRUNC(mrs->prob_avg * 1000);

		minstrel_dbg_append(ms, &p,
				    "%4u.%1u    %4u.%1u     %3u.%1u     ",
				    tp_max / 10, tp_max % 10,
				    tp_avg / 10, tp_avg % 10,
				    eprob / 10, eprob % 10);
		minstrel_dbg_append(ms, &p, "%3u   %3u %-3u   ",
				    mrs->retry_count,
				    mrs->last_success,
				    mrs->last_attempts);
		minstrel_dbg_append(ms, &p, "%9llu   %-9llu\n",
				    (unsigned long long)mrs->succ_hist,
				    (unsigned long long)mrs->att_hist);
	}

	return p;
}

static int
minstrel_ht_stats_open(struct inode *inode, struct file *file)
{
	struct minstrel_ht_sta *mi = inode->i_private;
	struct minstrel_debugfs_info *ms;
	unsigned int i;
	char *p;

	ms = minstrel_debugfs_info_alloc(mi);
	if (!ms)
		return -ENOMEM;

	file->private_data = ms;
	p = ms->buf;

	minstrel_dbg_append(ms, &p, "\n");
	minstrel_dbg_append(ms, &p,
			    "              best    ____________rate__________    ____statistics___    _____last____    ______sum-of________\n");
	minstrel_dbg_append(ms, &p,
			    "mode guard #  rate   [name   idx airtime  max_tp]  [avg(tp) avg(prob)]  [retry|suc|att]  [#success | #attempts]\n");

	p = minstrel_ht_stats_dump(mi, MINSTREL_CCK_GROUP, ms, p);
	for (i = 0; i < MINSTREL_CCK_GROUP; i++)
		p = minstrel_ht_stats_dump(mi, i, ms, p);
	for (i++; i < ARRAY_SIZE(mi->groups); i++)
		p = minstrel_ht_stats_dump(mi, i, ms, p);

	minstrel_dbg_append(ms, &p, "\nTotal packet count::    ideal %d      ",
			    max(0, (int)mi->total_packets -
				(int)mi->sample_packets));
	minstrel_dbg_append(ms, &p, "lookaround %d\n", mi->sample_packets);
	if (mi->avg_ampdu_len)
		minstrel_dbg_append(ms, &p,
				    "Average # of aggregated frames per A-MPDU: %d.%d\n",
				    MINSTREL_TRUNC(mi->avg_ampdu_len),
				    MINSTREL_TRUNC(mi->avg_ampdu_len * 10) % 10);
	ms->len = p - ms->buf;

	return nonseekable_open(inode, file);
}

static const struct file_operations minstrel_ht_stat_fops = {
	.owner = THIS_MODULE,
	.open = minstrel_ht_stats_open,
	.read = minstrel_stats_read,
	.release = minstrel_stats_release,
};

static char *
minstrel_ht_stats_csv_dump(struct minstrel_ht_sta *mi, int i,
			   struct minstrel_debugfs_info *ms, char *p)
{
	const struct mcs_group *mg;
	unsigned int j, tp_max, tp_avg, eprob, tx_time;
	char htmode = '2';
	char gimode = 'L';
	u32 gflags;

	if (!mi->supported[i])
		return p;

	mg = &minstrel_mcs_groups[i];
	gflags = mg->flags;

	if (gflags & IEEE80211_TX_RC_40_MHZ_WIDTH)
		htmode = '4';
	else if (gflags & IEEE80211_TX_RC_80_MHZ_WIDTH)
		htmode = '8';
	if (gflags & IEEE80211_TX_RC_SHORT_GI)
		gimode = 'S';

	for (j = 0; j < MCS_GROUP_RATES; j++) {
		struct minstrel_rate_stats *mrs = &mi->groups[i].rates[j];
		int idx = MI_RATE(i, j);
		unsigned int duration;

		if (!(mi->supported[i] & BIT(j)))
			continue;

		if (gflags & IEEE80211_TX_RC_MCS) {
			minstrel_dbg_append(ms, &p, "HT%c0,", htmode);
			minstrel_dbg_append(ms, &p, "%cGI,", gimode);
			minstrel_dbg_append(ms, &p, "%d,", mg->streams);
		} else if (gflags & IEEE80211_TX_RC_VHT_MCS) {
			minstrel_dbg_append(ms, &p, "VHT%c0,", htmode);
			minstrel_dbg_append(ms, &p, "%cGI,", gimode);
			minstrel_dbg_append(ms, &p, "%d,", mg->streams);
		} else if (i == MINSTREL_OFDM_GROUP) {
			minstrel_dbg_append(ms, &p, "OFDM,,1,");
		} else {
			minstrel_dbg_append(ms, &p, "CCK,");
			minstrel_dbg_append(ms, &p, "%cP,", j < 4 ? 'L' : 'S');
			minstrel_dbg_append(ms, &p, "1,");
		}

		minstrel_dbg_append(ms, &p, "%s",
				    (idx == mi->max_tp_rate[0]) ? "A" : "");
		minstrel_dbg_append(ms, &p, "%s",
				    (idx == mi->max_tp_rate[1]) ? "B" : "");
		minstrel_dbg_append(ms, &p, "%s",
				    (idx == mi->max_tp_rate[2]) ? "C" : "");
		minstrel_dbg_append(ms, &p, "%s",
				    (idx == mi->max_tp_rate[3]) ? "D" : "");
		minstrel_dbg_append(ms, &p, "%s",
				    (idx == mi->max_prob_rate) ? "P" : "");
		minstrel_dbg_append(ms, &p, "%s",
				    minstrel_ht_is_sample_rate(mi, idx) ? "S" : "");

		if (gflags & IEEE80211_TX_RC_MCS) {
			minstrel_dbg_append(ms, &p, ",MCS%-2u,",
					    (mg->streams - 1) * 8 + j);
		} else if (gflags & IEEE80211_TX_RC_VHT_MCS) {
			minstrel_dbg_append(ms, &p, ",MCS%-1u/%1u,",
					    j, mg->streams);
		} else {
			int r;

			if (i == MINSTREL_OFDM_GROUP)
				r = minstrel_ofdm_bitrates[j % 8];
			else
				r = minstrel_cck_bitrates[j % 4];

			minstrel_dbg_append(ms, &p, ",%2u.%1uM,",
					    r / 10, r % 10);
		}

		minstrel_dbg_append(ms, &p, "%u,", idx);

		duration = mg->duration[j];
		duration <<= mg->shift;
		tx_time = DIV_ROUND_CLOSEST(duration, 1000);
		minstrel_dbg_append(ms, &p, "%u,", tx_time);

		tp_max = minstrel_ht_get_tp_avg(mi, i, j, MINSTREL_FRAC(100, 100));
		tp_avg = minstrel_ht_get_tp_avg(mi, i, j, mrs->prob_avg);
		eprob = MINSTREL_TRUNC(mrs->prob_avg * 1000);

		minstrel_dbg_append(ms, &p, "%u.%u,%u.%u,%u.%u,%u,%u,%u,",
				    tp_max / 10, tp_max % 10,
				    tp_avg / 10, tp_avg % 10,
				    eprob / 10, eprob % 10,
				    mrs->retry_count,
				    mrs->last_success,
				    mrs->last_attempts);
		minstrel_dbg_append(ms, &p, "%llu,%llu,",
				    (unsigned long long)mrs->succ_hist,
				    (unsigned long long)mrs->att_hist);
		minstrel_dbg_append(ms, &p, "%d,%d,%d.%d\n",
				    max(0, (int)mi->total_packets -
					(int)mi->sample_packets),
				    mi->sample_packets,
				    MINSTREL_TRUNC(mi->avg_ampdu_len),
				    MINSTREL_TRUNC(mi->avg_ampdu_len * 10) % 10);
	}

	return p;
}

static int
minstrel_ht_stats_csv_open(struct inode *inode, struct file *file)
{
	struct minstrel_ht_sta *mi = inode->i_private;
	struct minstrel_debugfs_info *ms;
	unsigned int i;
	char *p;

	ms = minstrel_debugfs_info_alloc(mi);
	if (!ms)
		return -ENOMEM;

	file->private_data = ms;

	p = ms->buf;

	p = minstrel_ht_stats_csv_dump(mi, MINSTREL_CCK_GROUP, ms, p);
	for (i = 0; i < MINSTREL_CCK_GROUP; i++)
		p = minstrel_ht_stats_csv_dump(mi, i, ms, p);
	for (i++; i < ARRAY_SIZE(mi->groups); i++)
		p = minstrel_ht_stats_csv_dump(mi, i, ms, p);

	ms->len = p - ms->buf;

	return nonseekable_open(inode, file);
}

static const struct file_operations minstrel_ht_stat_csv_fops = {
	.owner = THIS_MODULE,
	.open = minstrel_ht_stats_csv_open,
	.read = minstrel_stats_read,
	.release = minstrel_stats_release,
};

void
minstrel_ht_add_sta_debugfs(void *priv, void *priv_sta, struct dentry *dir)
{
	debugfs_create_file("rc_stats", 0444, dir, priv_sta,
			    &minstrel_ht_stat_fops);
	debugfs_create_file("rc_stats_csv", 0444, dir, priv_sta,
			    &minstrel_ht_stat_csv_fops);
}
