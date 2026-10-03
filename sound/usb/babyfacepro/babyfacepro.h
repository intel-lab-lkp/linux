/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RME Babyface Pro / Pro FS - proprietary-mode USB audio driver
 *
 * The Babyface Pro (and Pro FS) present two personalities on the USB
 * bus: a class-compliant one (handled by snd-usb-audio) and a
 * proprietary one (VID 0x2a39 / PID 0x3fc0) whose PCM stream runs on
 * INTERRUPT endpoints (interface 5, ep 0x01 OUT / 0x82 IN).
 * Isochronous transfers are rejected there with EINVAL, and
 * snd-usb-audio has no interrupt-PCM path, so this driver is
 * standalone (snd-usb-caiaq-style interrupt streaming) instead of an
 * snd-usb-audio quirk.
 *
 * babyfacepro.c holds the card lifecycle and the PCM stream.
 *
 * The protocol (vendor requests + 14x32-bit frame layout) was
 * reverse-engineered from Windows captures and validated on hardware -
 * tools/usbdump/PROTOCOL.md in the driver's development repository is
 * the authoritative reference.
 *
 * Stream notes (hardware-validated 2026-08):
 *   - The URB size follows the application's period, and enough URBs
 *     stay in flight to cover two periods (bf_urb_frames(), hw_params):
 *     between urb_frames_min and frames_per_urb, at most nurbs URBs.
 *     A URB must be a whole number of alt packets - the device delivers IN
 *     data in alt-sized packets (448/640/1024 B, 8/16/32 frames for
 *     alt 1/2/3), and smaller URBs get -EOVERFLOW (babble).
 *   - Measured with fixed URBs, sweep 256->128->64->32->16
 *     (<= 128 kHz): with nurbs=8 the period floor is 32 frames
 *     (0.67 ms @ 48 kHz) without glitches;
 *     nurbs=16 drops it to 16 frames (0.33 ms).  Soaks (5-15 min,
 *     2026-08-25) refine this: period 32 is the zero-glitch floor
 *     (0 xruns both directions); period 16 is rock-solid on playback
 *     but the capture side drops ~1 buffer per 7 s (0.67 ms each -
 *     any scheduler hiccup overruns a 0.33 ms ring) - fine for
 *     monitoring, not for clean recording.
 *   - The device only advances the stream while BOTH endpoints have a
 *     pending URB - IN and OUT are always submitted as a pair.
 *   - SET_INTERFACE(5, alt) selects single/double/quad speed and USB
 *     packet capacity; the BASE rate is the family register (request
 *     0x10, index 0x0030: 32k / 44.1k / 48k family), from which the
 *     firmware derives its own DDS word.  Three families times three
 *     speeds give all nine rates.  The 0x1B DDS quad is varispeed: a
 *     pitch RATIO applied on top of the family rate, sticky until
 *     rewritten, with the 48 kHz cold-plug quad meaning x1.0.  See
 *     bf_clock_write / bf_pitch_write.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define USB_VENDOR_RME			0x2a39
#define USB_PRODUCT_BABYFACE_PRO_FS	0x3fc0

/* The proprietary audio interface (interface 5, interrupt endpoints). */
#define BF_IFACE			5
#define BF_EP_OUT			0x01
#define BF_EP_IN			0x82

#define BF_ALT_1			1	/* x1: 32/44.1/48 kHz, 448-B packets */
#define BF_ALT_2			2	/* x2: 64/88.2/96 kHz, 640-B packets */
#define BF_ALT_3			3	/* x4: 128/176.4/192 kHz, 1024-B packets */

/* Upper limits of the stream geometry: the largest URB and the most
 * URBs in flight.  The application's period and buffer choose the
 * values actually used below these; both are module params.
 */
#define BF_FRAMES_PER_URB_DEFAULT	256
#define BF_NURBS_DEFAULT		8

#define BF_WORDS_PER_FRAME		14	/* 14 x 32-bit words per frame */

/* Consecutive URB errors (CRC/babble/protocol or a failed resubmit)
 * before the stream is stopped and the apps get a clean -EPIPE.
 */
#define BF_URB_ERR_STOP			3

/* Minimum time from the end of one stream session to the next session
 * trigger.  A session triggered within about 15 ms of the previous one's
 * URBs stopping comes up with the outputs silent (measured on the
 * hardware); this leaves a margin.
 */
#define BF_SESSION_GAP_MS		50

/* Vendor requests (bmRequestType 0x40, value in wValue, no data phase). */
#define BF_REQ_KEEPALIVE		0x10	/* settings word / stream trigger */
#define BF_REQ_REG_CLEAR		0x16	/* cold-init register clear */
#define BF_REQ_PREAMP			0x17	/* 48V/PAD state + readback */
#define BF_REQ_DDS			0x1b	/* clock quads */
#define BF_REQ_STATUS_2			0x1c	/* read 4 B */
#define BF_REQ_SESSION_START		0x1d
#define BF_REQ_SESSION_ARM		0x14
#define BF_REQ_PREAMP_COMMIT		0x21	/* commit after 0x17 */

#define BF_REG_RATE_FAMILY		0x0030	/* family << 4 (bReq 0x10) */
/* 0x05ff = 0x05cf | 0x0030: the settings word and the family register
 * written together (measured 2026-09-16: bits 4-5 of the value land in
 * the family register).  The cold-plug capture writes 0x0021 here, i.e.
 * family 48 kHz + clock internal - which is why replaying it after a
 * family write used to reset the rate to 48 kHz.
 */
#define BF_REG_KEEPALIVE_INIT		0x05ff
#define BF_REG_KEEPALIVE_SETTINGS	0x05cf

/* Host settings-state word carried by the BF_REG_KEEPALIVE_SETTINGS
 * keepalive (PROTOCOL.md "keepalive 0x10 0x05CF wVal = host settings-
 * state register", hardware-verified 2026-08-22/23).
 */
#define BF_SETTINGS_CLOCK_INTERNAL	0x0001

struct snd_usb_babyface {
	struct snd_card *card;
	struct usb_device *dev;
	struct usb_interface *iface;

	struct mutex mutex;		/* controls + stream geometry */
	spinlock_t lock;		/* hw_ptr / subs */

	/* stream */
	struct urb **urbs_in;
	struct urb **urbs_out;
	void **buf_in;
	void **buf_out;
	dma_addr_t *dma_in;
	dma_addr_t *dma_out;
	unsigned int nurbs;
	unsigned int frames_per_urb;	/* URB buffer size: the largest URB */
	unsigned int urb_frames_min;	/* smallest URB a period may choose */
	unsigned int urb_frames;	/* this session's URB size */
	unsigned int urbs_active;	/* this session's URBs in flight */
	int session_dir;		/* the direction that chose them */
	unsigned int frame_bytes;	/* 56/40/32 for alt 1/2/3 */
	unsigned int rate;
	unsigned int alt;
	int pitch;			/* varispeed in 0.1% (-500..+500) */
	int stream_users;		/* substreams set up (hw_params..hw_free) */
	bool stream_setup[2];		/* per direction, counted in stream_users */
	pid_t owner[2];			/* per direction: tgid that opened it */
	bool streaming;			/* URBs actually in flight */
	ktime_t stream_stopped;		/* when the last session's URBs stopped */
	bool shutdown;
	atomic_t urb_err;		/* consecutive bad URBs (stops the stream) */
	struct work_struct stream_work;

	/* Set in open() and cleared in close() under ->lock.  The URB
	 * handlers and babyface_pcm_stop_both() read it under
	 * rcu_read_lock(); close() waits them out with synchronize_rcu().
	 */
	struct snd_pcm_substream __rcu *subs[2];
	unsigned long hw_ptr[2];
	unsigned long prev_period[2];
	/* For runtime->delay: when the last IN URB completed, and the OUT
	 * URBs in flight.  Under ->lock.
	 */
	ktime_t in_done;
	unsigned int out_inflight;
};

struct bf_rate {
	unsigned int rate;
	unsigned int alt;
	unsigned int frame_bytes;
	unsigned int min_fpu;	/* frames/URB floor = one alt packet (448/640/1024 B) */
};

/* -- babyfacepro.c ------------------------ */
const struct bf_rate *bf_rate_lookup(unsigned int rate);
/* The family register value for a rate: 0 / 1 / 2 for the 32k / 44.1k /
 * 48k family (rate >> (alt - 1)).
 */
unsigned int bf_rate_family(const struct bf_rate *r);
int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx);
int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf);
int bf_settings_write(struct snd_usb_babyface *chip);
int bf_clock_write(struct snd_usb_babyface *chip);
int bf_pitch_write(struct snd_usb_babyface *chip, int pitch);
int bf_cold_init(struct snd_usb_babyface *chip);
void babyface_stream_kill(struct snd_usb_babyface *chip);
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, snd_pcm_state_t state);
void babyface_stream_work(struct work_struct *work);
extern const struct snd_pcm_hw_constraint_list bf_rates_constraint;
