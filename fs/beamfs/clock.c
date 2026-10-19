// SPDX-License-Identifier: GPL-2.0-only
/*
 * beamfs -- clock anchor for the radiation event journal
 *
 * Author: Aurelien DESBRIERES <aurelien@hackers.camp>
 *
 * Journal entries carry ktime_get_ns(): monotonic since boot, which
 * orders events exactly and dates none of them. That is the right
 * clock for an entry -- it cannot jump, so the interval between two
 * corrections is good to the timer's resolution, and a burst of forty
 * upsets inside three milliseconds reads as exactly that.
 *
 * What it cannot say is when. This supplies the other half: what the
 * wall clock read at a known point on the monotonic scale, and what
 * that wall clock was worth at the time.
 *
 * Declaring the uncertainty rather than implying a precision is the
 * whole point. A journal that dates events to the nanosecond on a
 * machine synchronised by NTP is claiming something it cannot support,
 * and an auditor who notices that has reason to doubt everything else
 * in it. Instrumentation states its source and its error; so does this.
 */

#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/timekeeping.h>
#include <linux/moduleparam.h>
#include "beamfs.h"

/*
 * What the wall clock is worth, declared rather than guessed.
 *
 * The obvious approach -- ask the timekeeping core whether NTP thinks
 * it is synchronised -- does not work from a module: do_adjtimex is
 * not exported, and nothing else exposes STA_UNSYNC. Which is just as
 * well, because "NTP reports itself synchronised" is a weak claim to
 * build a forensic record on: it says a daemon is running, not that
 * the clock is right.
 *
 * So it is declared. An operator who has deployed PTP with hardware
 * timestamping knows it and says so; one who has an RTC and nothing
 * else says that. The journal then carries a claim its owner made
 * rather than one this code inferred.
 *
 * MiFID II takes the same line on trade timestamps: the requirement is
 * traceability to a stated reference, not a number on its own. A
 * timestamp whose provenance is unstated is not evidence, whatever its
 * resolution.
 *
 * The interval between two entries is unaffected by any of this. Those
 * come from ktime_get_ns(), are good to the timer's resolution, and
 * need no anchor at all -- a burst of forty upsets inside three
 * milliseconds reads as exactly that on the worst-served machine.
 */
static u32 beamfs_clock_source = BEAMFS_CLOCK_UNKNOWN;
module_param_named(clock_source, beamfs_clock_source, uint, 0644);
MODULE_PARM_DESC(clock_source,
		 "Wall-clock quality for journal anchoring: 0 unknown, 1 RTC, 2 NTP, 3 hardware (PTP/GPS)");

static u32 beamfs_clock_quality(void)
{
	u32 q = READ_ONCE(beamfs_clock_source);

	if (q > BEAMFS_CLOCK_HARDWARE)
		q = BEAMFS_CLOCK_UNKNOWN;

	/*
	 * A machine that has never had its clock set cannot be
	 * hardware-disciplined whatever the parameter says. Cheap
	 * sanity, and it catches the common case of an embedded board
	 * booting with no RTC and a clock_source setting copied from
	 * elsewhere.
	 */
	if (q > BEAMFS_CLOCK_UNKNOWN &&
	    ktime_get_real_seconds() < 1000000000LL)
		return BEAMFS_CLOCK_UNKNOWN;

	return q;
}

/*
 * beamfs_clock_anchor -- record the two clocks at one instant.
 *
 * Sampled as close together as the code can manage; the gap between
 * the two reads is a handful of nanoseconds and is not the dominant
 * error by several orders of magnitude.
 *
 * Called at mount and refreshed by the scrubber, so drift is bounded
 * by the refresh interval instead of accumulating from mount. A volume
 * left mounted six months would otherwise be dated by a reading six
 * months stale.
 */
void beamfs_clock_anchor(struct super_block *sb)
{
	struct beamfs_sb_info *sbi = BEAMFS_SB(sb);
	u64 mono, real;
	u32 quality;

	quality = beamfs_clock_quality();
	mono = ktime_get_ns();
	real = ktime_get_real_ns();

	sbi->s_anchor_mono = mono;
	sbi->s_anchor_real = real;
	sbi->s_anchor_quality = quality;

	if (sbi->s_beamfs_sb) {
		sbi->s_beamfs_sb->s_anchor_mono = cpu_to_le64(mono);
		sbi->s_beamfs_sb->s_anchor_real = cpu_to_le64(real);
		sbi->s_beamfs_sb->s_anchor_quality = cpu_to_le32(quality);
		beamfs_dirty_super(sbi);
	}
}

/*
 * beamfs_clock_to_real -- turn a journal timestamp into a wall time.
 *
 * Returns 0 when there is no anchor, which the caller must read as
 * "no date available" rather than "the epoch". An entry older than the
 * anchor converts to a time before it, which is correct: the monotonic
 * scale is continuous across the anchor.
 */
u64 beamfs_clock_to_real(struct beamfs_sb_info *sbi, u64 mono)
{
	if (!sbi->s_anchor_real)
		return 0;

	if (mono >= sbi->s_anchor_mono)
		return sbi->s_anchor_real + (mono - sbi->s_anchor_mono);

	return sbi->s_anchor_real - (sbi->s_anchor_mono - mono);
}
