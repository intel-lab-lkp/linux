// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   ALSA driver for Reloop Jockey 3 devices
 *
 *   Copyright (c) 2026 by Frank van de Pol <fvdpol@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/timekeeping.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/cleanup.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include "ploytec_proto.h"
#include "ploytec_codec.h"
#include "ploytec_midi.h"

#define RELOOP_VENDOR_ID         0x200c
#define RELOOP_JOCKEY3_ME_PID    0x1019
#define RELOOP_JOCKEY3_REMIX_PID 0x1037

enum { JOCKEY3_ME, JOCKEY3_REMIX };
#define CARD_NAME "Reloop Jockey 3"

/* Human-readable edition name for a usb_device_id.driver_info value. */
static const char *jockey3_model_name(int model)
{
	switch (model) {
	case JOCKEY3_ME:
		return "Master Edition";
	case JOCKEY3_REMIX:
		return "Remix";
	default:
		return "Unknown";
	}
}

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");

/**
 * DOC: Reverse-engineering references
 *
 * Comments below cite documents by paths beginning "re/". Those are the
 * reverse-engineering notes this driver was derived from -- USB protocol
 * analysis, vendor trace comparisons and the experiments behind several of the
 * timing constants. They are not part of the kernel tree: they live in the
 * driver's development repository, linked from its MAINTAINERS entry.
 */

/**
 * DOC: Device model
 *
 * The Reloop Jockey 3 presents two USB interfaces and speaks a proprietary
 * Ploytec protocol rather than USB Audio Class. Interface 0 is claimed by
 * probe(); interface 1 is claimed explicitly, as it owns the capture endpoint.
 * Three bulk endpoints carry everything:
 *
 * - EP 0x05 OUT: PCM playback, with the MIDI OUT byte stream multiplexed into
 *   a reserved slot of every packet (see PLOYTEC_MIDI_OUT_OFFSET)
 * - EP 0x86 IN:  PCM capture
 * - EP 0x83 IN:  MIDI input
 *
 * Audio is not sample-interleaved but bit-plane interleaved; see
 * ploytec_codec.c for the wire format and the encode/decode implementations.
 *
 * URBs run free for the lifetime of the device rather than being started and
 * stopped around PCM use: the playback stream must keep flowing because it
 * carries MIDI OUT, and the device expects a continuous packet stream. The PCM
 * callbacks therefore only toggle whether a URB's payload is filled from (or
 * copied to) an ALSA buffer.
 *
 * A sample-rate change requires tearing the URBs down, reprogramming the
 * device over EP0, and starting them again. jockey3_pcm_hw_params() checks
 * URB liveness on both directions afterward and recovers if either did not
 * restart; see the comment there for what that covers and why.
 *
 * A stall can also be found and recovered mid-stream, with no rate change or
 * PCM ioctl involved: jockey3_watchdog_work() polls URB liveness for the
 * device's whole lifetime and calls the same recovery ladder directly on a
 * new stall onset. See jockey3_watchdog_check() and jockey3_watchdog_arm().
 */

/**
 * DOC: Resource lifetime
 *
 * The card is created with snd_card_new(), not snd_devm_card_new(), and
 * released by jockey3_disconnect() with snd_card_free_when_closed(). A managed
 * card would instead be freed from the devres unwind inside
 * usb_unbind_interface(), where snd_card_free() blocks until userspace has
 * closed every file descriptor on the card -- on an unplug that unwind runs on
 * the USB hub work queue, so a single process sitting on a PCM fd would stall
 * hotplug for the whole hub. Every other USB sound driver in the tree avoids
 * that the same way.
 *
 * That splits teardown in two, and the split is the thing to keep straight:
 *
 * - jockey3_disconnect() does everything that must happen while the device is
 *   still there: latch JOCKEY3_FLAG_DISCONNECTED (for either interface, since
 *   the core takes them down one at a time), release anyone waiting on a
 *   reset, stop the watchdog, kill the URBs, release interface 1, and hand the
 *   card to snd_card_free_when_closed().
 *
 * - jockey3_card_free(), reached through card->private_free once the last file
 *   descriptor is closed, frees the buffers and URBs, destroys rate_mutex and
 *   gives the card slot back. It runs arbitrarily long after the disconnect
 *   and must attempt no USB operation, though the objects themselves stay
 *   referenced until it returns; see jockey3_free_resources().
 *
 * struct jockey3_chip lives in card->private_data, so it is freed with the
 * card and outlives the USB binding -- which is what makes it safe for an ALSA
 * callback that is still in flight during an unplug to keep dereferencing it.
 * Such a callback finds JOCKEY3_FLAG_DISCONNECTED set and returns -ENODEV.
 *
 * probe's error path performs the disconnect half by hand and then calls
 * snd_card_free(); the card is never registered on any path that reaches
 * there, so the synchronous free cannot block.
 */

/**
 * DOC: Locking
 *
 * The lock hierarchy is::
 *
 *     rate_mutex                     process context only, outermost
 *       |- playback.lock             IRQ-safe leaf
 *       |- capture.lock              IRQ-safe leaf
 *       `- midi_lock                 IRQ-safe leaf
 *
 * The three leaf spinlocks are never nested inside one another; anything that
 * needs more than one takes them in sequence, not nested. rate_mutex is never
 * taken from atomic context.
 *
 * Against the ALSA core the order is::
 *
 *     snd_pcm_stream_lock  ->  playback.lock / capture.lock
 *
 * which is why the URB completion handlers drop their stream spinlock before
 * calling snd_pcm_period_elapsed() or snd_pcm_stop_xrun(); both take the
 * stream lock, and taking it while holding ours would invert the order.
 *
 * .trigger and .pointer are called by the core with the stream lock held and
 * interrupts disabled, so neither may sleep.
 *
 * The watchdog work item adds one rule: rate_mutex must never be held across
 * cancel_delayed_work_sync(&chip->watchdog_work). jockey3_stop_urbs() is called
 * from inside rate_mutex at four sites, so it disarms with the non-sync
 * cancel_delayed_work(), which is safe under any lock; a tick that is already
 * running when the cancel lands re-reads 'stopping' and does nothing. The sync
 * form appears only in jockey3_disconnect(), in probe's error path and in
 * jockey3_free_resources(), none of which holds a mutex.
 *
 * Because that disarm does not wait, a tick can be parked on rate_mutex while
 * jockey3_suspend() or jockey3_pre_reset() takes the device down, then wake to
 * the timestamps they zeroed and read the stream as stalled. So
 * jockey3_recover_urb_stream() re-tests DISCONNECTED, SUSPENDED and RESETTING
 * under the mutex and leaves the restart to whichever path is bringing the
 * device back.
 *
 * jockey3_watchdog_work() itself may call jockey3_recover_urb_stream(),
 * which takes rate_mutex and calls jockey3_stop_urbs() -- i.e. the watchdog's
 * own tick disarming itself via the non-sync cancel above, which is exactly
 * the safe case: it never blocks and does not affect the tick already
 * running. It may also queue a full USB reset; that reset runs on system_wq
 * (usb_queue_reset_device(), drivers/usb/core/message.c), never
 * system_long_wq where the watchdog runs, so the two cannot serialize behind
 * one another, and rate_mutex is dropped before the wait for it, letting
 * jockey3_pre_reset()/jockey3_post_reset() take the mutex themselves to
 * complete.
 *
 * This also means jockey3_recover_urb_stream() can be entered from two
 * independent contexts (the watchdog, and a PCM ioctl) for what turns out to
 * be the same stall. rate_mutex alone does not prevent both from running
 * their stop/start-or-reset sequence concurrently, since neither holds it for
 * the whole ladder -- chip->recovery_in_progress (atomic_cmpxchg(), not a
 * lock, since the second caller must decline rather than block) is what
 * makes only one such ladder run at a time; see its doc comment.
 */

#define JOCKEY3_N_URBS 8

/*
 * Maximum Ploytec packets per URB ("N"), per direction. Several packets are
 * coalesced into one USB bulk transfer to cut the completion-interrupt rate
 * (rationale and measurements in re/streaming_overhead.md).
 *
 * The live N is not fixed: jockey3_pcm_hw_params() picks it per stream open,
 * per direction, as the largest power of two that fits the requested period
 * size, and stores it in struct jockey3_pcm_urb_stream's @n_shift/@n_pkts.
 * These two values are only the *ceiling* on that choice, and the width the
 * URB transfer buffers are allocated at (JOCKEY3_*_XFER_SIZE) -- which is why
 * the ceiling has to be a compile-time constant. Two separate constants, not
 * one, so the ceiling can be tuned per direction if the firmware ever turns
 * out to accept coalescing on one direction but not the other.
 *
 * Changing the live N never reallocates or tears down the URB ring: only
 * urb->transfer_buffer_length varies. PLOYTEC_PKT_SIZE (512) is exactly the
 * USB high-speed bulk max-packet size, so an N x 512 B transfer is
 * indistinguishable on the wire from N separate 512 B ones; the firmware
 * cannot tell the two apart. The buffers stay allocated JOCKEY3_*_XFER_SIZE
 * wide regardless of the N in use.
 *
 * A live N of 1 in both directions must be byte-for-byte identical to a
 * driver without coalescing: every packet loop below is keyed off the live
 * @n_pkts and degenerates to the original single-packet code path at N=1.
 */
#define JOCKEY3_PLAYBACK_N	8
#define JOCKEY3_CAPTURE_N	8

#define JOCKEY3_PLAYBACK_XFER_SIZE	(JOCKEY3_PLAYBACK_N * PLOYTEC_PKT_SIZE)
#define JOCKEY3_CAPTURE_XFER_SIZE	(JOCKEY3_CAPTURE_N * PLOYTEC_PKT_SIZE)

/*
 * jockey3_check_urb_stream_alive()'s liveness window, per direction, scaled
 * by that direction's own live N (struct jockey3_pcm_urb_stream's @n_shift):
 * the worst-case URB span scales linearly with N (N packet intervals instead
 * of one), so scaling the window the same way keeps the same margin against
 * it at any N instead of eating into a fixed margin as N grows. N is always a
 * power of two (jockey3_pcm_hw_params() only ever derives one), so this is a
 * shift rather than a multiply.
 */
#define JOCKEY3_LIVENESS_WINDOW_NS(shift)	((u64)NSEC_PER_MSEC << (shift))

/* Consecutive URB transport errors tolerated before a direction is given up on */
#define JOCKEY3_MAX_URB_ERRORS 8

/*
 * URB liveness watchdog.
 *
 * A healthy stream completes one URB every @n_pkts packet intervals, and a
 * packet is 226.8 us at 44100 Hz down to 83.3 us at 96000 Hz. So
 * JOCKEY3_WATCHDOG_STALL_MS of silence is many consecutive missed URBs at any
 * supported rate and N -- more than scheduling delay or bus contention
 * produces on a device that is still streaming.
 *
 * The threshold is sized against ALSA core's own stall timeout
 * (wait_for_avail() in sound/core/pcm_lib.c, roughly buffer_size * 1100 / rate
 * ms), because jockey3_watchdog_check() recovers on this signal and needs
 * headroom to succeed before the core returns -EIO to userspace itself.
 * Contrast jockey3_check_urb_stream_alive()'s 1 ms window, which is sampled
 * repeatedly inside a start grace and need only answer "anything just now?".
 *
 * jockey3_watchdog_arm() self-reschedules from the nearer of the two
 * directions' deadlines, as dev_watchdog() does in net/sched/sch_generic.c.
 * JOCKEY3_WATCHDOG_POLL_MS is the ceiling, used before either direction has
 * started; JOCKEY3_WATCHDOG_MIN_POLL_MS the floor, so a confirmed stall is
 * rechecked tightly rather than waiting out a stale window.
 */
#define JOCKEY3_WATCHDOG_POLL_MS	1000
#define JOCKEY3_WATCHDOG_MIN_POLL_MS	10
#define JOCKEY3_WATCHDOG_STALL_MS	20
/*
 * Bounds and evidence thresholds for the "has this direction reached steady
 * streaming yet" checks that run over the window from jockey3_start_urbs()
 * until the device is proven alive -- distinct from JOCKEY3_WATCHDOG_STALL_MS,
 * which governs silence between two completions on an already-established
 * stream. The grace duration itself is the runtime-tunable start_grace_ms
 * (see its comment near the top of the file); these clamp a bad write and
 * gate what counts as real streaming.
 *
 * JOCKEY3_GRACE_MS_MIN is deliberately well above JOCKEY3_WATCHDOG_STALL_MS: a
 * grace at the stall threshold escalates on scheduling jitter by construction.
 *
 * JOCKEY3_HEALTHY_MIN_COMPLETIONS feeds jockey3_stream_streaming_healthy(), the
 * stricter "did the warm restart actually take" test: a real stream lands
 * hundreds of completions inside any plausible grace, so requiring a handful,
 * spread over at least half the time that many URBs need and still arriving,
 * rejects a trickle of implausibly fast FIFO-drain completions without
 * rejecting a healthy resume. See that function.
 */
#define JOCKEY3_GRACE_MS_MIN		50
#define JOCKEY3_GRACE_MS_MAX		5000
#define JOCKEY3_HEALTHY_MIN_COMPLETIONS	4

/*
 * Bounded-retry budget for jockey3_recover_urb_stream(), chip-wide because the
 * remedy it escalates to (a full usb_reset_device()) is shared by both
 * directions -- a per-direction counter would double the effective rate for
 * no reason. A window re-opens (and the counter resets) the first time the
 * budget is consulted after JOCKEY3_RECOVERY_WINDOW_MS has passed since the
 * current one started, so a chip that stops stalling is never left refusing
 * to recover for the rest of its life.
 *
 * The watchdog calls jockey3_recover_urb_stream() directly (see its
 * report_xrun parameter), and an early xrun report from that call can wake a
 * concurrent jockey3_pcm_prepare() retry on the same direction before the
 * watchdog's own call returns. chip->recovery_in_progress (see
 * jockey3_recover_urb_stream()) makes that concurrent second call decline
 * outright rather than draw from this budget, so one physical stall still
 * draws against it at most once.
 */
#define JOCKEY3_RECOVERY_MAX_ATTEMPTS	3
#define JOCKEY3_RECOVERY_WINDOW_MS	60000

/*
 * Grace period for a PCM direction to reach steady streaming after its URB
 * ring is (re)started, in milliseconds. Sized well above the worst measured
 * restart latency rather than tuned close to it: the error cost is
 * asymmetric, since too long only delays an escalation that was coming
 * anyway, while too short kills a stream that was merely late.
 *
 * Writable at runtime (0644) and read through jockey3_start_grace_ms(),
 * which clamps to [JOCKEY3_GRACE_MS_MIN, JOCKEY3_GRACE_MS_MAX] so a bad write
 * cannot drive the grace down to or below the stall threshold.
 */
static int start_grace_ms = 200;
module_param(start_grace_ms, int, 0644);
MODULE_PARM_DESC(start_grace_ms, "Grace (ms) for PCM to reach steady streaming after its URB ring is (re)started.");

/*
 * Current start-grace budget in ms, clamped so a runtime write cannot drive
 * it to or below the stall threshold.
 */
static unsigned int jockey3_start_grace_ms(void)
{
	int ms = READ_ONCE(start_grace_ms);

	return clamp(ms, JOCKEY3_GRACE_MS_MIN, JOCKEY3_GRACE_MS_MAX);
}

/**
 * struct jockey3_pcm_urb_stream - per-direction PCM streaming state
 * @substream: the open ALSA substream, or NULL; @lock
 * @anchor: anchor holding the submitted URBs, for stop/kill
 * @urbs: the URB ring
 * @bufs: transfer buffer for each URB, allocated at the maximum N; @n_pkts,
 *	not this width, governs how many bytes an URB actually transfers
 * @urbs_in_flight: number of submitted URBs; diagnostic, must reach 0 after a stop
 * @last_callback_time: ktime of the last completion, for stall detection. Zeroed
 *	by jockey3_stop_urbs() so a stopped stream is not reported as alive.
 * @first_callback_time: ktime of the first completion since the last
 *	jockey3_start_urbs(); zeroed there and by jockey3_stop_urbs(). With
 *	@completions_since_start it lets jockey3_stream_streaming_healthy()
 *	judge cadence, not just recency: the device can retire a URB
 *	microseconds after submit, which is a FIFO draining rather than audio.
 *	Set lock-free from the completion handler.
 * @completions_since_start: count of completions since the last
 *	jockey3_start_urbs(); zeroed there and by jockey3_stop_urbs(). atomic_t,
 *	incremented lock-free from the completion handler.
 * @urbs_started_time: ktime at which jockey3_start_urbs() submitted this
 *	direction's ring; zeroed by jockey3_stop_urbs(). The watchdog measures
 *	from here until the first completion arrives. A separate timestamp is
 *	needed because @last_callback_time is deliberately 0 between a start and
 *	the first completion, which jockey3_check_urb_stream_alive() must keep
 *	reading as "not alive" -- so the watchdog cannot reuse it without either
 *	reporting a stall at every start or breaking the post-rate-change check.
 *	It doubles as the watchdog's post-start grace baseline (the grace
 *	duration is start_grace_ms).
 * @lock: protects the fields marked "@lock" below; IRQ-safe leaf
 * @dma_off: byte offset into runtime->dma_area, i.e. the hardware pointer; @lock
 * @period_off: bytes accumulated towards the current period; @lock
 * @running: stream is triggered and its payload should be filled; @lock
 * @rate_committed: this direction has had a rate accepted by
 *	jockey3_pcm_hw_params() and not yet released by jockey3_pcm_hw_free();
 *	@lock. See jockey3_rate_committed_streams() for why the rate interlock
 *	keys off this rather than off @running or off @substream.
 * @callbacks_active: number of URB completions currently inside the "safe zone"
 *	where they may still touch @substream or runtime->dma_area; @lock. The
 *	last one out wakes @drain_wait. A count rather than a flag because there
 *	are JOCKEY3_N_URBS URBs per direction and their completions can overlap
 *	on SMP.
 * @drain_wait: waited on by jockey3_pcm_sync_stop() until @callbacks_active is 0
 * @stopping: set by jockey3_stop_urbs() before the anchor is killed, cleared by
 *	jockey3_start_urbs(); @lock. Tested by the completion handler inside the
 *	same critical section that anchors and resubmits, so a callback can never
 *	re-anchor a URB after the kill has drained the anchor.
 * @consec_errors: consecutive URB transport errors; @lock. Reset on any
 *	successful completion and by jockey3_start_urbs().
 * @stall_reported: the watchdog has logged the onset of the current stall;
 *	@lock. Edge flag, so a wedge produces one onset line and one closing
 *	line rather than one per tick. Cleared either by the stream completing a
 *	URB again, or by jockey3_watchdog_clear_stall() when a restart ends the
 *	outage first -- which is the common case, since every recovery path goes
 *	through jockey3_stop_urbs()/jockey3_start_urbs().
 * @stall_since: ktime the current stall was measured from; @lock. Only
 *	meaningful while @stall_reported is set, and used to report how long the
 *	outage lasted once the stream comes back.
 * @n_shift: log2 of the Ploytec packets per URB ("N") the next resubmission
 *	will carry, in [0, 3]; @lock. Set by jockey3_pcm_hw_params() from the
 *	period size and reset to the default by jockey3_pcm_close(), so an idle
 *	direction never re-arms at a stale N. Changing it does not tear the ring
 *	down -- the wire cannot tell one N x 512 B transfer from N separate ones
 *	-- so each URB picks up the value at its next resubmission.
 * @n_pkts: 1 << @n_shift, mirrored for loop bounds; @lock
 */
struct jockey3_pcm_urb_stream {
	struct snd_pcm_substream *substream;
	struct usb_anchor anchor;
	struct urb *urbs[JOCKEY3_N_URBS];
	unsigned char *bufs[JOCKEY3_N_URBS];
	atomic_t urbs_in_flight;
	atomic64_t last_callback_time;
	atomic64_t first_callback_time;
	atomic_t completions_since_start;
	atomic64_t urbs_started_time;
	spinlock_t lock;	/* protects this stream's state; IRQ-safe leaf */
	unsigned int dma_off;
	unsigned int period_off;
	bool running;
	bool rate_committed;
	unsigned int callbacks_active;
	wait_queue_head_t drain_wait;
	bool stopping;
	unsigned int consec_errors;
	bool stall_reported;
	u64 stall_since;
	u8 n_shift;
	u8 n_pkts;
};

/**
 * struct jockey3_chip - per-device driver state
 * @card: the ALSA card; read-only after probe
 * @dev: the USB device; read-only after probe. Referenced for the chip's
 *	lifetime, as @intf0 and @intf1 are, because the card can outlive the
 *	unbind and usb_disconnect() would otherwise free them underneath an ALSA
 *	entry point. Released by jockey3_free_resources().
 * @intf0: interface 0, which the driver is bound to; read-only after probe
 * @intf1: interface 1, claimed explicitly because it owns EP 0x86
 * @pcm: the PCM device; read-only after probe
 * @rmidi: the rawmidi device; read-only after probe
 * @xfer_buf: bounce buffer for EP0 control transfers, USB_XFER_BUF_SIZE bytes.
 *	Serialized by @rate_mutex once the card is live. The one exception is
 *	jockey3_initialize(), which runs from probe before the card is
 *	registered or the watchdog armed, so it is the only user in existence.
 * @rate_mutex: serializes sample-rate changes and the URB stop/start that goes
 *	with them; process context only, outermost lock
 * @flags: JOCKEY3_FLAG_* bits, atomic bitops. SUSPENDED is set before
 *	jockey3_suspend() takes @rate_mutex and cleared by
 *	jockey3_restore_device() under it, so a tick blocked on that mutex sees
 *	it on the way out.
 * @current_rate: sample rate the hardware is programmed to; @rate_mutex
 * @dev_idx: card slot held in jockey3_devices_used
 * @reset_done: completed by jockey3_post_reset(), and by jockey3_disconnect()
 *	so a waiter is released when the USB core skips post_reset() entirely.
 *	Re-armed by jockey3_queue_reset() for a reset this driver starts, and by
 *	jockey3_pre_reset() for one started anywhere else.
 * @watchdog_work: periodic URB liveness check; see jockey3_watchdog_work().
 *	Armed by jockey3_start_urbs() and disarmed by jockey3_stop_urbs(), so it
 *	runs exactly when the URBs are supposed to be flowing -- which, for this
 *	device, is its whole lifetime rather than only while a PCM stream is open.
 * @recovery_attempts: resets taken from the current window by
 *	jockey3_recovery_budget_take(); not mutex-protected, since
 *	jockey3_pcm_hw_params()'s post-rate-change liveness check runs outside
 *	@rate_mutex by design. The two-atomic race with @recovery_window_start is
 *	benign: the worst case is a handful of extra resets in one window, not a
 *	stuck or negative budget.
 * @recovery_window_start: ktime the current budget window opened; 0 before
 *	the first attempt. See JOCKEY3_RECOVERY_WINDOW_MS.
 * @recovery_in_progress: one jockey3_recover_urb_stream() ladder running at a
 *	time, chip-wide rather than per-direction for the same reason
 *	@recovery_attempts is chip-wide: jockey3_stop_urbs()/jockey3_start_urbs()
 *	restart the shared ring for both directions together, so a Playback
 *	recovery and a concurrent Capture recovery -- e.g. the watchdog and a
 *	racing jockey3_pcm_prepare() retry -- would step on each other's
 *	stop/start (or reset) sequence rather than being independent. A second
 *	caller finding this already set declines immediately rather than racing
 *	the first; the first caller's restart brings back whichever direction
 *	the second one wanted too. Test-and-set via atomic_cmpxchg() rather than
 *	a mutex held across the whole ladder, since jockey3_check_urb_stream_alive()
 *	callers elsewhere poll rather than block on recovery finishing.
 * @warm_start: true if the current URB ring was last started by the stall
 *	watchdog's own lightweight restart, false for every other (re)start
 *	(first open, rate change, USB reset, resume, probe). Both kinds share
 *	one start_grace_ms; this only labels which one is in effect, for
 *	dev_dbg(). Chip-wide, not per-direction: jockey3_start_urbs() restarts
 *	the shared ring for both directions at once. Plain bool, written by
 *	jockey3_start_urbs() and read unlocked by the watchdog via
 *	WRITE_ONCE()/READ_ONCE().
 * @midi_in_substream: open MIDI IN substream, or NULL; @midi_lock
 * @midi_out_substream: open MIDI OUT substream, or NULL; @midi_lock
 * @midi_in_urb: the single MIDI IN URB; not anchored, killed directly
 * @midi_in_buf: transfer buffer for @midi_in_urb
 * @midi_lock: protects the MIDI fields; IRQ-safe leaf
 * @midi_out_acc: accumulator for the MIDI OUT rate limiter; @midi_lock
 * @midi_rate_divisor: current_rate / PLOYTEC_PLAYBACK_FRAMES; @midi_lock.
 *	Derived from @current_rate and published here because the rate limiter
 *	runs in URB completion context and cannot take @rate_mutex.
 * @midi_state: Running Status expander state; @midi_lock
 * @midi_stopping: see jockey3_pcm_urb_stream.stopping; @midi_lock
 * @midi_consec_errors: consecutive MIDI IN URB errors; @midi_lock
 * @playback: playback streaming state
 * @capture: capture streaming state
 */
struct jockey3_chip {
	struct snd_card *card;
	struct usb_device *dev;
	struct usb_interface *intf0;
	struct usb_interface *intf1;
	struct snd_pcm *pcm;
	struct snd_rawmidi *rmidi;
	unsigned char *xfer_buf;
	struct mutex rate_mutex;	/* serializes rate changes; outermost lock */
	unsigned long flags;
	unsigned int current_rate;
	unsigned int dev_idx;
	struct completion reset_done;
	struct delayed_work watchdog_work;
	atomic_t recovery_attempts;
	atomic64_t recovery_window_start;
	atomic_t recovery_in_progress;
	bool warm_start;	/* set by jockey3_start_urbs(): warm vs cold, for dev_dbg() only */

	/* MIDI Path */
	struct snd_rawmidi_substream *midi_in_substream;
	struct snd_rawmidi_substream *midi_out_substream;
	struct urb *midi_in_urb;
	unsigned char *midi_in_buf;
	spinlock_t midi_lock;	/* protects the MIDI fields; IRQ-safe leaf */
	unsigned int midi_out_acc;
	unsigned int midi_rate_divisor;
	struct ploytec_midi_running_status midi_state;
	bool midi_stopping;
	unsigned int midi_consec_errors;

	/* PCM urb streams */
	struct jockey3_pcm_urb_stream playback;
	struct jockey3_pcm_urb_stream capture;
};

static struct usb_driver jockey3_driver;

/*
 * Card index allocation. A plain incrementing counter would both race between
 * concurrent probes and never reuse a slot, so after SNDRV_CARDS successful
 * probes no further device could attach even if all of them had been unplugged.
 */
static DEFINE_MUTEX(jockey3_devices_mutex);
static DECLARE_BITMAP(jockey3_devices_used, SNDRV_CARDS);

/* Chip flags */
#define JOCKEY3_FLAG_DISCONNECTED	0
#define JOCKEY3_FLAG_RESETTING		1
#define JOCKEY3_FLAG_SUSPENDED		2

static inline bool jockey3_is_disconnected(const struct jockey3_chip *chip)
{
	return test_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
}

static inline bool jockey3_is_resetting(const struct jockey3_chip *chip)
{
	return test_bit(JOCKEY3_FLAG_RESETTING, &chip->flags);
}

static inline bool jockey3_is_suspended(const struct jockey3_chip *chip)
{
	return test_bit(JOCKEY3_FLAG_SUSPENDED, &chip->flags);
}

/*
 * Publish a new sample rate.
 *
 * chip->current_rate is protected by rate_mutex, but the MIDI OUT rate limiter
 * runs in URB completion (atomic) context and cannot take it. Rather than have
 * that path read current_rate unlocked, the value it actually needs is derived
 * here and published under midi_lock -- which also gets the division off the
 * per-URB hot path.
 */
static void jockey3_set_current_rate(struct jockey3_chip *chip, unsigned int rate)
{
	lockdep_assert_held(&chip->rate_mutex);

	chip->current_rate = rate;
	scoped_guard(spinlock_irqsave, &chip->midi_lock)
		chip->midi_rate_divisor = rate / PLOYTEC_PLAYBACK_FRAMES;
}

/*
 * Rate changes are serialized by chip->rate_mutex alone: jockey3_pcm_hw_params()
 * performs the whole stop/set-rate/start sequence while holding it, so any other
 * sleepable callback that takes the mutex is automatically excluded for the
 * duration. There is deliberately no separate "rate changing" flag to poll --
 * the only caller that could not take the mutex was .trigger, which runs in
 * atomic context and must not block at all.
 */

/*
 * Bounded, synchronous wait for a device reset queued via
 * usb_queue_reset_device() to complete. chip->reset_done is completed by
 * jockey3_post_reset(), and also by jockey3_disconnect() so that a waiter is
 * released when the USB core skips post_reset() entirely (a failed reset marks
 * the interface for rebinding and unbinds it instead).
 *
 * Deliberately does NOT call usb_reset_device() itself. A failed or aborted
 * reset marks the interface for rebinding and unbinds it, so the call would
 * run jockey3_disconnect() -- and the card teardown behind it -- in the very
 * thread that is still executing an ioctl on one of that card's file
 * descriptors. Queuing the reset instead lets the disconnect run on the USB
 * core's own workqueue, where the ALSA core's own refcounting keeps the
 * substream alive until this ioctl returns and userspace closes it.
 *
 * Until the card was switched from snd_devm_card_new() to
 * snd_card_free_when_closed() this was worse still: the teardown blocked in
 * snd_card_free() waiting for that same file descriptor to be closed, which
 * could never happen. That specific self-deadlock is gone, but reaching a
 * disconnect from inside an ioctl on the disconnecting card remains something
 * to keep out of this driver's calling threads.
 */
static int jockey3_wait_for_reset_completion(struct jockey3_chip *chip)
{
	/*
	 * Empirical testing shows that the reset cycle typically takes around
	 * 334 ms; a 1000 ms timeout gives sufficient headroom.
	 */
	if (!jockey3_is_resetting(chip))
		return 0;

	dev_dbg(&chip->intf0->dev, "Waiting for reset completion\n");

	if (!wait_for_completion_timeout(&chip->reset_done, msecs_to_jiffies(1000))) {
		dev_warn(&chip->intf0->dev, "Timeout waiting for reset completion\n");
		return -EAGAIN;
	}

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	return 0;
}

/*
 * Queue a full USB reset and arm chip->reset_done for a waiter. Does not
 * wait; pair with jockey3_wait_for_reset_completion() for that.
 */
static void jockey3_queue_reset(struct jockey3_chip *chip)
{
	reinit_completion(&chip->reset_done);
	set_bit(JOCKEY3_FLAG_RESETTING, &chip->flags);
	usb_queue_reset_device(chip->intf0);
}

/*
 * Chip-wide bounded-retry budget consulted by jockey3_recover_urb_stream()
 * before it escalates to a full USB reset. Returns true if the caller may go
 * ahead. A window older than JOCKEY3_RECOVERY_WINDOW_MS (or none opened yet)
 * is replaced with a fresh one, so a chip that stops stalling is never left
 * permanently refusing to recover; within a live window, up to
 * JOCKEY3_RECOVERY_MAX_ATTEMPTS resets are allowed before further attempts
 * are declined and reported instead.
 */
static bool jockey3_recovery_budget_take(struct jockey3_chip *chip)
{
	u64 now = ktime_get_mono_fast_ns();
	u64 window_start = atomic64_read(&chip->recovery_window_start);

	if (!window_start ||
	    now - window_start > (u64)JOCKEY3_RECOVERY_WINDOW_MS * NSEC_PER_MSEC) {
		atomic64_set(&chip->recovery_window_start, now);
		atomic_set(&chip->recovery_attempts, 1);
		return true;
	}

	return atomic_inc_return(&chip->recovery_attempts) <= JOCKEY3_RECOVERY_MAX_ATTEMPTS;
}

static inline struct jockey3_pcm_urb_stream *jockey3_get_pcm_urb_stream(struct jockey3_chip *chip,
									const int direction)
{
	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		return &chip->playback;
	else
		return &chip->capture;
}

static void jockey3_set_rate_committed(struct jockey3_chip *chip, const int direction,
				       bool committed)
{
	struct jockey3_pcm_urb_stream *urb_stream = jockey3_get_pcm_urb_stream(chip, direction);

	guard(spinlock_irqsave)(&urb_stream->lock);
	urb_stream->rate_committed = committed;
}

/*
 * How many directions currently hold the hardware rate, i.e. have had a rate
 * accepted by jockey3_pcm_hw_params() and not yet released by
 * jockey3_pcm_hw_free(). The device has a single rate for both directions, so
 * this is what the open-time constraint and the hw_params interlock key off.
 *
 * Deliberately not a count of *running* streams: .trigger is what sets
 * @running, so a stream that is open, has its rate fixed and is merely waiting
 * to be started would not be counted, and the other direction could reprogram
 * the hardware underneath it -- it would then play or record at the wrong
 * speed.
 *
 * Equally deliberately not a count of *open* streams: a full-duplex
 * application opens both directions before calling hw_params on either, and
 * counting opens would pin the second one to whatever rate the device happens
 * to be sitting at, refusing an otherwise legal rate change.
 *
 * One consequence is worth knowing: the constraint jockey3_pcm_open() applies
 * is permanent for that substream's runtime, while a commitment is not, so a
 * stream that opened while the other direction held a rate stays pinned to it
 * even after that direction closes. That is inherent to a device with a single
 * rate for both directions, and is the safe side of the trade.
 */
static int jockey3_rate_committed_streams(struct jockey3_chip *chip)
{
	int committed = 0;

	scoped_guard(spinlock_irqsave, &chip->capture.lock) {
		if (chip->capture.rate_committed)
			committed++;
	}

	scoped_guard(spinlock_irqsave, &chip->playback.lock) {
		if (chip->playback.rate_committed)
			committed++;
	}

	return committed;
}

static bool jockey3_process_out_packet(struct jockey3_chip *chip, u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->playback.substream;
	struct jockey3_pcm_urb_stream *urb_stream = &chip->playback;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	unsigned int frames_in_batch;
	unsigned int bytes_avail;
	int f = 0;

	if (unlikely(!substream || !substream->runtime))
		return false;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return false;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(substream);
	alsa_frame_size = runtime->channels * 3;  // 4 * 3 = 12 bytes

	while (f < PLOYTEC_PLAYBACK_FRAMES) {
		/* calculate how many samples we can process in one batch */
		frames_in_batch = PLOYTEC_PLAYBACK_FRAMES - f;
		bytes_avail = pcm_buffer_size - urb_stream->dma_off;

		/* Respect circular buffer wrap-around */
		if (bytes_avail < frames_in_batch * alsa_frame_size)
			frames_in_batch = bytes_avail / alsa_frame_size;

		if (frames_in_batch == 0)
			break;

		ploytec_encode_batch(urb_buf + f * PLOYTEC_PLAYBACK_FRAME_SIZE,
				     runtime->dma_area + urb_stream->dma_off,
				     frames_in_batch);

		urb_stream->dma_off += frames_in_batch * alsa_frame_size;
		if (urb_stream->dma_off >= pcm_buffer_size)
			urb_stream->dma_off -= pcm_buffer_size;

		urb_stream->period_off += frames_in_batch * alsa_frame_size;

		f += frames_in_batch;
	}

	if (urb_stream->period_off >= runtime->period_size * alsa_frame_size) {
		urb_stream->period_off %= runtime->period_size * alsa_frame_size;
		return true;
	}

	return false;
}

static bool jockey3_process_in_packet(struct jockey3_chip *chip, const u8 *urb_buf)
{
	struct snd_pcm_substream *substream = chip->capture.substream;
	struct jockey3_pcm_urb_stream *urb_stream = &chip->capture;
	struct snd_pcm_runtime *runtime;
	unsigned int pcm_buffer_size;
	unsigned int alsa_frame_size;
	unsigned int frames_in_batch;
	unsigned int bytes_left;
	int f = 0;

	if (unlikely(!substream || !substream->runtime))
		return false;

	runtime = substream->runtime;
	if (unlikely(!runtime->dma_area))
		return false;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(substream);
	alsa_frame_size = runtime->channels * 3; // 6 * 3 = 18 bytes

	while (f < PLOYTEC_CAPTURE_FRAMES) {
		frames_in_batch = PLOYTEC_CAPTURE_FRAMES - f;
		bytes_left = pcm_buffer_size - urb_stream->dma_off;

		/* Respect circular buffer wrap-around */
		if (bytes_left < frames_in_batch * alsa_frame_size)
			frames_in_batch = bytes_left / alsa_frame_size;

		if (frames_in_batch == 0)
			break;

		ploytec_decode_batch(runtime->dma_area + urb_stream->dma_off,
				     urb_buf + f * PLOYTEC_CAPTURE_FRAME_SIZE,
				     frames_in_batch);

		/* Advance pointers */
		urb_stream->dma_off += frames_in_batch * alsa_frame_size;
		if (urb_stream->dma_off >= pcm_buffer_size)
			urb_stream->dma_off -= pcm_buffer_size;

		urb_stream->period_off += frames_in_batch * alsa_frame_size;

		f += frames_in_batch;
	}

	if (urb_stream->period_off >= runtime->period_size * alsa_frame_size) {
		urb_stream->period_off %= runtime->period_size * alsa_frame_size;
		return true;
	}

	return false;
}

enum jockey3_urb_state {
	JOCKEY3_URB_OK,		/* completed normally */
	JOCKEY3_URB_STOPPED,	/* teardown in progress; return without resubmitting */
	JOCKEY3_URB_ERROR,	/* transport error, potentially transient */
};

static inline enum jockey3_urb_state jockey3_urb_check(const struct urb *urb)
{
	if (likely(urb->status == 0))
		return JOCKEY3_URB_OK;

	if (urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)
		return JOCKEY3_URB_STOPPED;

	return JOCKEY3_URB_ERROR;
}

/**
 * jockey3_warn_unexpected_stop() - report a URB that someone else cancelled
 * @chip: driver state
 * @stopping: whether this direction's teardown fence was set
 * @status: the urb->status that retired the URB
 * @type: direction name, for the log message
 *
 * -ENOENT, -ECONNRESET and -ESHUTDOWN normally mean the driver killed the URB
 * itself, and jockey3_urb_check() maps them to JOCKEY3_URB_STOPPED on that
 * assumption. Nothing verifies it: the USB core flushes an endpoint's URBs from
 * usb_disable_endpoint(), so an alt-setting change or an endpoint teardown
 * started anywhere else retires the whole ring by the same route, and every URB
 * of the direction would return here without a word.
 *
 * A physical unplug arrives here too and is not worth reporting.
 * usb_disconnect() moves the device to USB_STATE_NOTATTACHED before unbinding
 * the interfaces, so URBs that complete ahead of jockey3_disconnect() are
 * recognized by that rather than mistaken for an unexplained teardown.
 *
 * This is defensive. No failure observed so far has been traced to this path.
 */
static void jockey3_warn_unexpected_stop(struct jockey3_chip *chip, bool stopping,
					 int status, const char *type)
{
	if (stopping || jockey3_is_disconnected(chip) || jockey3_is_resetting(chip))
		return;

	if (chip->dev->state == USB_STATE_NOTATTACHED)
		return;

	/* Ratelimited: a whole ring of URBs retires together */
	dev_warn_ratelimited(&chip->intf0->dev,
			     "%s URB cancelled without a driver-initiated stop: %d\n",
			     type, status);
}

/**
 * jockey3_report_xrun() - tell userspace this direction lost its data
 * @urb_stream: the affected direction
 *
 * Reports an xrun on the open substream, if there is one. Nothing else in the
 * driver can do this safely by hand: snd_pcm_stop_xrun() takes the stream lock,
 * and the documented order is snd_pcm_stream_lock -> urb_stream->lock, so the
 * call has to be made with our spinlock dropped. Between dropping it and taking
 * it again the substream could be freed underneath us, which is what the
 * callbacks_active "safe zone" prevents -- jockey3_pcm_sync_stop() waits for
 * that count to reach zero before the ALSA core releases the buffer.
 *
 * Callers must hold neither @urb_stream->lock nor any driver mutex.
 */
static void jockey3_report_xrun(struct jockey3_pcm_urb_stream *urb_stream)
{
	struct snd_pcm_substream *substream = NULL;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		if (urb_stream->running && urb_stream->substream) {
			/* Join the safe zone so the substream cannot be freed below */
			urb_stream->callbacks_active++;
			substream = urb_stream->substream;
		}
	}

	if (!substream)
		return;

	snd_pcm_stop_xrun(substream);

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		if (!--urb_stream->callbacks_active)
			wake_up(&urb_stream->drain_wait);
	}
}

/**
 * jockey3_err_device_gone() - did the device simply leave?
 * @err: a transfer's return value
 *
 * Keyed on the error code, not on jockey3_is_disconnected(): a failed reset
 * also leaves the device %USB_STATE_NOTATTACHED and sets that flag on its way
 * to unbinding, and that one is a fault worth logging.
 *
 * Return: true if @err means the device is gone rather than misbehaving.
 */
static bool jockey3_err_device_gone(int err)
{
	return err == -ENODEV || err == -ESHUTDOWN;
}

/*
 * An unplug fails every in-flight URB's resubmit at once, so demote that case
 * as the submit and rate paths do. Log level only: @consec_errors comes from
 * jockey3_urb_error_give_up() and the completion status, not from here.
 */
static void jockey3_resubmit_failed(struct jockey3_chip *chip, int err, const char *what)
{
	if (jockey3_err_device_gone(err)) {
		dev_dbg(&chip->intf0->dev,
			"Not resubmitting the %s URB: device is gone (%d)\n", what, err);
		return;
	}

	dev_err(&chip->intf0->dev, "Failed to resubmit %s URB: %d\n", what, err);
}

/**
 * jockey3_urb_error_give_up() - account for a URB transport error
 * @chip: driver state
 * @urb_stream: the affected direction
 * @status: the urb->status that was seen
 * @type: direction name, for log messages
 *
 * Accounts for a transport error (-EPROTO, -EPIPE, -EOVERFLOW, -ETIME, ...).
 *
 * These are frequently transient -- marginal cabling and some host controllers
 * produce them routinely -- so a single one must not disable the card. Keep
 * resubmitting while the consecutive count stays below JOCKEY3_MAX_URB_ERRORS;
 * beyond that, stop feeding this direction and leave recovery to the next
 * .prepare, which already carries the stall detection and reset path.
 *
 * Return: true if the caller should give up and not resubmit.
 */
static bool jockey3_urb_error_give_up(struct jockey3_chip *chip,
				      struct jockey3_pcm_urb_stream *urb_stream,
				      int status, const char *type)
{
	unsigned int errors;
	bool crossed_limit;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		errors = ++urb_stream->consec_errors;
		/*
		 * The increment and this test share one critical section, so
		 * exactly one caller observes the transition even though up to
		 * JOCKEY3_N_URBS completions arrive together.
		 */
		crossed_limit = errors == JOCKEY3_MAX_URB_ERRORS;
	}

	/*
	 * Already given up. When the device goes away every one of the
	 * JOCKEY3_N_URBS in-flight URBs completes with an error, so without this
	 * the limit would be re-reported once per URB (and the stream stopped
	 * repeatedly). Report the transition only.
	 */
	if (errors > JOCKEY3_MAX_URB_ERRORS)
		return true;

	dev_err_ratelimited(&chip->intf0->dev, "%s URB error: %d (%u consecutive)\n",
			    type, status, errors);

	if (!crossed_limit)
		return false;

	dev_err(&chip->intf0->dev,
		"%s stopped after %u consecutive URB errors; deferring recovery\n",
		type, errors);

	jockey3_report_xrun(urb_stream);

	return true;
}

/*
 * Record one URB completion for the liveness and cadence checks. The device can
 * retire a URB microseconds after submit -- a hardware-side FIFO draining rather
 * than audio actually clocked onto or off the wire -- so
 * jockey3_stream_streaming_healthy() needs the first completion's time and the
 * running count, not just the last time, to tell real flow from that trickle.
 * Lock-free; called at the top of each completion handler. If two completions
 * race, exactly one sees the count go 1->1 and stamps @first_callback_time; a
 * transient @last_callback_time < @first_callback_time is harmless, the health
 * check rejects it and is re-polled.
 */
static void jockey3_note_completion(struct jockey3_pcm_urb_stream *urb_stream)
{
	u64 now = ktime_get_mono_fast_ns();

	atomic64_set(&urb_stream->last_callback_time, now);
	if (atomic_inc_return(&urb_stream->completions_since_start) == 1)
		atomic64_set(&urb_stream->first_callback_time, now);
}

static void jockey3_capture_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	struct jockey3_pcm_urb_stream *urb_stream = &chip->capture;
	struct snd_pcm_substream *substream = NULL;
	int n_pkts = 0;
	bool period_elapsed = false;
	bool data_valid = true;
	bool active = false;
	bool stopping;
	int sp, ret;

	atomic_dec(&urb_stream->urbs_in_flight);
	jockey3_note_completion(urb_stream);

	switch (jockey3_urb_check(urb)) {
	case JOCKEY3_URB_STOPPED:
		scoped_guard(spinlock_irqsave, &urb_stream->lock)
			stopping = urb_stream->stopping;
		jockey3_warn_unexpected_stop(chip, stopping, urb->status, "Capture");
		return;
	case JOCKEY3_URB_ERROR:
		if (jockey3_urb_error_give_up(chip, urb_stream, urb->status, "Capture"))
			return;
		/* Transient: resubmit, but this buffer holds no usable data */
		data_valid = false;
		break;
	case JOCKEY3_URB_OK:
		break;
	}

	if (unlikely(jockey3_is_disconnected(chip)))
		return;

	/*
	 * The firmware fills a multi-packet capture URB completely rather than
	 * terminating at one 512 B packet, so the expected case is
	 * actual_length == JOCKEY3_CAPTURE_XFER_SIZE. Derive the count from
	 * what actually came back rather than assuming N, in case that
	 * changes under load or on other firmware revisions.
	 */
	if (data_valid) {
		n_pkts = urb->actual_length / PLOYTEC_PKT_SIZE;
		if (unlikely(n_pkts == 0)) {
			dev_err(&chip->intf0->dev, "Capture URB too small: %d; required at least %d\n",
				urb->actual_length, PLOYTEC_PKT_SIZE);
			data_valid = false;
		} else if (unlikely(urb->actual_length % PLOYTEC_PKT_SIZE)) {
			/*
			 * A short trailing partial packet: use the
			 * complete ones and drop the rest, but this should
			 * not happen on firmware behaving as described above
			 * -- log it once so a change in device behavior is
			 * visible instead of silently discarded audio.
			 */
			dev_warn_once(&chip->intf0->dev,
				      "Capture URB length %d not a multiple of %d, using %d packet(s)\n",
				      urb->actual_length, PLOYTEC_PKT_SIZE, n_pkts);
		}
	}

	/* Step 1: Safely fetch the pointer and join the safe zone */
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		if (data_valid)
			urb_stream->consec_errors = 0;

		if (data_valid && !urb_stream->stopping &&
		    urb_stream->running && urb_stream->substream) {
			urb_stream->callbacks_active++;
			active = true;
			substream = urb_stream->substream;

			for (sp = 0; sp < n_pkts; sp++)
				period_elapsed |= jockey3_process_in_packet(chip,
					urb->transfer_buffer + sp * PLOYTEC_PKT_SIZE);
		}
	}

	/*
	 * Step 2: Safe Zone. ALSA core can't free 'substream' because
	 * jockey3_pcm_sync_stop() waits for 'callbacks_active' to drain before
	 * the core releases the buffer. Our lock is released here to avoid an
	 * ABBA deadlock with ALSA's internal locking: snd_pcm_period_elapsed()
	 * takes the stream lock, and the order is stream lock -> urb_stream->lock.
	 */
	if (period_elapsed && substream)
		snd_pcm_period_elapsed(substream);

	ret = 0;
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		/* Leave the safe zone; last one out wakes any waiter */
		if (active && !--urb_stream->callbacks_active)
			wake_up(&urb_stream->drain_wait);

		/*
		 * Keep resubmitting the URB while the interface is alive. The
		 * 'stopping' test and the anchor+submit below must stay in this
		 * one critical section: that is what stops a URB being re-added
		 * to an anchor jockey3_stop_urbs() has already drained.
		 */
		if (!urb_stream->stopping && !jockey3_is_disconnected(chip)) {
			urb->transfer_buffer_length = urb_stream->n_pkts * PLOYTEC_PKT_SIZE;
			atomic_inc(&urb_stream->urbs_in_flight);
			usb_anchor_urb(urb, &urb_stream->anchor);
			ret = usb_submit_urb(urb, GFP_ATOMIC);
			if (ret < 0) {
				atomic_dec(&urb_stream->urbs_in_flight);
				usb_unanchor_urb(urb);
			}
		}
	}
	if (ret < 0)
		jockey3_resubmit_failed(chip, ret, "capture");
}

/**
 * jockey3_get_next_midi_out_byte() - pick the MIDI byte for one playback packet
 * @chip: driver state
 *
 * Every outgoing playback packet reserves one slot for MIDI. This returns the
 * byte for it, through a leaky-bucket limiter holding the stream to roughly
 * 2500 bytes/sec. The hardware carries the full MIDI line rate of 3125
 * bytes/sec, but sustained traffic at it makes the control surface stop
 * responding to updates, so the limit sits below that. The idle byte is
 * returned when there is nothing to send or no budget.
 *
 * midi_lock is held across snd_rawmidi_transmit() so
 * chip->midi_out_substream cannot change under us. The order is safe because
 * the rawmidi core calls snd_rawmidi_output_trigger() without substream->lock,
 * and holding a driver lock there is the established idiom (sound/usb/midi.c
 * does it under ep->buffer_lock).
 *
 * Called from the playback URB completion handler, so this runs in atomic
 * context.
 *
 * Return: the byte to place in the packet's MIDI slot.
 */
static u8 jockey3_get_next_midi_out_byte(struct jockey3_chip *chip)
{
	u8 b;

	guard(spinlock_irqsave)(&chip->midi_lock);

	/*
	 * Rate limit MIDI to ~2500 bytes/sec -- see the kernel-doc above for why
	 * this sits below the device's 3125 bytes/sec MIDI line rate.
	 */
	chip->midi_out_acc += 2500;
	if (chip->midi_out_acc < chip->midi_rate_divisor)
		return PLOYTEC_MIDI_IDLE_BYTE;
	chip->midi_out_acc -= chip->midi_rate_divisor;

	/* Handle queued byte from Running Status expansion first before consuming from ALSA */
	if (chip->midi_state.has_queued_byte) {
		chip->midi_state.has_queued_byte = false;
		return chip->midi_state.queued_byte;
	}

	if (!chip->midi_out_substream)
		return PLOYTEC_MIDI_IDLE_BYTE;

	if (snd_rawmidi_transmit(chip->midi_out_substream, &b, 1) != 1)
		return PLOYTEC_MIDI_IDLE_BYTE;

	return ploytec_midi_running_status_expand(&chip->midi_state, b, &chip->intf0->dev);
}

/*
 * Fill the sample area of a playback packet with silence, for when there is
 * no PCM data to send.
 *
 * Only the sample area is touched: everything from PLOYTEC_MIDI_OUT_OFFSET
 * onwards -- the MIDI slot, the sync byte and the trailing gap -- is rewritten
 * unconditionally by jockey3_playback_callback() immediately afterwards.
 */
static void jockey3_silence_out_packet(u8 *buf)
{
	memset(buf, 0, PLOYTEC_MIDI_OUT_OFFSET);
}

/*
 * Prime a freshly allocated playback buffer so the first URB, which is
 * submitted before any completion handler has run, carries a valid idle
 * packet. The buffer comes from kzalloc(), so the sample area and the
 * trailing gap are already silent.
 */
static void jockey3_init_out_packet(u8 *buf)
{
	int sp;

	for (sp = 0; sp < JOCKEY3_PLAYBACK_N; sp++) {
		u8 *sub = buf + sp * PLOYTEC_PKT_SIZE;

		sub[PLOYTEC_MIDI_OUT_OFFSET] = PLOYTEC_MIDI_IDLE_BYTE;
		sub[PLOYTEC_SYNC_BYTE_OFFSET] = PLOYTEC_SYNC_BYTE_VALUE;
	}
}

static void jockey3_playback_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	struct jockey3_pcm_urb_stream *urb_stream = &chip->playback;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	struct snd_pcm_substream *substream = NULL;
	unsigned int submit_pkts;
	bool period_elapsed = false;
	bool data_valid = true;
	bool active = false;
	bool stopping;
	int i, sp, ret;

	atomic_dec(&urb_stream->urbs_in_flight);
	jockey3_note_completion(urb_stream);

	switch (jockey3_urb_check(urb)) {
	case JOCKEY3_URB_STOPPED:
		scoped_guard(spinlock_irqsave, &urb_stream->lock)
			stopping = urb_stream->stopping;
		jockey3_warn_unexpected_stop(chip, stopping, urb->status, "Playback");
		return;
	case JOCKEY3_URB_ERROR:
		if (jockey3_urb_error_give_up(chip, urb_stream, urb->status, "Playback"))
			return;
		data_valid = false;
		break;
	case JOCKEY3_URB_OK:
		break;
	}

	if (unlikely(jockey3_is_disconnected(chip)))
		return;

	/*
	 * Step 1: Safely fetch the pointer and join the safe zone.
	 *
	 * @n_pkts is captured once here and reused below for the MIDI/sync
	 * loop and the resubmit's transfer_buffer_length, rather than re-read
	 * at each site: jockey3_pcm_hw_params() can change it concurrently
	 * (under the same lock), and this URB's buffer must only ever claim
	 * to carry as many freshly written packets as it actually filled
	 * this callback. Reading a larger value at resubmit than was used to
	 * fill would send stale packets left over from an earlier, larger N
	 * instead of tearing the ring down.
	 */
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		submit_pkts = urb_stream->n_pkts;

		if (data_valid)
			urb_stream->consec_errors = 0;

		if (data_valid && !urb_stream->stopping &&
		    urb_stream->running && urb_stream->substream) {
			urb_stream->callbacks_active++;
			active = true;
			substream = urb_stream->substream;

			for (sp = 0; sp < submit_pkts; sp++)
				period_elapsed |= jockey3_process_out_packet(chip,
					buf + sp * PLOYTEC_PKT_SIZE);
		} else {
			for (sp = 0; sp < submit_pkts; sp++)
				jockey3_silence_out_packet(buf + sp * PLOYTEC_PKT_SIZE);
		}
	}

	/*
	 * The outgoing MIDI data is encapsulated in the playback stream, one
	 * real (rate-limited) byte per packet: jockey3_get_next_midi_out_byte()'s
	 * leaky-bucket limiter is calibrated on @midi_rate_divisor, which
	 * assumes exactly one call per packet interval. Calling it once
	 * per URB instead of once per packet would silently divide MIDI
	 * OUT throughput by the chosen N.
	 */
	for (sp = 0; sp < submit_pkts; sp++) {
		u8 *sub = buf + sp * PLOYTEC_PKT_SIZE;

		sub[PLOYTEC_MIDI_OUT_OFFSET] = jockey3_get_next_midi_out_byte(chip);

		/* Ploytec Sync byte and gap padding */
		sub[PLOYTEC_SYNC_BYTE_OFFSET] = PLOYTEC_SYNC_BYTE_VALUE;
		for (i = PLOYTEC_SYNC_BYTE_OFFSET + 1; i < PLOYTEC_PKT_SIZE; i++)
			sub[i] = 0x00;
	}

	/*
	 * Step 2: Safe Zone. ALSA core can't free 'substream' because
	 * jockey3_pcm_sync_stop() waits for 'callbacks_active' to drain before
	 * the core releases the buffer. Our lock is released here to avoid an
	 * ABBA deadlock with ALSA's internal locking: snd_pcm_period_elapsed()
	 * takes the stream lock, and the order is stream lock -> urb_stream->lock.
	 */
	if (period_elapsed && substream)
		snd_pcm_period_elapsed(substream);

	ret = 0;
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		/* Leave the safe zone; last one out wakes any waiter */
		if (active && !--urb_stream->callbacks_active)
			wake_up(&urb_stream->drain_wait);

		/*
		 * Keep resubmitting the URB while the interface is alive. The
		 * 'stopping' test and the anchor+submit below must stay in this
		 * one critical section: that is what stops a URB being re-added
		 * to an anchor jockey3_stop_urbs() has already drained.
		 */
		if (!urb_stream->stopping && !jockey3_is_disconnected(chip)) {
			urb->transfer_buffer_length = submit_pkts * PLOYTEC_PKT_SIZE;
			atomic_inc(&urb_stream->urbs_in_flight);
			usb_anchor_urb(urb, &urb_stream->anchor);
			ret = usb_submit_urb(urb, GFP_ATOMIC);
			if (ret < 0) {
				atomic_dec(&urb_stream->urbs_in_flight);
				usb_unanchor_urb(urb);
			}
		}
	}
	if (ret < 0)
		jockey3_resubmit_failed(chip, ret, "playback");
}

static void jockey3_midi_in_callback(struct urb *urb)
{
	struct jockey3_chip *chip = urb->context;
	unsigned char *buf = (unsigned char *)urb->transfer_buffer;
	unsigned int errors;
	bool stopping;
	int i, n = 0, ret;

	switch (jockey3_urb_check(urb)) {
	case JOCKEY3_URB_STOPPED:
		scoped_guard(spinlock_irqsave, &chip->midi_lock)
			stopping = chip->midi_stopping;
		jockey3_warn_unexpected_stop(chip, stopping, urb->status, "MIDI IN");
		return;
	case JOCKEY3_URB_ERROR:
		scoped_guard(spinlock_irqsave, &chip->midi_lock)
			errors = ++chip->midi_consec_errors;

		/* Already given up; see jockey3_urb_error_give_up() */
		if (errors > JOCKEY3_MAX_URB_ERRORS)
			return;

		dev_err_ratelimited(&chip->intf0->dev,
				    "MIDI IN URB error: %d (%u consecutive)\n",
				    urb->status, errors);
		if (errors == JOCKEY3_MAX_URB_ERRORS) {
			dev_err(&chip->intf0->dev,
				"MIDI IN stopped after %u consecutive URB errors\n", errors);
			return;
		}
		/* Transient: resubmit, but there is no usable data in this buffer */
		urb->actual_length = 0;
		break;
	case JOCKEY3_URB_OK:
		scoped_guard(spinlock_irqsave, &chip->midi_lock)
			chip->midi_consec_errors = 0;
		break;
	}

	if (unlikely(jockey3_is_disconnected(chip)))
		return;

	/*
	 * Compact the payload in place, dropping the padding the device emits
	 * between real MIDI bytes. The transfer buffer is ours, so this needs no
	 * lock; doing it up front turns the delivery below into a single locked
	 * call instead of one per byte.
	 *
	 * For different devices (firmware revision) different padding bytes have
	 * been observed: 0xF9, 0xFB, 0xFD, 0xFF. Since the device is not sending
	 * any MIDI system or real-time messages, we can safely ignore any byte
	 * 0xF0..0xFF received from the device.
	 */
	for (i = 0; i < urb->actual_length; i++)
		if (buf[i] < 0xF0)
			buf[n++] = buf[i];

	ret = 0;
	scoped_guard(spinlock_irqsave, &chip->midi_lock) {
		/*
		 * Deliver under midi_lock so the substream cannot be cleared by
		 * jockey3_midi_in_close() while we are dereferencing it.
		 */
		if (n && !chip->midi_stopping && chip->midi_in_substream)
			snd_rawmidi_receive(chip->midi_in_substream, buf, n);

		if (!chip->midi_stopping && !jockey3_is_disconnected(chip))
			ret = usb_submit_urb(urb, GFP_ATOMIC);
	}
	if (ret < 0)
		jockey3_resubmit_failed(chip, ret, "MIDI IN");
}

/*
 * How long until a direction's watchdog deadline (last activity plus
 * JOCKEY3_WATCHDOG_STALL_MS, or the current start grace if no completion has
 * arrived since the last start), in ms clamped to
 * [JOCKEY3_WATCHDOG_MIN_POLL_MS, JOCKEY3_WATCHDOG_POLL_MS]. Returns
 * JOCKEY3_WATCHDOG_POLL_MS if the direction has never started (no deadline to
 * chase yet), and the floor if the deadline has already passed, so a
 * confirmed stall gets rechecked tightly instead of waiting out a stale
 * window. Only meaningful while a PCM stream is open somewhere -- see the
 * caller, jockey3_watchdog_arm().
 */
static unsigned long jockey3_watchdog_next_delay_ms(struct jockey3_chip *chip,
						    const struct jockey3_pcm_urb_stream *urb_stream)
{
	u64 last = atomic64_read(&urb_stream->last_callback_time);
	u64 started = atomic64_read(&urb_stream->urbs_started_time);
	u64 threshold_ms = JOCKEY3_WATCHDOG_STALL_MS;
	unsigned int grace_ms = jockey3_start_grace_ms();
	u64 now, remaining_ns;

	if (!started)
		return JOCKEY3_WATCHDOG_POLL_MS;

	now = ktime_get_mono_fast_ns();

	/*
	 * Mirror jockey3_watchdog_check()'s own time-based grace window
	 * (see its kernel-doc): while still inside it, the deadline to chase
	 * is urbs_started_time + grace_ms regardless of whether an early
	 * completion has already advanced last_callback_time, or this would
	 * schedule a tight re-poll off a completion that does not end grace.
	 */
	if (now - started < (u64)grace_ms * NSEC_PER_MSEC) {
		last = started;
		threshold_ms = grace_ms;
	} else if (!last) {
		last = started;
	}

	remaining_ns = last + threshold_ms * NSEC_PER_MSEC - now;

	if ((s64)remaining_ns <= 0)
		return JOCKEY3_WATCHDOG_MIN_POLL_MS;

	return clamp_t(unsigned long, div_u64(remaining_ns, NSEC_PER_MSEC),
		       JOCKEY3_WATCHDOG_MIN_POLL_MS, JOCKEY3_WATCHDOG_POLL_MS);
}

/* Forward declaration: defined further down, alongside its other caller jockey3_pcm_hw_params() */
static bool jockey3_stream_is_open(struct jockey3_chip *chip, const int direction);

/*
 * Schedule the next watchdog tick.
 *
 * State-dependent cadence. With no substream open nothing is waiting on ALSA
 * core's wait_for_avail() timeout, so there is no reason to chase
 * JOCKEY3_WATCHDOG_STALL_MS; poll at the JOCKEY3_WATCHDOG_POLL_MS ceiling.
 * Once a substream is open, self-reschedule from the nearer of the two
 * directions' deadlines, as dev_watchdog() does in net/sched/sch_generic.c.
 *
 * Unlike dev_watchdog() the delay is not passed through
 * round_jiffies_relative(), which rounds to whole seconds -- right for a
 * multi-second watchdog_timeo, two orders of magnitude too coarse here.
 *
 * system_long_wq rather than system_wq: the tick is cheap, but
 * jockey3_watchdog_check() may call jockey3_recover_urb_stream(), which blocks
 * for seconds, and system_wq items are expected to be short.
 */
static void jockey3_watchdog_arm(struct jockey3_chip *chip)
{
	unsigned long delay_ms;

	if (jockey3_stream_is_open(chip, SNDRV_PCM_STREAM_PLAYBACK) ||
	    jockey3_stream_is_open(chip, SNDRV_PCM_STREAM_CAPTURE))
		delay_ms = min(jockey3_watchdog_next_delay_ms(chip, &chip->playback),
			       jockey3_watchdog_next_delay_ms(chip, &chip->capture));
	else
		delay_ms = JOCKEY3_WATCHDOG_POLL_MS;

	queue_delayed_work(system_long_wq, &chip->watchdog_work, msecs_to_jiffies(delay_ms));
}

/**
 * jockey3_watchdog_clear_stall() - close out a stall that a restart ended
 * @chip: driver state
 * @urb_stream: the affected direction
 * @type: direction name, for the log message
 *
 * Every recovery path in this driver goes through jockey3_stop_urbs() and
 * jockey3_start_urbs(), so a stalled stream is essentially always brought back
 * by a restart rather than by starting to complete URBs again on its own. If
 * the restart simply cleared the flag, the watchdog's onset line would never
 * be paired with anything and the outage would have no recorded end -- which
 * makes "stalls that ended" indistinguishable from "stalls still open" for
 * anything reading the log afterwards.
 *
 * So the restart closes the outage explicitly. Together with the recovery line
 * in jockey3_watchdog_check(), every onset has exactly one counterpart.
 */
static void jockey3_watchdog_clear_stall(struct jockey3_chip *chip,
					 struct jockey3_pcm_urb_stream *urb_stream,
					 const char *type)
{
	u64 outage_ns = 0;
	bool reported;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		reported = urb_stream->stall_reported;
		if (reported)
			outage_ns = ktime_get_mono_fast_ns() - urb_stream->stall_since;
		urb_stream->stall_reported = false;
	}

	if (reported)
		dev_warn(&chip->intf0->dev,
			 "%s URB stream restarted after stalling for %llu ms\n",
			 type, div_u64(outage_ns, NSEC_PER_MSEC));
}

/*
 * Stop the watchdog from the URB teardown path.
 *
 * Deliberately the non-sync cancel: jockey3_stop_urbs() runs inside rate_mutex
 * at several sites and the work item takes locks of its own, so waiting for a
 * running tick here would be a deadlock waiting to happen. Not waiting is safe
 * because a tick that is already running re-reads 'stopping' under the stream
 * lock and does nothing. The sync cancel that teardown does need lives in
 * jockey3_disconnect() and in probe's error path, where no mutex is held.
 */
static void jockey3_watchdog_disarm(struct jockey3_chip *chip)
{
	cancel_delayed_work(&chip->watchdog_work);
}

/**
 * jockey3_stop_urbs() - stop all PCM and MIDI URBs
 * @chip: driver state
 *
 * Fences the completion handlers, then kills every URB. Sleeps, so it must not
 * be called from atomic context. On return no completion handler is running and
 * none can resubmit.
 */
static void jockey3_stop_urbs(struct jockey3_chip *chip)
{
	dev_dbg(&chip->intf0->dev, "Stopping all URBs\n");

	jockey3_watchdog_disarm(chip);

	/*
	 * Fence the completion handlers before killing anything. Each 'stopping'
	 * store pairs with the test the matching handler makes while holding the
	 * same lock it uses to anchor and resubmit, so once these guards are
	 * released no handler can add a URB back to an anchor we are about to
	 * drain. The spinlocks provide the required ordering; no explicit barrier
	 * is needed.
	 *
	 * These stores must also stay above the timestamp zeroing below. The
	 * watchdog is disarmed without waiting for a tick that is already
	 * running, and such a tick samples the timestamps before it takes the
	 * stream lock; 'stopping' being set by the time it gets there is the only
	 * thing that stops it reporting a stall for a stream we stopped on purpose.
	 */
	scoped_guard(spinlock_irqsave, &chip->playback.lock)
		chip->playback.stopping = true;
	scoped_guard(spinlock_irqsave, &chip->capture.lock)
		chip->capture.stopping = true;
	scoped_guard(spinlock_irqsave, &chip->midi_lock)
		chip->midi_stopping = true;

	/*
	 * usb_kill_urb()/usb_kill_anchored_urbs() do not return until the
	 * completion handler of each URB has finished, so no callback can still
	 * be in its safe zone once these return -- no separate drain is needed
	 * here (jockey3_pcm_sync_stop() covers the ALSA buffer-teardown path).
	 *
	 * Worth spelling out for the anchored form, which keeps getting read as
	 * a use-after-free: __usb_hcd_giveback_urb() does unanchor before
	 * urb->complete(), so the kill loop cannot find a running handler on the
	 * list. anchor->suspend_wakeups covers it -- raised before the unanchor
	 * and dropped after complete() returns, and usb_kill_anchored_urbs()
	 * drains until usb_anchor_check_wakeup() sees it zero.
	 */
	usb_kill_urb(chip->midi_in_urb);
	usb_kill_anchored_urbs(&chip->playback.anchor);
	usb_kill_anchored_urbs(&chip->capture.anchor);

	/*
	 * Drop the liveness timestamps: a stale value would otherwise make
	 * jockey3_check_urb_stream_alive() report a stopped stream as alive for
	 * up to its 1 ms window. The cadence counters go with them so the next
	 * start's jockey3_stream_streaming_healthy() sees only its own completions.
	 */
	atomic64_set(&chip->playback.last_callback_time, 0);
	atomic64_set(&chip->capture.last_callback_time, 0);
	atomic64_set(&chip->playback.first_callback_time, 0);
	atomic64_set(&chip->capture.first_callback_time, 0);
	atomic_set(&chip->playback.completions_since_start, 0);
	atomic_set(&chip->capture.completions_since_start, 0);
	atomic64_set(&chip->playback.urbs_started_time, 0);
	atomic64_set(&chip->capture.urbs_started_time, 0);

	/* after killing the URBs there will be no in-flight requests anymore since the callback
	 * function has been called as part of the shutdown. The number of in-flight URBs should
	 * therefore be zero at this point. Log an inconsistency error if not.
	 */
	if (atomic_read(&chip->playback.urbs_in_flight) != 0)
		dev_err(&chip->intf0->dev, "Inconsistent URB in-flight count: playback=%d != 0\n",
			atomic_read(&chip->playback.urbs_in_flight));
	if (atomic_read(&chip->capture.urbs_in_flight) != 0)
		dev_err(&chip->intf0->dev, "Inconsistent URB in-flight count: capture=%d != 0\n",
			atomic_read(&chip->capture.urbs_in_flight));
}

/*
 * Demote an unplug, as jockey3_start_urbs_failed() already does for the
 * aggregate result. -%ENOENT stays loud: the endpoints were disabled while the
 * driver is still bound, which takes a device reset to undo.
 *
 * @slot is the ring slot, or -1 for the single MIDI IN URB.
 */
static void jockey3_submit_failed(struct jockey3_chip *chip, int err,
				  const char *what, int slot)
{
	if (jockey3_err_device_gone(err)) {
		if (slot < 0)
			dev_dbg(&chip->intf0->dev,
				"Not submitting the %s URB: device is gone (%d)\n", what, err);
		else
			dev_dbg(&chip->intf0->dev,
				"Not submitting %s URB %d: device is gone (%d)\n",
				what, slot, err);
		return;
	}

	if (slot < 0)
		dev_err(&chip->intf0->dev, "Failed to submit the %s URB: %d\n", what, err);
	else
		dev_err(&chip->intf0->dev, "Failed to submit %s URB %d: %d\n",
			what, slot, err);
}

/**
 * jockey3_start_urbs() - submit all PCM and MIDI URBs
 * @chip: driver state
 * @warm: true only for the stall watchdog's own lightweight restart of a ring
 *	that was streaming a moment earlier; false for a cold start (first open,
 *	rate change, USB reset, resume, probe). Both share start_grace_ms; this
 *	only labels the restart for dev_dbg() and is recorded chip-wide in
 *	@chip->warm_start.
 *
 * Clears the stop fences and error budgets, then submits the full URB ring for
 * both directions plus the MIDI IN URB. Uses GFP_KERNEL, so process context
 * only. A failure to submit one URB does not prevent the others being tried.
 *
 * The return value must be checked. Only a completion handler resubmits a URB,
 * so a ring that comes up short stays short: nothing retries the URBs that
 * failed here, and the direction runs at reduced depth for as long as the
 * device stays bound, with no bookkeeping that would ever notice. Callers pass
 * the result to jockey3_start_urbs_failed(), or return it to their own caller.
 *
 * Return: 0 on success, or the first submit error encountered.
 */
static int jockey3_start_urbs(struct jockey3_chip *chip, bool warm)
{
	int i, ret, first_err = 0;
	int n_playback = 0, n_capture = 0;
	u8 playback_n_pkts, capture_n_pkts;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	/*
	 * Belt and braces; jockey3_recover_urb_stream() is what actually bails.
	 * jockey3_restore_device() clears the flag before its own restart.
	 */
	if (jockey3_is_suspended(chip))
		return -ESHUTDOWN;

	dev_dbg(&chip->intf0->dev, "Starting all URBs (%s start, grace %u ms)\n",
		warm ? "warm" : "cold", jockey3_start_grace_ms());

	/*
	 * Clear the error budget as well: a stream that was given up on must get
	 * a fresh JOCKEY3_MAX_URB_ERRORS allowance, otherwise the first error
	 * after a restart would immediately exceed the stale count and give up
	 * again with no retries.
	 */
	scoped_guard(spinlock_irqsave, &chip->playback.lock) {
		chip->playback.stopping = false;
		chip->playback.consec_errors = 0;
	}
	scoped_guard(spinlock_irqsave, &chip->capture.lock) {
		chip->capture.stopping = false;
		chip->capture.consec_errors = 0;
	}
	scoped_guard(spinlock_irqsave, &chip->midi_lock) {
		chip->midi_stopping = false;
		chip->midi_consec_errors = 0;
	}

	/* Report and clear any stall this restart is about to end */
	jockey3_watchdog_clear_stall(chip, &chip->playback, "Playback");
	jockey3_watchdog_clear_stall(chip, &chip->capture, "Capture");

	/*
	 * Stamp the start before submitting, not after: this is what the
	 * watchdog measures from until the first completion arrives, and a URB
	 * can complete before the loop below has finished. Reset the cadence
	 * counters in the same breath, and record which grace applies.
	 */
	WRITE_ONCE(chip->warm_start, warm);
	atomic64_set(&chip->playback.first_callback_time, 0);
	atomic64_set(&chip->capture.first_callback_time, 0);
	atomic_set(&chip->playback.completions_since_start, 0);
	atomic_set(&chip->capture.completions_since_start, 0);
	atomic64_set(&chip->playback.urbs_started_time, ktime_get_mono_fast_ns());
	atomic64_set(&chip->capture.urbs_started_time, ktime_get_mono_fast_ns());

	/*
	 * Each direction's ring is (re)armed at its own current N
	 * (jockey3_pcm_hw_params()), which persists across a restart rather than
	 * falling back to the JOCKEY3_PLAYBACK_N/JOCKEY3_CAPTURE_N default just
	 * because the ring turned over. A direction with no open stream is held
	 * at that default by jockey3_pcm_close(), so it always re-arms at a
	 * safe N.
	 *
	 * Both are read once here rather than per iteration: they are written
	 * under the stream lock, which is not held here, so a re-load could
	 * leave URBs in the same ring armed at different lengths.
	 */
	playback_n_pkts = READ_ONCE(chip->playback.n_pkts);
	capture_n_pkts = READ_ONCE(chip->capture.n_pkts);

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->playback.urbs[i]->transfer_buffer_length =
			playback_n_pkts * PLOYTEC_PKT_SIZE;
		chip->capture.urbs[i]->transfer_buffer_length =
			capture_n_pkts * PLOYTEC_PKT_SIZE;

		atomic_inc(&chip->playback.urbs_in_flight);
		usb_anchor_urb(chip->playback.urbs[i], &chip->playback.anchor);
		ret = usb_submit_urb(chip->playback.urbs[i], GFP_KERNEL);
		if (ret < 0) {
			atomic_dec(&chip->playback.urbs_in_flight);
			usb_unanchor_urb(chip->playback.urbs[i]);
			jockey3_submit_failed(chip, ret, "playback", i);
			if (!first_err)
				first_err = ret;
		} else {
			n_playback++;
		}

		atomic_inc(&chip->capture.urbs_in_flight);
		usb_anchor_urb(chip->capture.urbs[i], &chip->capture.anchor);
		ret = usb_submit_urb(chip->capture.urbs[i], GFP_KERNEL);
		if (ret < 0) {
			atomic_dec(&chip->capture.urbs_in_flight);
			usb_unanchor_urb(chip->capture.urbs[i]);
			jockey3_submit_failed(chip, ret, "capture", i);
			if (!first_err)
				first_err = ret;
		} else {
			n_capture++;
		}
	}
	ret = usb_submit_urb(chip->midi_in_urb, GFP_KERNEL);
	if (ret < 0) {
		jockey3_submit_failed(chip, ret, "MIDI IN", -1);
		if (!first_err)
			first_err = ret;
	}

	if (n_playback < JOCKEY3_N_URBS || n_capture < JOCKEY3_N_URBS) {
		if (jockey3_err_device_gone(first_err))
			dev_dbg(&chip->intf0->dev,
				"Started only %d/%d playback and %d/%d capture URBs; device is gone\n",
				n_playback, JOCKEY3_N_URBS, n_capture, JOCKEY3_N_URBS);
		else
			dev_err(&chip->intf0->dev,
				"Started only %d/%d playback and %d/%d capture URBs; ring will not refill\n",
				n_playback, JOCKEY3_N_URBS, n_capture, JOCKEY3_N_URBS);
	}

	/*
	 * Arm regardless of first_err. A ring that came up short, or did not come
	 * up at all, is precisely the state worth watching: when the endpoints
	 * have been disabled underneath us every submit fails and nothing is left
	 * to report the resulting silence.
	 */
	jockey3_watchdog_arm(chip);

	return first_err;
}

/**
 * jockey3_start_urbs_failed() - react to a failed jockey3_start_urbs()
 * @chip: driver state
 * @err: the value jockey3_start_urbs() returned; 0 is ignored
 * @context: what was being attempted, for the log message
 *
 * Classifies a submit failure so that a condition needing a device reset is not
 * mistaken for an ordinary unplug:
 *
 * - %-ENOENT means the endpoint is administratively gone while the driver is
 *   still bound. usb_submit_urb() reports it when usb_pipe_endpoint() finds no
 *   endpoint, and usb_hcd_link_urb_to_ep() when the endpoint is not enabled --
 *   which is the state usb_set_interface() leaves interface 0 in when its
 *   SET_INTERFACE request fails, since it disables the endpoints before sending
 *   the request and does not re-enable them on that path. Nothing short of
 *   re-enumerating the device restores it.
 *
 * - %-ENODEV means the device is already detached, so a reset would be
 *   pointless and jockey3_disconnect() is on its way. This is the common case
 *   on an ordinary unplug and must not be treated like the one above.
 *
 * - anything else is reported and left alone; the watchdog picks up whatever
 *   silence results.
 *
 * Note that trying to undo the failure by selecting the working altsetting
 * again does not work: if the control endpoint is unresponsive, that request
 * times out as well and the endpoints stay disabled regardless.
 */
static void jockey3_start_urbs_failed(struct jockey3_chip *chip, int err, const char *context)
{
	if (!err)
		return;

	if (err == -ENODEV || jockey3_is_disconnected(chip)) {
		dev_dbg(&chip->intf0->dev, "Could not start URBs after %s: device is gone\n",
			context);
		return;
	}

	if (err == -ESHUTDOWN || jockey3_is_suspended(chip)) {
		dev_dbg(&chip->intf0->dev,
			"Could not start URBs after %s: device is suspended; the resume path will start them\n",
			context);
		return;
	}

	if (err == -ENOENT) {
		dev_err(&chip->intf0->dev,
			"Endpoints are disabled after %s; the device needs a reset to restore them\n",
			context);
		return;
	}

	dev_err(&chip->intf0->dev, "Failed to start URBs after %s: %d\n", context, err);
}

/*
 * Log a failed rate-change step at the severity it deserves, matching what
 * jockey3_start_urbs_failed() applies on the URB path.
 */
static void jockey3_rate_step_failed(struct jockey3_chip *chip, int err, const char *what)
{
	if (jockey3_err_device_gone(err)) {
		dev_dbg(&chip->intf0->dev, "%s: device is gone (%d)\n", what, err);
		return;
	}

	dev_err(&chip->intf0->dev, "%s: %d\n", what, err);
}

static int jockey3_set_rate(struct jockey3_chip *chip, unsigned int rate, bool cold_init)
{
	int ret;
	u32 current_hw_rate;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	dev_dbg(&chip->intf0->dev, "Setting rate to %u Hz\n", rate);

	ret = ploytec_initialize_device(chip->intf0, chip->xfer_buf, !cold_init, NULL);
	if (ret < 0) {
		jockey3_rate_step_failed(chip, ret, "Failed to initialize device to change rate");
		return ret;
	}

	ret = ploytec_get_rate(chip->intf0, chip->xfer_buf, PLOYTEC_RATE_IDX_DEVICE,
			       &current_hw_rate);
	if (ret < 0) {
		jockey3_rate_step_failed(chip, ret, "Failed to read current hardware rate");
		return ret;
	}
	dev_dbg(&chip->intf0->dev, "Current hardware rate: %u Hz\n", current_hw_rate);

	/*
	 * Program the rate even when the device already reports it. Skipping the
	 * write on a match would silently elide it during probe every time,
	 * since initialization always asks for 44100 Hz and that is also the
	 * device's power-on default. The write evidently does more than set a
	 * frequency; no vendor initialization omits it. See
	 * re/usb/init_timing_comparison.md.
	 *
	 * Callers that want to avoid a redundant rate change already check
	 * against chip->current_rate before getting here.
	 */
	dev_dbg(&chip->intf0->dev, "Setting hardware rate: %u Hz\n", rate);
	ret = ploytec_set_rate(chip->intf0, chip->xfer_buf, rate, cold_init);
	if (ret < 0) {
		jockey3_rate_step_failed(chip, ret, "Failed to set rate");
		return ret;
	}
	ret = ploytec_start_streaming(chip->intf0, chip->xfer_buf);
	if (ret < 0) {
		jockey3_rate_step_failed(chip, ret, "Failed to start streaming after rate change");
		return ret;
	}

	dev_dbg(&chip->intf0->dev, "Rate set OK\n");
	return 0;
}

static bool jockey3_stream_is_open(struct jockey3_chip *chip, const int direction)
{
	struct jockey3_pcm_urb_stream *urb_stream = jockey3_get_pcm_urb_stream(chip, direction);
	bool open;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		open = urb_stream->substream;
	}
	return open;
}

/* Forward declaration: defined further down, alongside its other callers jockey3_pcm_hw_params()
 * and jockey3_pcm_prepare()
 */
static int jockey3_recover_urb_stream(struct jockey3_chip *chip, const int direction,
				      const char *context, bool report_xrun);

static bool jockey3_check_urb_stream_alive(const struct jockey3_pcm_urb_stream *urb_stream)
{
	u64 last_time = atomic64_read(&urb_stream->last_callback_time);
	u64 window_ns = JOCKEY3_LIVENESS_WINDOW_NS(READ_ONCE(urb_stream->n_shift));

	if (!last_time)
		return false;

	/*
	 * Alive if we had activity within the last window_ns. The window
	 * must exceed one URB span (@n_pkts packet intervals) or a
	 * perfectly healthy stream could sample as dead between completions
	 * -- and scaling it with N (see JOCKEY3_LIVENESS_WINDOW_NS()) keeps
	 * the same margin against that span at any N, rather than a fixed
	 * window eating into a shrinking margin as N grows.
	 */
	return (ktime_get_mono_fast_ns() - last_time <= window_ns);
}

/*
 * Shortest wall-clock time one URB's worth of audio can legitimately occupy:
 * @n_pkts Ploytec packets, PLOYTEC_PLAYBACK_FRAMES (10) or
 * PLOYTEC_CAPTURE_FRAMES (8) PCM frames each, at @rate. Returns 0 if @rate or
 * N is not yet known. Completions arriving in a small fraction of this are a
 * hardware-side FIFO draining or the host controller retiring transfer
 * descriptors, not the device's audio pipeline running.
 */
static u64 jockey3_min_urb_interval_ns(const struct jockey3_pcm_urb_stream *urb_stream,
				       unsigned int rate, bool is_playback)
{
	unsigned int fpp = is_playback ? PLOYTEC_PLAYBACK_FRAMES : PLOYTEC_CAPTURE_FRAMES;
	unsigned int n = READ_ONCE(urb_stream->n_pkts);

	if (!rate || !n)
		return 0;

	return div_u64((u64)n * fpp * NSEC_PER_SEC, rate);
}

/*
 * jockey3_stream_streaming_healthy() - is this direction actually streaming,
 * as opposed to having produced a trickle of implausibly fast completions?
 *
 * jockey3_check_urb_stream_alive() answers "did anything complete just now",
 * which a single FIFO-drain completion satisfies. This is the stricter test,
 * used at the one decision that must not be fooled by trickle: whether the
 * stall watchdog's warm URB restart worked, or the ladder should escalate to a
 * full USB reset (a stream-killing event, and on this hardware one that can
 * leave the capture endpoint down). It requires
 *
 *   - at least JOCKEY3_HEALTHY_MIN_COMPLETIONS completions since the last
 *     start -- more than a FIFO drain supplies, and a real stream lands
 *     hundreds inside any plausible grace;
 *   - those spread over at least half the time that many real URBs would take
 *     (rejects a fast burst);
 *   - the most recent one still within jockey3_check_urb_stream_alive()'s
 *     N-scaled liveness window (rejects a burst that then stopped).
 *
 * With no programmed rate it cannot form the interval and reports not-healthy
 * rather than fall back to a weaker test; @current_rate is always set while a
 * stream is recovering.
 */
static bool jockey3_stream_streaming_healthy(struct jockey3_chip *chip,
					     const struct jockey3_pcm_urb_stream *urb_stream)
{
	bool is_playback = urb_stream == &chip->playback;
	const char *type = is_playback ? "Playback" : "Capture";
	u64 first = atomic64_read(&urb_stream->first_callback_time);
	u64 last = atomic64_read(&urb_stream->last_callback_time);
	int n = atomic_read(&urb_stream->completions_since_start);
	unsigned int rate = READ_ONCE(chip->current_rate);
	u64 min_interval, span, floor;

	/* Not enough completions yet to judge -- the common "still warming up"
	 * state during the poll, kept silent so it does not flood the log.
	 */
	if (n < JOCKEY3_HEALTHY_MIN_COMPLETIONS || !first || last <= first)
		return false;

	min_interval = jockey3_min_urb_interval_ns(urb_stream, rate, is_playback);
	if (!min_interval) {
		dev_dbg_ratelimited(&chip->intf0->dev,
				    "%s health: no programmed rate, cannot judge cadence\n", type);
		return false;
	}

	span = last - first;
	floor = (u64)(n - 1) * (min_interval >> 1);
	if (span < floor) {
		dev_dbg_ratelimited(&chip->intf0->dev,
				    "%s health: %d completions in %llu us spans < %llu us floor -- still trickle\n",
				    type, n, div_u64(span, NSEC_PER_USEC),
				    div_u64(floor, NSEC_PER_USEC));
		return false;
	}

	if (ktime_get_mono_fast_ns() - last >
	    JOCKEY3_LIVENESS_WINDOW_NS(READ_ONCE(urb_stream->n_shift))) {
		dev_dbg_ratelimited(&chip->intf0->dev,
				    "%s health: cadence plausible but last completion is stale\n",
				    type);
		return false;
	}

	dev_dbg(&chip->intf0->dev,
		"%s health: streaming -- %d completions, ~%llu us/URB (min %llu us)\n",
		type, n, div_u64(span, (u64)(n - 1) * NSEC_PER_USEC),
		div_u64(min_interval, NSEC_PER_USEC));
	return true;
}

/**
 * jockey3_watchdog_check() - one direction's share of a watchdog tick
 * @chip: driver state
 * @direction: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
 *
 * Reports a direction that has stopped completing URBs and, on the onset edge,
 * recovers it -- the only path that catches a stall mid-stream, with no PCM
 * ioctl to hand it to. Gated as jockey3_pcm_hw_params() gates its own capture
 * recovery: Playback always recovers because it carries MIDI OUT; Capture only
 * if a capture stream is open, since the ladder restarts the shared ring and
 * an idle Capture stall would glitch working Playback for no visible gain. It
 * is logged and left for the next capture open.
 *
 * Logging is edge-triggered, one line per stall start and end. The measured
 * age is reported rather than the threshold, since the poll interval would
 * only bound it to one tick.
 *
 * Deliberately does not gate on urbs_in_flight: endpoints disabled underneath
 * the driver leave nothing in flight, which is the case that must not go
 * unnoticed. The count is reported as evidence -- a full ring means
 * "submitted, never returned", an empty one "nothing could be submitted".
 *
 * The onset tags itself "startup" or "steady-state" after which threshold
 * caught it: the start grace, or JOCKEY3_WATCHDOG_STALL_MS once that window
 * has passed. Only "steady-state" is a real mid-stream fault.
 *
 * Time-based rather than keyed off the first completion: at low N a restart
 * can complete an URB within under a millisecond, a hardware FIFO draining
 * rather than the pipeline catching up, so an early completion must not end
 * the grace. jockey3_stream_streaming_healthy() applies the same reasoning in
 * the recovery ladder.
 */
static void jockey3_watchdog_check(struct jockey3_chip *chip, const int direction)
{
	struct jockey3_pcm_urb_stream *urb_stream = jockey3_get_pcm_urb_stream(chip, direction);
	const char *type = direction == SNDRV_PCM_STREAM_PLAYBACK ? "Playback" : "Capture";
	bool log_onset = false, log_recovery = false;
	u64 now, last, started, age_ns, outage_ns = 0;
	u64 threshold_ms = JOCKEY3_WATCHDOG_STALL_MS;
	unsigned int grace_ms = jockey3_start_grace_ms();
	bool open = false, startup = false;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		/*
		 * One flag covers every deliberate stop -- rate change, suspend,
		 * pre_reset and teardown all reach jockey3_stop_urbs(). Checked
		 * before now/last/age_ns are sampled, not after: this tick can
		 * have been sitting on system_long_wq for a while before it got
		 * to run, and a stop+restart cycle -- which routinely takes on
		 * the order of 100 ms, far longer than this spinlock is ever
		 * held -- can complete inside that wait. Sampling first and
		 * checking stopping second, as this used to, let a tick whose
		 * data predated the stop act on it once stopping had already
		 * gone false again, misreporting a rate change's own deliberate
		 * silence as a stall. See re/rate_change_stall.md for the
		 * hardware traces this closed.
		 */
		if (urb_stream->stopping) {
			urb_stream->stall_reported = false;
			return;
		}

		/*
		 * Read both candidate baselines before sampling @now, whichever
		 * one ends up used below: a completion landing on another CPU
		 * between reading a baseline and reading @now would otherwise let
		 * "now - last" underflow to a u64 near its max instead of going
		 * negative. Reading both first guarantees @now is taken no
		 * earlier than either.
		 */
		started = atomic64_read(&urb_stream->urbs_started_time);
		last = atomic64_read(&urb_stream->last_callback_time);
		now = ktime_get_mono_fast_ns();

		if (!started)
			return;		/* never started; nothing to watch yet */

		if (now - started < (u64)grace_ms * NSEC_PER_MSEC) {
			/*
			 * Still inside the fixed post-restart grace window --
			 * measured from urbs_started_time regardless of whether
			 * last_callback_time has already advanced. See this
			 * function's kernel-doc for why an early completion is
			 * not treated as proof the window is over.
			 */
			last = started;
			threshold_ms = grace_ms;
			startup = true;
		} else if (!last) {
			/* Past the grace window and still never completed once */
			last = started;
		}

		age_ns = now - last;

		open = urb_stream->substream;

		if (age_ns > threshold_ms * NSEC_PER_MSEC) {
			if (!urb_stream->stall_reported) {
				urb_stream->stall_reported = true;
				urb_stream->stall_since = last;
				log_onset = true;
			}
		} else if (urb_stream->stall_reported) {
			urb_stream->stall_reported = false;
			/*
			 * @last is the completion that ended the outage and
			 * @stall_since the one before it started, so the
			 * difference is the true gap rather than a rounding of
			 * it to the poll interval.
			 */
			outage_ns = last - urb_stream->stall_since;
			log_recovery = true;
		}
	}

	if (log_onset) {
		dev_warn(&chip->intf0->dev,
			 "%s URB stream stalled: no completion for %llu ms (%d URBs in flight, substream %s, %s)\n",
			 type, div_u64(age_ns, NSEC_PER_MSEC),
			 atomic_read(&urb_stream->urbs_in_flight),
			 open ? "open" : "idle",
			 startup ? "startup" : "steady-state");
		if (startup)
			dev_dbg(&chip->intf0->dev,
				"%s stall was inside the %s start grace (%u ms)\n", type,
				READ_ONCE(chip->warm_start) ? "warm" : "cold", grace_ms);
	} else if (log_recovery) {
		dev_warn(&chip->intf0->dev, "%s URB stream recovered after %llu ms\n",
			 type, div_u64(outage_ns, NSEC_PER_MSEC));
	}

	if (log_onset && (direction == SNDRV_PCM_STREAM_PLAYBACK || open))
		jockey3_recover_urb_stream(chip, direction, "watchdog", true);
}

/**
 * jockey3_watchdog_work() - periodic URB liveness check
 * @work: the chip's watchdog_work
 *
 * Every error path in this driver hangs off a URB completion, so a device that
 * simply stops completing URBs is invisible to all of them: nothing runs, so
 * nothing is logged. A playback stream in that state does not even produce an
 * xrun, because the hardware pointer never advances far enough to overtake the
 * application. This is the only place that can notice such a silence, which is
 * why it runs for the device's whole lifetime rather than only while a PCM
 * stream is open -- the URBs do too, since MIDI OUT rides in every playback
 * packet and there is no idle state in which "no completions" is legitimate.
 *
 * Acts as well as detects: jockey3_watchdog_check() calls
 * jockey3_recover_urb_stream() directly on a new stall onset. This is the
 * only place recovery can be triggered without some PCM ioctl (hw_params,
 * prepare) re-entering the driver first -- necessary because a long-running,
 * uninterrupted stream never re-enters otherwise, and ALSA core's own
 * wait_for_avail() (sound/core/pcm_lib.c) times out the open substream with
 * -EIO on its own schedule regardless of whether this driver ever notices.
 * jockey3_watchdog_arm()'s cadence tightens while a PCM stream is open, for
 * exactly this reason.
 *
 * Calling into jockey3_recover_urb_stream() from here is safe even though it
 * calls jockey3_stop_urbs(), which disarms this same work item: the disarm is
 * the non-sync cancel_delayed_work(), which never blocks and only prevents a
 * future queueing -- it does not affect the tick that is already running
 * (this one). The reset jockey3_recover_urb_stream() may queue runs on
 * system_wq (drivers/usb/core/message.c's usb_queue_reset_device()), not
 * system_long_wq where this tick runs, so the two cannot serialize behind
 * each other; and rate_mutex is not held across the wait for that reset
 * (jockey3_recover_urb_stream() drops it first), which is what lets
 * jockey3_pre_reset()/jockey3_post_reset() take the mutex themselves and
 * complete while this tick is blocked waiting.
 */
static void jockey3_watchdog_work(struct work_struct *work)
{
	struct jockey3_chip *chip = container_of(to_delayed_work(work),
						 struct jockey3_chip, watchdog_work);

	/*
	 * Suspend as well as teardown: jockey3_stop_urbs() disarms with the
	 * non-sync cancel_delayed_work(), which leaves a tick that is already
	 * running to finish. Without this it would requeue itself and keep
	 * ticking for the whole suspend, for no purpose -- every check below
	 * bails out on the suspended flag anyway. Resume re-arms via
	 * jockey3_start_urbs().
	 */
	if (jockey3_is_disconnected(chip) || jockey3_is_suspended(chip))
		return;

	/*
	 * A reset stops and restarts the URBs from the USB core's own workqueue.
	 * Sampling in the middle of that would report a stall that is both
	 * expected and already being dealt with.
	 */
	if (!jockey3_is_resetting(chip)) {
		jockey3_watchdog_check(chip, SNDRV_PCM_STREAM_PLAYBACK);
		jockey3_watchdog_check(chip, SNDRV_PCM_STREAM_CAPTURE);
	}

	jockey3_watchdog_arm(chip);
}

/*
 * Poll a stream for up to timeout_ms. With @require_healthy false the bar is
 * jockey3_check_urb_stream_alive() -- "something completed just now", enough
 * for the cold-start paths that only ask whether the ring came up. With it
 * true the bar is jockey3_stream_streaming_healthy(), which a trickle of
 * FIFO-drain completions cannot clear; used where a false pass would escalate
 * to a needless USB reset. Always logs a dev_warn on timeout regardless of
 * whether the caller acts on the result, so stall frequency (Playback and
 * Capture alike) stays trackable in the field via dmesg.
 */
/*
 * @fresh_start: this wait follows a jockey3_start_urbs() the caller just did, so
 *	the confirmation latency is reported as the time from that restart to the
 *	first completion (@urbs_started_time -> @first_callback_time), not from
 *	when this poll loop happened to begin. The two directions are checked one
 *	after the other, so measuring from loop entry would credit the whole of
 *	the first direction's confirmation to the second direction's start time
 *	and under-report it -- Capture routinely printed "after 0 ms" only
 *	because Playback had already burned the first several ms. Pass false for a
 *	bare .prepare() liveness check on an already-running ring, where
 *	@urbs_started_time is stale and the meaningful number is how long this
 *	call spent polling.
 */
static bool jockey3_wait_urb_stream_started(struct jockey3_chip *chip, const int direction,
					    const unsigned int timeout_ms, bool require_healthy,
					    bool fresh_start)
{
	struct jockey3_pcm_urb_stream *urb_stream = jockey3_get_pcm_urb_stream(chip, direction);
	const char *type = direction == SNDRV_PCM_STREAM_PLAYBACK ? "Playback" : "Capture";
	u64 since_ns = fresh_start ? atomic64_read(&urb_stream->urbs_started_time) : 0;
	unsigned long start = jiffies;
	unsigned long deadline = start + msecs_to_jiffies(timeout_ms);
	u64 first, last;

	dev_dbg(&chip->intf0->dev, "Waiting up to %u ms for %s to %s\n",
		timeout_ms, type, require_healthy ? "stream steadily" : "show liveness");

	while (time_before(jiffies, deadline)) {
		/*
		 * A device that went down mid-wait completes nothing until it is
		 * restored; spinning out the grace only delays the caller's next
		 * decision, which in the recovery path is whether to reset.
		 */
		if (jockey3_is_disconnected(chip) || jockey3_is_suspended(chip))
			return false;

		if (require_healthy ? jockey3_stream_streaming_healthy(chip, urb_stream)
				    : jockey3_check_urb_stream_alive(urb_stream)) {
			u64 mark;
			unsigned int elapsed_ms;

			first = atomic64_read(&urb_stream->first_callback_time);
			if (since_ns && first >= since_ns)
				mark = first;
			else if (since_ns)
				mark = ktime_get_mono_fast_ns();
			else
				mark = 0;
			elapsed_ms = mark ? div_u64(mark - since_ns, NSEC_PER_MSEC)
					  : jiffies_to_msecs(jiffies - start);

			dev_dbg(&chip->intf0->dev, "%s confirmed %s after %u ms\n", type,
				require_healthy ? "streaming" : "alive", elapsed_ms);
			return true;
		}

		usleep_range(500, 2000);
	}

	dev_warn(&chip->intf0->dev, "%s URB has stalled.\n", type);

	/* What the grace saw when it gave up: too few completions is silence or
	 * trickle, many with a tight first->last span is a burst that stopped.
	 */
	first = atomic64_read(&urb_stream->first_callback_time);
	last = atomic64_read(&urb_stream->last_callback_time);
	dev_dbg(&chip->intf0->dev,
		"%s stall detail: %d completions since start, first->last %llu us, %d URBs in flight\n",
		type, atomic_read(&urb_stream->completions_since_start),
		(first && last > first) ? div_u64(last - first, NSEC_PER_USEC) : 0,
		atomic_read(&urb_stream->urbs_in_flight));
	return false;
}

/**
 * jockey3_recover_urb_stream() - bring a stalled direction back
 * @chip: driver state
 * @direction: SNDRV_PCM_STREAM_PLAYBACK or SNDRV_PCM_STREAM_CAPTURE
 * @context: short description of what found the stall, for the log
 * @report_xrun: report an xrun on both directions' open substreams once
 *	recovery is committed to -- both, since the ring is shared regardless of
 *	which direction stalled. True only when a running stream just lost
 *	continuity; false when the caller already expects the discontinuity (a
 *	rate change) or nothing has flowed yet (.prepare).
 *
 * Shared by jockey3_pcm_hw_params()'s post-rate-change check,
 * jockey3_pcm_prepare()'s liveness check and jockey3_watchdog_check()'s
 * mid-stream detection. Every call site confirms the stall first, and a
 * direction found alive at entry returns immediately, so two sequential calls
 * for one stall are cheap.
 *
 * Concurrent calls need more: two can both pass the alive check before either
 * restarts anything, then run competing stop/start sequences or resets.
 * @chip->recovery_in_progress closes that -- one ladder at a time, chip-wide.
 * Without it a watchdog tick and a racing jockey3_pcm_prepare() retry collide
 * and produce -EPROTO on every endpoint, needing a re-enumeration to clear.
 *
 * Ladder: a lightweight URB stop/start, then a full USB reset if that did not
 * take, queued with usb_queue_reset_device() and awaited by
 * jockey3_wait_for_reset_completion() rather than calling usb_reset_device()
 * from ioctl context -- see there for why. The reset step is gated on
 * jockey3_recovery_budget_take(), so a chip that keeps stalling is not reset
 * in a tight loop.
 *
 * The onset line is ratelimited: jockey3_pcm_prepare() runs on every xrun
 * recovery, so a client looping on a wedged stream re-enters here several
 * times a second, and the budget bounds resets, not log lines.
 *
 * Return: 0 if the direction is confirmed alive, recovery gave up and logged
 * why (still non-fatal to the caller), or a concurrent call already had this
 * in hand; -ENODEV if the device is gone; or -EAGAIN if a reset was queued
 * but did not complete in time.
 */
static int jockey3_recover_urb_stream(struct jockey3_chip *chip, const int direction,
				      const char *context, bool report_xrun)
{
	struct jockey3_pcm_urb_stream *urb_stream = jockey3_get_pcm_urb_stream(chip, direction);
	const char *type = direction == SNDRV_PCM_STREAM_PLAYBACK ? "Playback" : "Capture";
	unsigned int grace;
	int ret = 0;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	if (jockey3_check_urb_stream_alive(urb_stream))
		return 0;

	if (atomic_cmpxchg(&chip->recovery_in_progress, 0, 1) != 0) {
		dev_dbg(&chip->intf0->dev,
			"%s stream stalled (%s); another recovery is already in progress, skipping\n",
			type, context);
		return 0;
	}

	dev_warn_ratelimited(&chip->intf0->dev,
			     "%s stream stalled (%s); restarting URBs to recover\n",
			     type, context);

	/*
	 * Even the light restart below discards in-flight buffer state, so
	 * sample-accuracy is already broken the moment recovery is committed
	 * to -- report it now rather than waiting to see how far the ladder
	 * escalates. Both directions, not just @direction: jockey3_stop_urbs()/
	 * jockey3_start_urbs() below tear down and resubmit the *shared* ring
	 * unconditionally, so an open sibling substream loses continuity too,
	 * even though it was never found stalled itself. Found on the bench: an
	 * open Capture stream died with its own -EIO from ALSA core's
	 * wait_for_avail() timeout while a Playback-triggered recovery was still
	 * in flight, because only Playback had been told. jockey3_report_xrun()
	 * itself is a no-op if a given substream is not open and running, so
	 * calling it on both unconditionally is safe.
	 */
	if (report_xrun) {
		jockey3_report_xrun(&chip->playback);
		jockey3_report_xrun(&chip->capture);
	}

	scoped_guard(mutex, &chip->rate_mutex) {
		/*
		 * Something else may have already fixed this while the call above
		 * was waiting for rate_mutex -- most plausibly
		 * jockey3_pcm_hw_params()'s own rate-change restart, which holds
		 * this same mutex across its whole stop/set-rate/start sequence.
		 * The alive check above this function's entry is stale by the
		 * time the lock is held; re-checking here is what makes a call
		 * that raced a legitimate recovery a no-op instead of a second,
		 * redundant full URB-ring teardown/rebuild landing right on top
		 * of one that already succeeded. Without this re-check the
		 * redundant restart can miss its own liveness budget and escalate
		 * to a full USB reset that a change which had already recovered on
		 * its own never needed. See re/rate_change_stall.md.
		 */
		if (jockey3_check_urb_stream_alive(urb_stream)) {
			dev_dbg(&chip->intf0->dev,
				"%s stream came back under rate_mutex (%s); no restart needed\n",
				type, context);
			goto out;
		}

		/*
		 * Stale for the same reason the alive check above is repeated: a
		 * tick that blocked here may have been waiting on
		 * jockey3_suspend() or jockey3_pre_reset(), and wakes to a
		 * stream that looks dead only because they zeroed the
		 * timestamps. Restarting on that would submit URBs to a
		 * suspended device, then escalate to resetting one.
		 *
		 * Bailing loses nothing: both paths restart the ring themselves
		 * on the way back up.
		 */
		if (jockey3_is_disconnected(chip) || jockey3_is_suspended(chip) ||
		    jockey3_is_resetting(chip)) {
			dev_dbg(&chip->intf0->dev,
				"%s stream stalled (%s), but the device is being taken down; leaving the restart to the resume path\n",
				type, context);
			goto out;
		}

		jockey3_stop_urbs(chip);
		jockey3_start_urbs_failed(chip, jockey3_start_urbs(chip, true), context);
	}

	/*
	 * Start grace, and the strict health gate: the light restart above was
	 * of a ring that was streaming moments earlier, so a real resume shows
	 * a proper completion cadence quickly, while a trickle of implausibly
	 * fast FIFO-drain completions must NOT count -- passing on those
	 * escalates a merely jittery restart to the USB reset below.
	 */
	grace = jockey3_start_grace_ms();
	if (jockey3_wait_urb_stream_started(chip, direction, grace, true, true)) {
		dev_dbg(&chip->intf0->dev,
			"%s stream recovered via light URB restart (%s)\n", type, context);
		goto out;
	}

	/*
	 * Re-tested because the restart and grace above take long enough for the
	 * device to go away underneath them, and escalating then goes badly.
	 * jockey3_queue_reset() would reinit_completion() over the complete_all()
	 * that jockey3_disconnect() issues to release waiters, and block the full
	 * timeout on a reset that will never come. Ahead of the budget, so this
	 * spends no attempt.
	 *
	 * Suspended as well as disconnected, and this is not the same check as
	 * the one under rate_mutex above. That one covers a tick that arrives
	 * after jockey3_suspend() has run. A tick which passed it earlier, while
	 * the device was still live, is legitimately mid-recovery and reaches
	 * here only after its grace has elapsed -- by which time suspend may have
	 * happened. Resetting from there was observed to queue a reset the
	 * suspended device could not complete, leaving the resume path waiting
	 * out jockey3_wait_for_reset_completion() twice over.
	 *
	 * Racy by construction -- either state arriving just after the test still
	 * hits it. The timeout bounds that; closing it properly would mean
	 * serializing against jockey3_disconnect(), which must not block on
	 * driver locks.
	 */
	if (jockey3_is_disconnected(chip) || jockey3_is_suspended(chip)) {
		dev_dbg(&chip->intf0->dev,
			"%s stream still stalled after URB restart, but the device is down; not resetting (%s)\n",
			type, context);
		ret = -ENODEV;
		goto out;
	}

	if (!jockey3_recovery_budget_take(chip)) {
		dev_err(&chip->intf0->dev,
			"%s stream still stalled after URB restart; recovery budget exhausted, not resetting (%s)\n",
			type, context);
		goto out;
	}

	dev_warn(&chip->intf0->dev,
		 "%s stream still stalled after URB restart; queuing full USB reset (%s), attempt %d/%d in window\n",
		 type, context, atomic_read(&chip->recovery_attempts),
		 JOCKEY3_RECOVERY_MAX_ATTEMPTS);
	jockey3_queue_reset(chip);

	ret = jockey3_wait_for_reset_completion(chip);
	if (ret < 0)
		goto out;

	grace = jockey3_start_grace_ms();
	if (!jockey3_wait_urb_stream_started(chip, direction, grace, false, true))
		dev_err(&chip->intf0->dev,
			"%s stream still stalled after full USB reset; hardware may need power-cycling (%s)\n",
			type, context);
	else
		dev_dbg(&chip->intf0->dev,
			"%s stream recovered after full USB reset (%s)\n", type, context);

out:
	atomic_set(&chip->recovery_in_progress, 0);
	return ret;
}

/*
 * SNDRV_PCM_INFO_BATCH: the hardware pointer only advances once per completed
 * bulk URB (a whole Ploytec frame group), never per sample, so userspace must
 * not assume a sample-accurate position.
 *
 * SNDRV_PCM_INFO_RESUME is deliberately absent: the device loses stream
 * synchronization across a suspend, so the ALSA core should return -ESTRPIPE
 * and have userspace re-prepare rather than issue TRIGGER_RESUME.
 */
#define JOCKEY3_PCM_INFO	(SNDRV_PCM_INFO_MMAP |		\
				 SNDRV_PCM_INFO_INTERLEAVED |	\
				 SNDRV_PCM_INFO_BLOCK_TRANSFER |\
				 SNDRV_PCM_INFO_BATCH |		\
				 SNDRV_PCM_INFO_MMAP_VALID)

#define JOCKEY3_PCM_RATES	(SNDRV_PCM_RATE_44100 |	\
				 SNDRV_PCM_RATE_48000 |	\
				 SNDRV_PCM_RATE_88200 |	\
				 SNDRV_PCM_RATE_96000)

static const struct snd_pcm_hardware jockey3_pcm_hw_playback = {
	.info			= JOCKEY3_PCM_INFO,
	.formats		= SNDRV_PCM_FMTBIT_S24_3LE,
	.rates			= JOCKEY3_PCM_RATES,
	.rate_min		= 44100,
	.rate_max		= 96000,
	.channels_min		= 4,
	.channels_max		= 4,
	.buffer_bytes_max	= 1024 * 1024,
	/*
	 * One playback URB carries a packet count chosen per open by
	 * jockey3_pcm_hw_params(), of 10 frames * 4 channels * 3 bytes = 120
	 * bytes each. period_elapsed is OR'd across the packet loop in
	 * jockey3_playback_callback(), so the minimum must cover at least one
	 * packet, the smallest a URB can ever carry (N=1), or a period boundary
	 * inside a URB could be missed entirely.
	 */
	.period_bytes_min	= PLOYTEC_PLAYBACK_FRAMES * 4 * 3,
	.period_bytes_max	= 512 * 1024,
	.periods_min		= 2,
	.periods_max		= 1024,
};

static const struct snd_pcm_hardware jockey3_pcm_hw_capture = {
	.info			= JOCKEY3_PCM_INFO,
	.formats		= SNDRV_PCM_FMTBIT_S24_3LE,
	.rates			= JOCKEY3_PCM_RATES,
	.rate_min		= 44100,
	.rate_max		= 96000,
	.channels_min		= 6,
	.channels_max		= 6,
	.buffer_bytes_max	= 1024 * 1024,
	/*
	 * One capture URB carries a packet count chosen per open (see
	 * jockey3_pcm_hw_params()) of up to 8 frames * 6 channels * 3 bytes =
	 * 144 bytes each; see the playback .period_bytes_min comment above for
	 * why the minimum must cover at least one packet.
	 */
	.period_bytes_min	= PLOYTEC_CAPTURE_FRAMES * 6 * 3,
	.period_bytes_max	= 512 * 1024,
	.periods_min		= 2,
	.periods_max		= 1024,
};

static int jockey3_pcm_open(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	int ret;

	dev_dbg(&chip->intf0->dev, "PCM open stream %d\n", substream->stream);

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	runtime->hw = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		      jockey3_pcm_hw_playback : jockey3_pcm_hw_capture;

	/* The period accounting assumes a whole number of periods per buffer */
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	/*
	 * Rate constraints under rate_mutex, which also excludes a concurrent
	 * rate change in jockey3_pcm_hw_params(). Re-check for disconnect here:
	 * the check above raced with anything that happened while we were not
	 * holding the mutex.
	 */
	scoped_guard(mutex, &chip->rate_mutex) {
		if (jockey3_is_disconnected(chip))
			return -ENODEV;

		if (jockey3_rate_committed_streams(chip) > 0) {
			/* Force the new stream to match the existing hardware rate */
			ret = snd_pcm_hw_constraint_single(runtime,
							   SNDRV_PCM_HW_PARAM_RATE,
							   chip->current_rate);
			if (ret < 0)
				return ret;
		}
	}

	/* Substream registration under spinlock to ensure memory consistency to the ISR*/
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		urb_stream->substream = substream;
	}

	return 0;
}

static int jockey3_pcm_close(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	unsigned int default_n = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
				 JOCKEY3_PLAYBACK_N : JOCKEY3_CAPTURE_N;

	dev_dbg(&chip->intf0->dev, "PCM close stream %d\n", substream->stream);

	/*
	 * No drain is needed here: the ALSA core has already run .sync_stop via
	 * snd_pcm_release_substream() -> snd_pcm_drop() -> do_hw_free(), which is
	 * also where runtime->dma_area is released.
	 */
	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		urb_stream->substream = NULL;
		urb_stream->running = false;
		urb_stream->rate_committed = false;

		/*
		 * A closed direction is not covered by jockey3_pcm_hw_params()
		 * any more, so its N would otherwise sit at whatever the last
		 * open left it at -- possibly a small, period-tuned value that
		 * has nothing to do with the next stream to open it, or with
		 * this direction's own idle ring being re-armed by a rate
		 * change on the *other* direction (jockey3_start_urbs() re-arms
		 * both unconditionally). Reset to the default here so an idle
		 * direction always re-arms at a safe N.
		 */
		WRITE_ONCE(urb_stream->n_shift, ilog2(default_n));
		WRITE_ONCE(urb_stream->n_pkts, default_n);
	}

	return 0;
}

/**
 * jockey3_pcm_sync_stop() - wait for in-flight URB callbacks to drain
 * @substream: the substream being stopped
 *
 * Waits for every URB completion that could still touch this stream's substream
 * or runtime->dma_area to leave its safe zone.
 *
 * The ALSA core calls this from snd_pcm_do_prepare() and from do_hw_free()
 * *before* snd_pcm_lib_free_pages() releases the buffer, and only when the
 * stream actually ran (runtime->stop_operating). It runs in process context
 * with no stream lock held, so sleeping here is safe: a callback in its safe
 * zone has dropped urb_stream->lock and may freely take the stream lock, so the
 * stream lock -> urb_stream->lock order is never inverted.
 *
 * Return: 0 always; a timeout is logged but cannot usefully be reported.
 */
static int jockey3_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	long remaining;

	spin_lock_irq(&urb_stream->lock);
	remaining = wait_event_lock_irq_timeout(urb_stream->drain_wait,
						urb_stream->callbacks_active == 0,
						urb_stream->lock,
						msecs_to_jiffies(1000));
	spin_unlock_irq(&urb_stream->lock);

	if (!remaining)
		dev_err(&chip->intf0->dev,
			"Timeout draining %s URB callbacks\n",
			substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture");

	return 0;
}

static int jockey3_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	bool stalled = false;
	int ret = 0;

	dev_dbg(&chip->intf0->dev, "PCM prepare stream %d\n", substream->stream);
	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	ret = jockey3_wait_for_reset_completion(chip);
	if (ret < 0)
		return ret;

	/*
	 * Taking rate_mutex here serializes against an in-flight rate change in
	 * jockey3_pcm_hw_params(), which holds it across the whole stop/set/start
	 * sequence -- so the liveness below is sampled from a settled state
	 * rather than from the middle of a URB restart.
	 */
	scoped_guard(mutex, &chip->rate_mutex) {
		if (jockey3_is_disconnected(chip))
			return -ENODEV;

		/*
		 * Either direction may have been left stalled by an earlier rate
		 * change that happened while this stream was not open (see
		 * jockey3_pcm_hw_params()). Catch it here, before the stream that
		 * is being prepared starts relying on it.
		 *
		 * This single sample is only a hint. jockey3_check_urb_stream_alive()'s
		 * window is scaled to always span a handful of packet intervals
		 * regardless of N (JOCKEY3_LIVENESS_WINDOW_NS()), but a handful is
		 * still not many -- one preemption is enough to make a healthy
		 * stream read as dead -- and .prepare runs on every xrun recovery,
		 * where acting on a false positive would disrupt a working stream.
		 * What it flags is confirmed below before anything is done about it.
		 */
		stalled = !jockey3_check_urb_stream_alive(urb_stream);
	}

	/*
	 * Confirm, and recover, outside rate_mutex on purpose: polling would
	 * otherwise hold the mutex for a whole start grace, and recovery may
	 * escalate to a queued USB reset, whose jockey3_pre_reset() and
	 * jockey3_post_reset() need to acquire the mutex themselves to complete.
	 * Start grace and the plain alive check: this is a stream being
	 * (re)opened, not a warm restart of one that was just running.
	 */
	if (stalled)
		stalled = !jockey3_wait_urb_stream_started(chip, substream->stream,
							  jockey3_start_grace_ms(), false,
							  false);

	if (stalled) {
		const char *context = substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
			"opening a capture stream" : "preparing a playback stream";

		ret = jockey3_recover_urb_stream(chip, substream->stream, context, false);
		if (ret < 0)
			return ret;
	}

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		urb_stream->dma_off = 0;
		urb_stream->period_off = 0;
	}
	return 0;
}

/*
 * Called by the ALSA core with the substream stream lock held and interrupts
 * disabled, so this callback must never sleep. In particular it must not wait
 * for a rate change or a device reset: a URB completion can re-enter here via
 * snd_pcm_period_elapsed() -> snd_pcm_stop_xrun(), and blocking would then
 * stall the very rate change it is waiting on.
 *
 * Serialization against a concurrent rate change is provided by rate_mutex in
 * the sleepable callbacks instead; the ALSA state machine guarantees .prepare
 * runs before TRIGGER_START and after every XRUN.
 *
 * Lock order here is snd_pcm_stream_lock -> urb_stream->lock, which is why the
 * URB callbacks drop urb_stream->lock before calling snd_pcm_period_elapsed().
 */
static int jockey3_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);

	dev_dbg(&chip->intf0->dev, "PCM trigger stream %d, cmd %d\n", substream->stream, cmd);

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
			urb_stream->running = true;
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			urb_stream->running = false;
			break;
		default:
			return -EINVAL;
		}
	}
	return 0;
}

static snd_pcm_uframes_t jockey3_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	unsigned int dma_off;

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		dma_off = urb_stream->dma_off;
	}
	return bytes_to_frames(substream->runtime, dma_off);
}

static int jockey3_initialize_ploytec(struct jockey3_chip *chip, u32 *fw_version)
{
	int ret;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	ret = ploytec_initialize_device(chip->intf0, chip->xfer_buf, false, fw_version);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Ploytec failed to initialize: %d\n", ret);
		return ret;
	}

	ret = ploytec_start_streaming(chip->intf0, chip->xfer_buf);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Ploytec failed to start streaming: %d\n", ret);
		return ret;
	}

	dev_dbg(&chip->intf0->dev, "Ploytec initialized successfully\n");
	return 0;
}

/**
 * jockey3_pcm_set_n() - choose and arm this direction's packets per URB
 * @chip: driver state
 * @substream: the substream hw_params was just called for
 * @hw_params: the negotiated parameters
 *
 * Chooses how many Ploytec packets each USB bulk URB on this direction
 * carries, called "N" below for brevity: the largest power of two with
 * N * pkt_bytes <= period_bytes. A big period gets the full coalescing
 * saving, a tiny one falls back to N=1 automatically via .period_bytes_min.
 * Takes effect without tearing the URB ring down -- the firmware cannot tell
 * an N x 512 B bulk transfer apart from N separate 512 B ones, so each URB on
 * this direction simply picks up the new N at its own next resubmission; the
 * ring turns over onto it within at most JOCKEY3_N_URBS completions. See
 * re/streaming_overhead.md for the rationale and measurements.
 *
 * Called unconditionally on every hw_params(), including when the rate is
 * unchanged and the URB ring is never touched: N is a per-open, per-period
 * property, independent of whether a rate change happens to be in progress.
 */
static void jockey3_pcm_set_n(struct jockey3_chip *chip, struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *hw_params)
{
	struct jockey3_pcm_urb_stream *urb_stream =
		jockey3_get_pcm_urb_stream(chip, substream->stream);
	unsigned int period_bytes = params_period_bytes(hw_params);
	bool is_playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int pkt_bytes = is_playback ? PLOYTEC_PLAYBACK_FRAMES * 4 * 3 :
						  PLOYTEC_CAPTURE_FRAMES * 6 * 3;
	unsigned int max_n = is_playback ? JOCKEY3_PLAYBACK_N : JOCKEY3_CAPTURE_N;
	unsigned int n = max(period_bytes / pkt_bytes, 1U);
	u8 n_shift = clamp_t(u8, ilog2(n), 0, ilog2(max_n));

	scoped_guard(spinlock_irqsave, &urb_stream->lock) {
		WRITE_ONCE(urb_stream->n_shift, n_shift);
		WRITE_ONCE(urb_stream->n_pkts, 1 << n_shift);
	}

	dev_dbg(&chip->intf0->dev, "hw_params: %s using %u packet(s)/URB (period_bytes=%u)\n",
		is_playback ? "playback" : "capture", 1U << n_shift, period_bytes);
}

/**
 * jockey3_pcm_change_rate() - reprogram the hardware rate for hw_params()
 * @chip: driver state
 * @substream: the substream asking for the rate
 * @rate: the requested rate in Hz
 * @changed: set true if the hardware was actually reprogrammed
 *
 * Split out of jockey3_pcm_hw_params() because nesting two scoped_guard()
 * blocks in one scope shadows the guard's own variable and trips -Wshadow.
 * Call with the USB device lock held; see the call site for why.
 *
 * Return: 0 on success or if the rate already matched (@changed stays false),
 * %-EBUSY if the other direction holds a different rate, %-ENODEV if the device
 * is gone, or the error from programming the rate.
 */
static int jockey3_pcm_change_rate(struct jockey3_chip *chip,
				   struct snd_pcm_substream *substream,
				   unsigned int rate, bool *changed)
{
	int ret;

	/*
	 * rate_mutex is held across the whole stop/set-rate/start sequence, which
	 * is what excludes a concurrent rate change from another substream: any
	 * other sleepable path that needs a settled rate takes the same mutex.
	 */
	guard(mutex)(&chip->rate_mutex);

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	/*
	 * Drop this direction's own claim on the rate before counting:
	 * an application is free to hw_free and run hw_params again at
	 * a different rate, and its previous commitment must not be
	 * what blocks it. Cleared inside rate_mutex, so the other
	 * direction cannot observe the gap and reprogram the hardware.
	 */
	jockey3_set_rate_committed(chip, substream->stream, false);

	if (chip->current_rate == rate) {
		dev_dbg(&chip->intf0->dev, "Rate already set to %u, skipping change\n",
			rate);
		jockey3_set_rate_committed(chip, substream->stream, true);
		return 0;
	}

	*changed = true;

	/*
	 * The other direction already holds the rate. The ALSA core
	 * should have enforced the constraint from jockey3_pcm_open(),
	 * so this is a backstop -- but it is also the only check that
	 * covers a stream which opened while the device was idle and
	 * only now asks for a rate the other direction has since fixed.
	 */
	if (jockey3_rate_committed_streams(chip) > 0) {
		dev_err(&chip->intf0->dev,
			"Cannot change rate to %u while the other stream holds %u\n",
			rate, chip->current_rate);
		return -EBUSY;
	}

	jockey3_stop_urbs(chip);

	ret = jockey3_set_rate(chip, rate, false);
	if (ret != 0) {
		if (jockey3_err_device_gone(ret))
			dev_dbg(&chip->intf0->dev,
				"Rate change to %u abandoned; device is gone (%d)\n",
				rate, ret);
		else
			dev_err(&chip->intf0->dev,
				"Rate change to %u failed: %d\n", rate, ret);
		/*
		 * The rate change is what left the endpoints disabled if
		 * they are, so this restart is the one most likely to
		 * come back -ENOENT. Report it before returning the rate
		 * error, which would otherwise be the only thing seen.
		 */
		jockey3_start_urbs_failed(chip, jockey3_start_urbs(chip, false),
					  "a failed rate change");
		return ret;
	}

	jockey3_set_current_rate(chip, rate);
	jockey3_set_rate_committed(chip, substream->stream, true);

	jockey3_start_urbs_failed(chip, jockey3_start_urbs(chip, false), "a rate change");

	return 0;
}

static int jockey3_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);
	unsigned int rate = params_rate(hw_params);
	bool playback_alive, capture_alive, capture_open;
	bool rate_changed = false;
	unsigned int grace;
	int ret = 0;

	dev_dbg(&chip->intf0->dev, "PCM hw_params rate %u, rate_committed_streams %d\n",
		rate, jockey3_rate_committed_streams(chip));

	if (jockey3_is_disconnected(chip))
		return -ENODEV;

	/*
	 * A previous call may have left a stall-recovery reset in flight (see
	 * jockey3_recover_urb_stream(), called below). Without this, a rate
	 * change arriving before that reset completes runs jockey3_set_rate()
	 * and the URB restart against a device that is mid-reset, which fails
	 * outright instead of recovering. jockey3_pcm_prepare() waits for the
	 * same reason.
	 */
	ret = jockey3_wait_for_reset_completion(chip);
	if (ret < 0)
		return ret;

	/* Independent of rate and of whether it changes; see jockey3_pcm_set_n() */
	jockey3_pcm_set_n(chip, substream, hw_params);

	/*
	 * Programming a rate calls usb_set_interface(), which the USB core
	 * serializes against usb_disconnect() with the device lock and nothing
	 * else; both reach remove_intf_ep_devs(), whose only guard is the
	 * unlocked bitfield intf->ep_devs_created.
	 *
	 * Three constraints fix where it goes. It cannot move down into
	 * ploytec_initialize_device(): probe, disconnect and the reset callbacks
	 * already hold it and it is not recursive. It must be dropped before the
	 * recovery below, which can queue a reset that needs this same lock. And
	 * it goes outside rate_mutex, the order pre_reset()/post_reset() use.
	 *
	 * The watchdog must never take it: jockey3_disconnect() holds it while
	 * waiting in cancel_delayed_work_sync().
	 */
	scoped_guard(device, &chip->dev->dev)
		ret = jockey3_pcm_change_rate(chip, substream, rate, &rate_changed);
	if (ret < 0)
		return ret;
	if (!rate_changed)
		return 0;

	/*
	 * The firmware does not always resume streaming after a rate change, so
	 * check liveness on both directions and let
	 * jockey3_recover_urb_stream() force a re-synchronization.
	 *
	 * Playback recovers unconditionally -- it carries MIDI OUT, so a stall
	 * breaks MIDI control. Capture only if a stream is open: an idle
	 * Capture stall is logged and deferred to the next open, since
	 * recovering it would glitch working Playback audio.
	 *
	 * Outside rate_mutex, because an escalated reset needs pre_reset() and
	 * post_reset() to take it. Playback goes first: if both died, its
	 * restart of the shared ring makes the capture call a no-op.
	 *
	 * Start grace -- a rate change reprograms the endpoints over EP0.
	 */
	grace = jockey3_start_grace_ms();
	playback_alive = jockey3_wait_urb_stream_started(chip, SNDRV_PCM_STREAM_PLAYBACK,
							 grace, false, true);
	capture_alive = jockey3_wait_urb_stream_started(chip, SNDRV_PCM_STREAM_CAPTURE,
							grace, false, true);
	capture_open = jockey3_stream_is_open(chip, SNDRV_PCM_STREAM_CAPTURE);

	if (!playback_alive || (!capture_alive && capture_open)) {
		dev_warn(&chip->intf0->dev,
			 "Rate change to %u Hz left a stream stalled (playback_alive=%d, capture_alive=%d, capture_open=%d); attempting recovery\n",
			 rate, playback_alive, capture_alive, capture_open);

		if (!playback_alive) {
			ret = jockey3_recover_urb_stream(chip, SNDRV_PCM_STREAM_PLAYBACK,
							 "rate change", false);
			if (ret < 0)
				return ret;
		}

		if (!capture_alive && capture_open) {
			ret = jockey3_recover_urb_stream(chip, SNDRV_PCM_STREAM_CAPTURE,
							 "rate change", false);
			if (ret < 0)
				return ret;
		}
	} else {
		if (!capture_alive)
			dev_dbg(&chip->intf0->dev,
				"Capture URB stalled after rate change to %u Hz, but no capture stream is open; deferring recovery to next capture open\n",
				rate);
		dev_dbg(&chip->intf0->dev, "Rate changed to %u successfully\n", rate);
	}

	return 0;
}

/*
 * Release this direction's claim on the hardware rate. The URB ring is
 * deliberately left running (see the top-of-file DOC); all this hands back is
 * the right of the other direction -- or of this one, on a re-run of
 * hw_params -- to program a different rate.
 */
static int jockey3_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct jockey3_chip *chip = snd_pcm_substream_chip(substream);

	jockey3_set_rate_committed(chip, substream->stream, false);
	return 0;
}

static const struct snd_pcm_ops jockey3_pcm_ops = {
	.open = jockey3_pcm_open,
	.close = jockey3_pcm_close,
	.hw_params = jockey3_pcm_hw_params,
	.hw_free = jockey3_pcm_hw_free,
	.prepare = jockey3_pcm_prepare,
	.trigger = jockey3_pcm_trigger,
	.sync_stop = jockey3_pcm_sync_stop,
	.pointer = jockey3_pcm_pointer,
};

static int jockey3_midi_in_open(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;
	return 0;
}

/*
 * Normally trigger(0) has already cleared the pointer, but the rawmidi core can
 * reach close without it (an interrupted drain, or close_substream() with
 * cleanup suppressed), so drop it here too rather than leave a dangling
 * reference for the URB callback.
 */
static int jockey3_midi_in_close(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	if (chip->midi_in_substream == substream)
		chip->midi_in_substream = NULL;

	return 0;
}

static void jockey3_midi_in_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	chip->midi_in_substream = up ? substream : NULL;
}

static int jockey3_midi_out_open(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	if (jockey3_is_disconnected(chip))
		return -ENODEV;
	return 0;
}

/* See jockey3_midi_in_close() for why the pointer is cleared here as well. */
static int jockey3_midi_out_close(struct snd_rawmidi_substream *substream)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	if (chip->midi_out_substream == substream)
		chip->midi_out_substream = NULL;

	return 0;
}

static void jockey3_midi_out_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct jockey3_chip *chip = substream->rmidi->private_data;

	guard(spinlock_irqsave)(&chip->midi_lock);
	chip->midi_out_substream = up ? substream : NULL;
}

static const struct snd_rawmidi_ops jockey3_midi_in_ops = {
	.open = jockey3_midi_in_open,
	.close = jockey3_midi_in_close,
	.trigger = jockey3_midi_in_trigger
};

static const struct snd_rawmidi_ops jockey3_midi_out_ops = {
	.open = jockey3_midi_out_open,
	.close = jockey3_midi_out_close,
	.trigger = jockey3_midi_out_trigger
};

static int jockey3_initialize(struct jockey3_chip *chip, int model)
{
	int ret;
	int rate;
	u32 fw_version;

	/*
	 * Let the device finish booting before speaking to it.
	 *
	 * After a mains power cycle the Jockey 3 enumerates and answers control
	 * transfers while its audio engine is still coming up. Initialize it in
	 * that window and the engine never starts: every control transfer
	 * succeeds, the rate reads back correctly, ALSA accepts playback, and the
	 * device is silent -- capture returns bit-exact zero and nothing is
	 * logged. A USB re-enumeration does not provoke it; only a real
	 * power-off does, because the device is self-powered and a VBUS cut is
	 * merely a cable unplug to it.
	 *
	 * The threshold is sharp: the device needs between 144 and 156 ms after
	 * enumeration, below which the engine fails to start every time. 250 ms
	 * rather than the smallest passing value, because the time this driver
	 * otherwise takes to reach its first transfer is host-dependent, so a
	 * value that only topped that up would erode on a faster machine.
	 *
	 * Placed here rather than in ploytec_initialize_device() because that
	 * runs twice per probe -- once below, once via jockey3_set_rate() -- and
	 * again on every rate change. This point runs once, and is the last
	 * before the driver's first EP0 transfer; everything above it in probe()
	 * is ALSA and USB core bookkeeping.
	 *
	 * See re/usb/init_timing_comparison.md for the measurements.
	 */
	msleep(250);

	for (int retry = 10; retry > 0; retry--) {
		ret = jockey3_initialize_ploytec(chip, &fw_version);
		if (ret == 0)
			break;
		usleep_range(50000, 100000); /* Wait 50-100 ms before retrying */
	}
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to initialize Ploytec: %d\n", ret);
		return ret;
	}

	// see ploytec_get_firmware() for the packing of buf[0..2] into fw_version
	dev_info(&chip->intf0->dev, CARD_NAME " %s Firmware 0x%02x v%d.%d.%d\n",
		 jockey3_model_name(model),
		 (fw_version >> 16) & 0xFF, (fw_version >> 8) & 0xFF,
		 (fw_version >> 4) & 0x0F, fw_version & 0x0F);

	rate = 44100;	// default sample rate at power-on
	scoped_guard(mutex, &chip->rate_mutex)
		jockey3_set_current_rate(chip, rate);

	ret = jockey3_set_rate(chip, rate, true);
	if (ret < 0)
		return ret;

	/*
	 * Fail the probe rather than escalating: a device that cannot accept its
	 * URBs has no working audio and no MIDI OUT, and queuing a reset against
	 * one that never started is worse than reporting a clean failure here.
	 */
	ret = jockey3_start_urbs(chip, false);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to start URBs during initialization: %d\n",
			ret);
		return ret;
	}

	dev_dbg(&chip->intf0->dev, "Initialization complete.\n");

	return 0;
}

/*
 * Free everything jockey3_probe() allocated into @chip, and give the card slot
 * back. NULL-safe throughout, so it is correct at any point of a half-built
 * probe as well as at the end of the device's life.
 *
 * Two rules govern what may go in here, both stemming from where it runs:
 * card->private_free, i.e. after the last file descriptor on the card is
 * closed, which on an unplug is arbitrarily long after jockey3_disconnect().
 *
 *  - No USB *operation* may be attempted. The objects are still allocated,
 *    since jockey3_probe() references each and this function drops them, but
 *    they are unbound by now and the hardware may be gone. Reading is safe,
 *    talking is not. The dev_dbg(&chip->intf0->dev, ...) idiom is avoided too:
 *    the interface is no longer ours, so the attribution would be wrong.
 *  - The URBs must already be dead. jockey3_disconnect() kills them, and so
 *    does probe's error path, both before the card is released.
 */
static void jockey3_free_resources(struct jockey3_chip *chip)
{
	int i;

	/*
	 * The last fence on the watchdog. jockey3_disconnect()'s sync cancel is
	 * not enough on its own: a jockey3_start_urbs() already past its
	 * DISCONNECTED gate arms the watchdog at its tail, under no mutex that
	 * would exclude the cancel. Here that cannot happen -- private_free runs
	 * once the card is closed, so every such caller has returned.
	 */
	cancel_delayed_work_sync(&chip->watchdog_work);

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		usb_free_urb(chip->playback.urbs[i]);
		kfree(chip->playback.bufs[i]);
		usb_free_urb(chip->capture.urbs[i]);
		kfree(chip->capture.bufs[i]);
	}
	usb_free_urb(chip->midi_in_urb);
	kfree(chip->midi_in_buf);
	kfree(chip->xfer_buf);

	mutex_destroy(&chip->rate_mutex);

	scoped_guard(mutex, &jockey3_devices_mutex)
		__clear_bit(chip->dev_idx, jockey3_devices_used);

	/* Last, reversing jockey3_probe(): everything above may read these. */
	usb_put_intf(chip->intf1);
	usb_put_intf(chip->intf0);
	usb_put_dev(chip->dev);
}

static void jockey3_card_free(struct snd_card *card)
{
	jockey3_free_resources(card->private_data);
}

static int jockey3_init_midi_urb(struct jockey3_chip *chip)
{
	struct usb_device *dev = chip->dev;

	memset(&chip->midi_state, 0, sizeof(chip->midi_state));
	chip->midi_out_acc = 0;

	chip->midi_in_buf = kmalloc(PLOYTEC_PKT_SIZE, GFP_KERNEL);
	if (!chip->midi_in_buf)
		return -ENOMEM;

	chip->midi_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!chip->midi_in_urb)
		return -ENOMEM;

	usb_fill_bulk_urb(chip->midi_in_urb, dev,
			  usb_rcvbulkpipe(dev, PLOYTEC_EP_NUM_MIDI_IN),
			  chip->midi_in_buf, PLOYTEC_PKT_SIZE,
			  jockey3_midi_in_callback, chip);

	return 0;
}

static int jockey3_init_playback_urbs(struct jockey3_chip *chip)
{
	struct usb_device *dev = chip->dev;
	int i;

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->playback.bufs[i] = kzalloc(JOCKEY3_PLAYBACK_XFER_SIZE, GFP_KERNEL);
		if (!chip->playback.bufs[i])
			return -ENOMEM;

		chip->playback.urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!chip->playback.urbs[i])
			return -ENOMEM;

		jockey3_init_out_packet(chip->playback.bufs[i]);

		usb_fill_bulk_urb(chip->playback.urbs[i], dev,
				  usb_sndbulkpipe(dev, PLOYTEC_EP_NUM_PCM_OUT),
				  chip->playback.bufs[i], JOCKEY3_PLAYBACK_XFER_SIZE,
				  jockey3_playback_callback, chip);
	}

	return 0;
}

static int jockey3_init_capture_urbs(struct jockey3_chip *chip)
{
	struct usb_device *dev = chip->dev;
	int i;

	for (i = 0; i < JOCKEY3_N_URBS; i++) {
		chip->capture.bufs[i] = kzalloc(JOCKEY3_CAPTURE_XFER_SIZE, GFP_KERNEL);
		if (!chip->capture.bufs[i])
			return -ENOMEM;

		chip->capture.urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!chip->capture.urbs[i])
			return -ENOMEM;

		usb_fill_bulk_urb(chip->capture.urbs[i], dev,
				  usb_rcvbulkpipe(dev, PLOYTEC_EP_NUM_PCM_IN),
				  chip->capture.bufs[i], JOCKEY3_CAPTURE_XFER_SIZE,
				  jockey3_capture_callback, chip);
	}

	return 0;
}

static bool jockey3_has_bulk_endpoint(struct usb_interface *intf, u8 ep_num, bool out)
{
	int i, j;

	for (i = 0; i < intf->num_altsetting; i++) {
		struct usb_host_interface *alts = &intf->altsetting[i];

		for (j = 0; j < alts->desc.bNumEndpoints; j++) {
			struct usb_endpoint_descriptor *epd = &alts->endpoint[j].desc;

			if (out) {
				if (usb_endpoint_is_bulk_out(epd) &&
				    usb_endpoint_num(epd) == ep_num)
					return true;
			} else {
				if (usb_endpoint_is_bulk_in(epd) &&
				    usb_endpoint_num(epd) == ep_num)
					return true;
			}
		}
	}
	return false;
}

static int jockey3_validate_endpoints(struct usb_interface *intf0, struct usb_interface *intf1)
{
	if (!jockey3_has_bulk_endpoint(intf0, PLOYTEC_EP_NUM_PCM_OUT, true) ||
	    !jockey3_has_bulk_endpoint(intf0, PLOYTEC_EP_NUM_MIDI_IN, false)) {
		dev_err(&intf0->dev, "Required bulk endpoints not found on Interface 0 (OUT: 0x%02x, IN: 0x%02x)\n",
			PLOYTEC_EP_NUM_PCM_OUT, PLOYTEC_EP_NUM_MIDI_IN);
		return -ENODEV;
	}

	if (!jockey3_has_bulk_endpoint(intf1, PLOYTEC_EP_NUM_PCM_IN, false)) {
		dev_err(&intf0->dev, "Required bulk IN endpoint not found on Interface 1 (IN: 0x%02x)\n",
			PLOYTEC_EP_NUM_PCM_IN);
		return -ENODEV;
	}
	return 0;
}

/*
 * Channel maps.
 *
 * This device exposes several independent stereo pairs, which is a different
 * thing from a multichannel speaker arrangement -- and speaker positions are
 * all the chmap enum can express. Claiming, say, RL/RR for the headphone pair
 * would invite an audio server to treat a cue output as rear speakers, so the
 * discrete pairs are reported as SNDRV_CHMAP_UNKNOWN.
 *
 * The one exception is the playback Master pair, which is marked FL/FR so that
 * userspace (PipeWire and friends) can identify the device's primary output.
 * There is no equivalent notion for the inputs, so capture is left entirely
 * unpositioned.
 *
 * Physical layout (see Documentation/sound/cards/jockey3.rst):
 *   Playback: 1-2 Master Out L/R, 3-4 Headphone L/R
 *   Capture:  1-2 Input 1 L/R, 3-4 Input 2 L/R, 5-6 Microphone
 *
 * The microphone is mono: the balanced input stage feeds the same analog
 * signal to both converters, so channels 5 and 6 carry identical content.
 */
static const struct snd_pcm_chmap_elem jockey3_playback_chmap[] = {
	{ .channels = 4,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR,
		   SNDRV_CHMAP_UNKNOWN, SNDRV_CHMAP_UNKNOWN } },
	{ }
};

static const struct snd_pcm_chmap_elem jockey3_capture_chmap[] = {
	{ .channels = 6,
	  .map = { SNDRV_CHMAP_UNKNOWN, SNDRV_CHMAP_UNKNOWN,
		   SNDRV_CHMAP_UNKNOWN, SNDRV_CHMAP_UNKNOWN,
		   SNDRV_CHMAP_UNKNOWN, SNDRV_CHMAP_UNKNOWN } },
	{ }
};

static int jockey3_init_pcm(struct jockey3_chip *chip)
{
	int ret = snd_pcm_new(chip->card, CARD_NAME " Audio", 0, 1, 1, &chip->pcm);

	if (ret < 0)
		return ret;

	strscpy(chip->pcm->name, CARD_NAME " Audio", sizeof(chip->pcm->name));
	chip->pcm->private_data = chip;
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK, &jockey3_pcm_ops);
	snd_pcm_set_ops(chip->pcm, SNDRV_PCM_STREAM_CAPTURE, &jockey3_pcm_ops);
	snd_pcm_set_managed_buffer_all(chip->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	ret = snd_pcm_add_chmap_ctls(chip->pcm, SNDRV_PCM_STREAM_PLAYBACK,
				     jockey3_playback_chmap, 4, 0, NULL);
	if (ret < 0)
		return ret;

	return snd_pcm_add_chmap_ctls(chip->pcm, SNDRV_PCM_STREAM_CAPTURE,
				      jockey3_capture_chmap, 6, 0, NULL);
}

static int jockey3_init_midi(struct jockey3_chip *chip)
{
	int ret = snd_rawmidi_new(chip->card, CARD_NAME " MIDI", 0, 1, 1, &chip->rmidi);

	if (ret < 0)
		return ret;

	chip->rmidi->private_data = chip;
	strscpy(chip->rmidi->name, CARD_NAME " MIDI", sizeof(chip->rmidi->name));
	snd_rawmidi_set_ops(chip->rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &jockey3_midi_in_ops);
	snd_rawmidi_set_ops(chip->rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &jockey3_midi_out_ops);
	chip->rmidi->info_flags = SNDRV_RAWMIDI_INFO_INPUT |
				  SNDRV_RAWMIDI_INFO_OUTPUT |
				  SNDRV_RAWMIDI_INFO_DUPLEX;
	return 0;
}

static void jockey3_setup_card_names(struct jockey3_chip *chip, int driver_info)
{
	/*
	 * card->driver is only char[16] and is what shows up as the card ID in
	 * /proc/asound, so it holds a short model identifier rather than the
	 * module name (which would be silently truncated).
	 */
	strscpy(chip->card->driver, "Jockey3", sizeof(chip->card->driver));
	strscpy(chip->card->shortname, CARD_NAME, sizeof(chip->card->shortname));
	strscpy(chip->card->mixername, CARD_NAME, sizeof(chip->card->mixername));

	snprintf(chip->card->longname, sizeof(chip->card->longname),
		 "%s %s at USB %s", CARD_NAME, jockey3_model_name(driver_info),
		 dev_name(&chip->dev->dev));
}

static int jockey3_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_interface *intf1;
	struct snd_card *card;
	struct jockey3_chip *chip;
	unsigned int dev_idx;
	int ret;

	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	intf1 = usb_ifnum_to_if(dev, 1);
	if (!intf1)
		return -ENODEV;

	ret = jockey3_validate_endpoints(intf, intf1);
	if (ret < 0)
		return ret;

	/* Claim the first enabled, unused card slot */
	scoped_guard(mutex, &jockey3_devices_mutex) {
		for (dev_idx = 0; dev_idx < SNDRV_CARDS; dev_idx++)
			if (enable[dev_idx] && !test_bit(dev_idx, jockey3_devices_used))
				break;

		if (dev_idx >= SNDRV_CARDS)
			return -ENODEV;

		__set_bit(dev_idx, jockey3_devices_used);
	}

	/*
	 * Deliberately not snd_devm_card_new(): a managed card is freed from the
	 * devres unwind inside usb_unbind_interface(), where snd_card_free()
	 * blocks until userspace closes every file descriptor on the card. On an
	 * unplug that unwind runs on the USB hub work queue, so one process
	 * sitting on a PCM fd would stall hotplug for the whole hub. The card is
	 * released from jockey3_disconnect() with snd_card_free_when_closed()
	 * instead, which is what every other USB sound driver does; the
	 * resources that outlive it go in card->private_free below.
	 */
	ret = snd_card_new(&intf->dev, index[dev_idx], id[dev_idx], THIS_MODULE,
			   sizeof(struct jockey3_chip), &card);
	if (ret < 0)
		goto err_free_idx;

	chip = card->private_data;
	chip->dev_idx = dev_idx;

	chip->card = card;
	/*
	 * The card outlives the unbind, so these pointers must too:
	 * snd_card_free_when_closed() defers the release until userspace closes
	 * the card, while usb_disconnect() device_unregister()s the interfaces
	 * as soon as the device leaves.
	 *
	 * This keeps the memory valid, not the device usable -- the interfaces
	 * are still unbound and every transfer through them fails.
	 * JOCKEY3_FLAG_DISCONNECTED remains the thing to test.
	 */
	chip->dev = usb_get_dev(dev);
	chip->intf0 = usb_get_intf(intf);
	chip->intf1 = usb_get_intf(intf1);
	chip->flags = 0;

	spin_lock_init(&chip->midi_lock);
	spin_lock_init(&chip->playback.lock);
	spin_lock_init(&chip->capture.lock);
	mutex_init(&chip->rate_mutex);
	init_completion(&chip->reset_done);

	init_usb_anchor(&chip->playback.anchor);
	init_usb_anchor(&chip->capture.anchor);
	init_waitqueue_head(&chip->playback.drain_wait);
	init_waitqueue_head(&chip->capture.drain_wait);

	/*
	 * Arm the release path only now that everything it touches exists --
	 * jockey3_free_resources() calls mutex_destroy() on rate_mutex, so it
	 * must not become reachable before mutex_init() above. From here on
	 * every failure goes to err_free_card, and the card slot is given back
	 * by jockey3_free_resources() rather than by err_free_idx. Keep this
	 * assignment below the initialization block: nothing fallible may be
	 * inserted between them.
	 */
	card->private_free = jockey3_card_free;
	/*
	 * card->private_data is zeroed by snd_card_new(), but be explicit:
	 * these two are load-bearing for the stop/stall bookkeeping.
	 */
	atomic_set(&chip->playback.urbs_in_flight, 0);
	atomic_set(&chip->capture.urbs_in_flight, 0);
	atomic64_set(&chip->playback.last_callback_time, 0);
	atomic64_set(&chip->capture.last_callback_time, 0);
	atomic64_set(&chip->playback.urbs_started_time, 0);
	atomic64_set(&chip->capture.urbs_started_time, 0);
	/*
	 * Default N until the first hw_params() picks a period-tuned value
	 * (jockey3_pcm_set_n()); matches the width the URB buffers below are
	 * actually allocated at, and jockey3_init_out_packet() primes.
	 */
	chip->playback.n_shift = ilog2(JOCKEY3_PLAYBACK_N);
	chip->playback.n_pkts = JOCKEY3_PLAYBACK_N;
	chip->capture.n_shift = ilog2(JOCKEY3_CAPTURE_N);
	chip->capture.n_pkts = JOCKEY3_CAPTURE_N;
	INIT_DELAYED_WORK(&chip->watchdog_work, jockey3_watchdog_work);

	chip->xfer_buf = kmalloc(USB_XFER_BUF_SIZE, GFP_KERNEL);
	if (!chip->xfer_buf) {
		ret = -ENOMEM;
		goto err_free_card;
	}

	ret = jockey3_init_midi_urb(chip);
	if (ret < 0)
		goto err_free_card;

	ret = jockey3_init_playback_urbs(chip);
	if (ret < 0)
		goto err_free_card;

	ret = jockey3_init_capture_urbs(chip);
	if (ret < 0)
		goto err_free_card;

	/* Interface 1 owns the capture endpoint (0x86) */
	ret = usb_driver_claim_interface(&jockey3_driver, intf1, chip);
	if (ret < 0)
		goto err_free_card;

	ret = jockey3_init_pcm(chip);
	if (ret < 0)
		goto err_free_card;

	ret = jockey3_init_midi(chip);
	if (ret < 0)
		goto err_free_card;

	jockey3_setup_card_names(chip, usb_id->driver_info);

	if (card->id[0] == '\0')
		snd_card_set_id(card, "RJ3");

	usb_set_intfdata(intf, chip);
	ret = jockey3_initialize(chip, usb_id->driver_info);
	if (ret < 0)
		goto err_free_card;

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_free_card;

	return 0;

err_free_card:
	/*
	 * Same order as jockey3_disconnect(): latch DISCONNECTED so a watchdog
	 * tick cannot re-arm itself, wait for any tick already running, kill the
	 * URBs, and only then let go of interface 1 and the card. All of it is
	 * safe on a half-built chip -- the locks, anchors and work item are
	 * initialized above with no failure point between, and everything
	 * jockey3_free_resources() touches is NULL until it is allocated.
	 *
	 * snd_card_free() rather than the disconnect path's
	 * snd_card_free_when_closed(): the card is not registered yet on any
	 * path that reaches here, so nothing can have it open and the
	 * synchronous free cannot block.
	 */
	usb_set_intfdata(intf, NULL);
	set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);
	cancel_delayed_work_sync(&chip->watchdog_work);
	jockey3_stop_urbs(chip);
	usb_driver_release_interface(&jockey3_driver, intf1);
	snd_card_free(card);
	return ret;

err_free_idx:
	/* Only reached before the card exists */
	scoped_guard(mutex, &jockey3_devices_mutex)
		__clear_bit(dev_idx, jockey3_devices_used);
	return ret;
}

static void jockey3_disconnect(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	/*
	 * Latch DISCONNECTED first, and for EITHER interface. Every ALSA entry
	 * point tests it on the way in, so setting it before anything else is
	 * torn down closes the window where e.g. jockey3_pcm_hw_params() had
	 * already passed its check and would go on to resubmit URBs we just
	 * killed.
	 *
	 * Doing it for interface 1 as well is what covers the unbind order. The
	 * driver is bound to both interfaces and the USB core takes them down
	 * one at a time, calling disconnect() and then usb_disable_interface()
	 * for each, so whichever goes first has its endpoints flushed while the
	 * other is still bound. Those URBs come back -ESHUTDOWN through the
	 * ordinary completion path, and without this they are indistinguishable
	 * from an endpoint torn down behind the driver's back. Losing either
	 * interface means the card is going away regardless -- interface 1 owns
	 * the capture endpoint -- so there is nothing to keep running for.
	 */
	if (chip)
		set_bit(JOCKEY3_FLAG_DISCONNECTED, &chip->flags);

	if (chip && intf == chip->intf0) {
		clear_bit(JOCKEY3_FLAG_RESETTING, &chip->flags);
		/*
		 * Release anyone blocked in jockey3_wait_for_reset_completion():
		 * a failed reset unbinds the interface instead of calling
		 * jockey3_post_reset(), so this is the only wakeup they get.
		 */
		complete_all(&chip->reset_done);

		/*
		 * Sync here, unlike in jockey3_stop_urbs(): no mutex is held on
		 * this path, and a tick that is mid-flight must be finished with
		 * the chip before the card is torn down. DISCONNECTED is already
		 * set above, so a tick that started just before this will not
		 * requeue itself.
		 */
		cancel_delayed_work_sync(&chip->watchdog_work);

		jockey3_stop_urbs(chip);

		/*
		 * Release interface 1 here, not from card->private_free: that
		 * runs when the last file descriptor is closed, by which time
		 * usb_disconnect() may already have freed the interface. This
		 * recurses into jockey3_disconnect() for interface 1, which only
		 * latches DISCONNECTED and clears its intfdata; if the core got
		 * there first, usb_driver_release_interface() sees a condition
		 * other than USB_INTERFACE_BOUND and returns.
		 */
		usb_driver_release_interface(&jockey3_driver, chip->intf1);

		/*
		 * snd_card_free_when_closed() disconnects the card now and frees
		 * it once userspace has closed it -- it never blocks, which is
		 * the whole point of not using a managed card (see the comment in
		 * jockey3_probe()). It runs snd_pcm_stop(DISCONNECTED) under the
		 * stream lock on the way, which drives our .trigger and clears
		 * 'running' with the proper locking, so there is nothing to clear
		 * here by hand. @chip lives in card->private_data and may be gone
		 * the moment this returns, so nothing below may touch it.
		 */
		snd_card_free_when_closed(chip->card);
	}
	usb_set_intfdata(intf, NULL);
}

/*
 * rate_mutex is taken and released within each of pre_reset()/post_reset()
 * rather than being held across the reset. The USB core does not guarantee
 * post_reset() runs at all: if the reset fails, the interface is marked for
 * rebinding and unbound instead, so a lock handed off from pre_reset() would
 * be leaked permanently and every later PCM ioctl would block on it forever.
 *
 * The window between the two callbacks is not left unguarded: the device is
 * physically in reset, so every EP0 transfer fails and is error-checked, and
 * JOCKEY3_FLAG_RESETTING gates the ALSA entry points.
 */
static int jockey3_pre_reset(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		/*
		 * A reset started outside this driver -- usbfs USBDEVFS_RESET,
		 * say -- does not pass through jockey3_queue_reset(), so
		 * chip->reset_done still carries the complete_all() from the
		 * previous reset and jockey3_wait_for_reset_completion() would
		 * return immediately, letting an ALSA ioctl talk to a device
		 * that is mid-reset. Re-arm it for that path. Testing the flag
		 * unlocked is safe because the USB core serializes resets per
		 * device, and re-arming before setting the flag leaves no
		 * window in which a waiter sees "resetting" but a stale
		 * completion.
		 */
		if (!jockey3_is_resetting(chip))
			reinit_completion(&chip->reset_done);
		set_bit(JOCKEY3_FLAG_RESETTING, &chip->flags);
		scoped_guard(mutex, &chip->rate_mutex)
			jockey3_stop_urbs(chip);
	}
	return 0;
}

static int jockey3_post_reset(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);
	u32 hw_rate = 0;

	if (chip && intf == chip->intf0) {
		scoped_guard(mutex, &chip->rate_mutex) {
			jockey3_initialize_ploytec(chip, NULL);

			/*
			 * Re-apply the rate unconditionally, as the vendor
			 * sequence does. The read below is a diagnostic only.
			 */
			if (ploytec_get_rate(chip->intf0, chip->xfer_buf,
					     PLOYTEC_RATE_IDX_DEVICE, &hw_rate) == 0 &&
			    hw_rate != chip->current_rate)
				dev_dbg(&chip->intf0->dev,
					"Rate after reset: HW %u, expected %u\n",
					hw_rate, chip->current_rate);

			jockey3_set_rate(chip, chip->current_rate, true);

			jockey3_start_urbs_failed(chip, jockey3_start_urbs(chip, false),
						  "a device reset");
		}

		clear_bit(JOCKEY3_FLAG_RESETTING, &chip->flags);
		complete_all(&chip->reset_done);
	}
	return 0;
}

static int jockey3_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		dev_dbg(&intf->dev, "USB suspend, stopping URBs\n");

		/*
		 * Before rate_mutex below: a tick blocked on that mutex resumes
		 * the moment this drops it, and the flag is what stops it
		 * restarting the ring. See jockey3_recover_urb_stream().
		 */
		set_bit(JOCKEY3_FLAG_SUSPENDED, &chip->flags);

		/* Notify ALSA core to transition state and unblock userspace */
		if (chip->pcm)
			snd_pcm_suspend_all(chip->pcm);

		/*
		 * Stop the physical URBs under rate_mutex, matching
		 * jockey3_restore_device() on the resume side -- otherwise a
		 * suspend could land in the middle of a rate change.
		 */
		scoped_guard(mutex, &chip->rate_mutex)
			jockey3_stop_urbs(chip);
	}
	return 0;
}

static int jockey3_restore_device(struct jockey3_chip *chip, bool reset)
{
	int ret;

	guard(mutex)(&chip->rate_mutex);

	/*
	 * Cleared under the mutex, before the restart below that
	 * jockey3_recover_urb_stream() defers to. A tick cannot slip in between:
	 * it needs this same mutex to restart anything.
	 */
	clear_bit(JOCKEY3_FLAG_SUSPENDED, &chip->flags);

	if (reset) {
		ret = jockey3_initialize_ploytec(chip, NULL);
		if (ret < 0)
			return ret;
	}

	ret = jockey3_set_rate(chip, chip->current_rate, true);
	if (ret < 0)
		return ret;

	/*
	 * Report the failure up: the PM core logs a failed resume, and unlike the
	 * reset path there is no queued recovery on the way that would pick this
	 * up on its own.
	 */
	ret = jockey3_start_urbs(chip, false);
	if (ret < 0) {
		dev_err(&chip->intf0->dev, "Failed to start URBs while restoring device: %d\n",
			ret);
		return ret;
	}
	return 0;
}

static int jockey3_resume(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		dev_dbg(&intf->dev, "USB resume, restoring device\n");
		return jockey3_restore_device(chip, false);
	}
	return 0;
}

static int jockey3_reset_resume(struct usb_interface *intf)
{
	struct jockey3_chip *chip = usb_get_intfdata(intf);

	if (chip && intf == chip->intf0) {
		dev_dbg(&intf->dev, "USB reset resume, restoring device\n");
		return jockey3_restore_device(chip, true);
	}
	return 0;
}

static const struct usb_device_id jockey3_ids[] = {
	{ USB_DEVICE(RELOOP_VENDOR_ID, RELOOP_JOCKEY3_ME_PID), .driver_info = JOCKEY3_ME },
	{ USB_DEVICE(RELOOP_VENDOR_ID, RELOOP_JOCKEY3_REMIX_PID), .driver_info = JOCKEY3_REMIX },
	{}
};
MODULE_DEVICE_TABLE(usb, jockey3_ids);

static struct usb_driver jockey3_driver = {
	.name = "snd-reloop-jockey3",
	/*
	 * Tear our own URBs down rather than having the core do it first.
	 *
	 * Without this, usb_unbind_interface() calls usb_disable_interface()
	 * before jockey3_disconnect(), so on an ordinary module unload the whole
	 * ring retires with -ESHUTDOWN while the driver still believes it is
	 * running -- indistinguishable, from inside the completion handler, from
	 * an endpoint torn down behind our back. jockey3_disconnect() already
	 * kills every URB, which is exactly the contract this flag asks for.
	 *
	 * It applies only while the device is still attached; on a physical
	 * unplug the core kills the URBs first regardless, which is why the
	 * completion path also tests USB_STATE_NOTATTACHED.
	 */
	.soft_unbind = 1,
	.probe = jockey3_probe,
	.disconnect = jockey3_disconnect,
	.pre_reset = jockey3_pre_reset,
	.post_reset = jockey3_post_reset,
	.suspend = jockey3_suspend,
	.resume = jockey3_resume,
	.reset_resume = jockey3_reset_resume,
	.id_table = jockey3_ids
};

/*
 * The codec's bit-spread lookup tables are module-global and unlocked, so they
 * are built once per module rather than per device.
 */
static int __init jockey3_module_init(void)
{
	enum ploytec_codec_variant codec_variant = ploytec_initialize_codec();

	switch (codec_variant) {
	case PLOYTEC_CODEC_PORTABLE:
		pr_debug("using portable codec\n");
		break;
	case PLOYTEC_CODEC_OPTIMIZED_64BIT:
		pr_debug("using 64-bit optimized codec\n");
		break;
	case PLOYTEC_CODEC_OPTIMIZED_32BIT:
		pr_debug("using 32-bit optimized codec\n");
		break;
	}

	return usb_register(&jockey3_driver);
}
module_init(jockey3_module_init);

static void __exit jockey3_module_exit(void)
{
	usb_deregister(&jockey3_driver);
}
module_exit(jockey3_module_exit);

MODULE_AUTHOR("Frank van de Pol <fvdpol@gmail.com>");
MODULE_DESCRIPTION(CARD_NAME " ALSA Driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: snd-pcm snd-rawmidi");
