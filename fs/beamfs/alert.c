// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- threshold alerting
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Everything beamfs knows about its own health is already recorded:
 * dmesg carries each uncorrectable, the RS journal holds coordinates,
 * the error budget holds remaining margin. All of it has to be looked
 * at, and on an unattended system nobody looks.
 *
 * This notifies. A uevent on the filesystem's kobject when a threshold
 * is crossed, which udev, systemd or a bare daemon can act on. The
 * kernel says a line has been crossed and stops there; deciding what
 * that means -- page an operator, schedule a copy, ground the aircraft
 * -- is policy, and policy does not belong in a filesystem.
 *
 * Three thresholds, each answering a different question:
 *
 *   a block reached zero margin      is this volume about to lose data
 *   uncorrectables in an interval    is it losing data now
 *   blocks at zero margin            how much of it is at the edge
 *
 * Rate-limited per event class, because the failure mode this must not
 * have is a volume degrading fast enough to bury the operator in
 * notifications about it.
 *
 * No overlap with the report side: this signals that something
 * happened, and the RS journal, read by the userspace decoder, says
 * what. A notification carrying its own diagnosis would duplicate the
 * decoder and drift from it.
 */

#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/jiffies.h>
#include "beamfs.h"

/* Minimum gap between two notifications of the same class. */
#define BEAMFS_ALERT_INTERVAL_MS 60000

/*
 * beamfs_alert -- emit one, if the class has not fired recently.
 *
 * @class: index into the per-class rate limiter
 * @reason: short token, the thing a udev rule matches on
 * @detail: what was seen, for a human reading the log
 */
static void beamfs_alert(struct super_block *sb, unsigned int class,
			 const char *reason, const char *detail)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	char *envp[4];
	char buf_reason[64];
	char buf_detail[96];
	unsigned long now = jiffies;

	if (class >= BEAMFS_ALERT_CLASSES)
		return;
	if (!sbi->s_kobj.state_initialized)
		return;

	/*
	 * time_before handles wraparound; a zero last_alert means never,
	 * and jiffies zero at boot would otherwise suppress the first
	 * alert of the machine's life.
	 */
	if (sbi->s_alert_last[class] &&
	    time_before(now, sbi->s_alert_last[class] +
			msecs_to_jiffies(BEAMFS_ALERT_INTERVAL_MS)))
		return;
	sbi->s_alert_last[class] = now ? now : 1;

	snprintf(buf_reason, sizeof(buf_reason), "BEAMFS_REASON=%s", reason);
	snprintf(buf_detail, sizeof(buf_detail), "BEAMFS_DETAIL=%s", detail);

	envp[0] = buf_reason;
	envp[1] = buf_detail;
	envp[2] = "BEAMFS_ALERT=1";
	envp[3] = NULL;

	kobject_uevent_env(&sbi->s_kobj, KOBJ_CHANGE, envp);

	pr_warn("beamfs: alert %s: %s\n", reason, detail);
}

/*
 * beamfs_alert_uncorrectable -- a block could not be recovered.
 *
 * The most serious of the three: data has been lost, not merely put at
 * risk. Fires on the first one and then no more often than the
 * interval, since a volume shedding blocks would otherwise bury the
 * operator in notices about it.
 */
void beamfs_alert_uncorrectable(struct super_block *sb, u64 phys)
{
	char detail[96];

	snprintf(detail, sizeof(detail), "block %llu unrecoverable",
		 (unsigned long long)phys);
	beamfs_alert(sb, BEAMFS_ALERT_UNCORRECTABLE, "uncorrectable", detail);
}

/*
 * beamfs_alert_margin -- a block has no correction capacity left.
 *
 * It still reads correctly. The next upset in the wrong subblock does
 * not, and this is the only warning that will come before that read.
 * Which is the whole argument for the error budget: without it there
 * is nothing to notify about until the data is already gone.
 */
void beamfs_alert_margin(struct super_block *sb, u64 phys, unsigned int used)
{
	char detail[96];

	if (used < BEAMFS_ERROR_BUDGET_MAX)
		return;

	snprintf(detail, sizeof(detail),
		 "block %llu has consumed all %u correctable symbols",
		 (unsigned long long)phys, (unsigned int)BEAMFS_ERROR_BUDGET_MAX);
	beamfs_alert(sb, BEAMFS_ALERT_MARGIN, "no-margin", detail);
}

/*
 * beamfs_alert_check_rate -- has correction activity crossed the line?
 *
 * Called by the scrubber at the end of a sweep, where a whole-volume
 * figure exists. s_alert_rate_limit is the number of corrections per
 * sweep an operator is willing to see; zero disables the check, which
 * is the default because the right number depends on the deployment
 * and inventing one would produce either noise or silence.
 */
void beamfs_alert_check_rate(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 delta;
	char detail[96];

	if (!sbi->s_alert_rate_limit)
		return;

	delta = sbi->s_scrub_corrected - sbi->s_alert_last_corrected;
	sbi->s_alert_last_corrected = sbi->s_scrub_corrected;

	if (delta < sbi->s_alert_rate_limit)
		return;

	snprintf(detail, sizeof(detail),
		 "%llu blocks corrected in one sweep, limit %u",
		 delta, sbi->s_alert_rate_limit);
	beamfs_alert(sb, BEAMFS_ALERT_RATE, "correction-rate", detail);
}
