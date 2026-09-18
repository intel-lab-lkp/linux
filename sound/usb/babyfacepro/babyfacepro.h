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
 * This file and babyfacepro.c cover the card lifecycle (probe,
 * disconnect), the PCM stream, and mixer-state persistence across
 * re-probes/resume.  babyfacepro-ctl.c adds the ALSA control surface,
 * starting with the output masters (volume + mute) and the crosspoint
 * routing matrix - the minimum needed for the card to produce
 * controllable, routed audio.  Preamp/gain, the routing-flag switches,
 * varispeed pitch, front-panel support and suspend/resume are added by
 * later patches in this series, each with its own register map and
 * device-state additions; see the cover letter for the full series
 * plan.
 *
 * The protocol (vendor requests + 14x32-bit frame layout) was
 * reverse-engineered from Windows captures and validated on hardware -
 * tools/usbdump/PROTOCOL.md in the driver's development repository is
 * the authoritative reference.
 *
 * Stream notes (hardware-validated 2026-08):
 *   - frames_per_urb is tunable 8..1024 (multiple of 8) but must be at
 *     least one alt packet wide - the device delivers IN data in
 *     alt-sized packets (448/640/1024 B for alt 1/2/3), smaller URBs
 *     get -EOVERFLOW (babble).  So frames_per_urb >= 8/16/32 for
 *     alt 1/2/3; the driver rejects violating rates in hw_params.
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

/* Default stream geometry - conservative, matches the RME TotalMix
 * 256-sample buffer.  Both are tunable via module params; the
 * low-latency profile (validated) is frames_per_urb=16 nurbs=16.
 */
#define BF_FRAMES_PER_URB_DEFAULT	256
#define BF_NURBS_DEFAULT		8

#define BF_WORDS_PER_FRAME		14	/* 14 x 32-bit words per frame */

/* Consecutive URB errors (CRC/babble/protocol or a failed resubmit)
 * before the stream is stopped and the apps get a clean -EPIPE.
 */
#define BF_URB_ERR_STOP			3

/* Vendor requests (bmRequestType 0x40, value in wValue, no data phase).
 * Only the subset the core probe/PCM path needs; the mixer, front
 * panel and DSP EQ patches each add their own as those features land.
 */
#define BF_REQ_KEEPALIVE		0x10	/* settings word / stream trigger */
#define BF_REQ_CROSSPOINT		0x12	/* 16-bit crosspoint / master */
#define BF_REQ_REG_CLEAR		0x16	/* cold-init register clear */
#define BF_REQ_PREAMP			0x17	/* 48V/PAD state + readback */
#define BF_REQ_GAIN			0x1a	/* 8-bit gain / master companion */
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
 * state register", hardware-verified 2026-08-22/23).  Only the
 * Internal value is used until the clock-source control lands with a
 * later patch in this series.
 */
#define BF_SETTINGS_CLOCK_INTERNAL	0x0001
#define BF_SETTINGS_CLOCK_OPTICAL	0x0004

/* Register addresses (masters + crosspoint matrix). */
#define BF_REG_MASTER_16		0x03e0	/* + 2*out (bReq 0x12) */
#define BF_REG_MASTER_8			0x0004	/* + 2*out (bReq 0x1a) */
#define BF_REG_CROSS_BASE_L		0x0034	/* + 0x34*out + src (bReq 0x12) */
#define BF_REG_CROSS_BASE_R		0x004e	/* + 0x34*out + src */
#define BF_REG_CROSS_STRIDE		0x0034
/* Low map (the AN1/2 monitor bus's own per-source registers, one set
 * shared across every output - not one per output block like the
 * standard crosspoint map above). NOT a shadow/mirror of the standard
 * map for AN1/2: it is what that output actually sums from, and the
 * standard map alone has no audible effect on it (hardware-verified
 * 2026-09-14, see bf_xpoint_write's own comment and KERNEL-DRIVER.md).
 */
#define BF_REG_LOWMAP_BASE_L		0x0000	/* + idx_l */
#define BF_REG_LOWMAP_BASE_R		0x001a	/* + idx_r */

/* The "cross" register block within each output: the L-registers sit at
 * odd offsets 5..23 and the R-registers at even offsets 4..22 (the stereo
 * source pairs that can be cross-linked).  bf_crosspoint_clear_cross()
 * zeroes them because the cold-init clear does not cover them.
 */
#define BF_CROSS_L_FIRST		5
#define BF_CROSS_L_LAST			23
#define BF_CROSS_R_FIRST		4
#define BF_CROSS_R_LAST			22

/* Calibrated master value: 0 dB = 0x2000 (+6 dB = 0x4000).  See
 * CALIBRATION.md.  The crosspoint fader curve is DIFFERENT (0 dB =
 * 0x16a0, top 0x2d41 - see below).
 */
#define BF_MASTER_0DB			0x2000

/* The 8-bit master is the REAL output volume (hardware-verified
 * 2026-08-24: writing it changes the level, the 16-bit does not).
 * Scale: 0.5 dB per step, 0xf3 = 0 dB (the scene-load default),
 * bottom 0x73 = -64 dB (silence), top 0xff = +6 dB.  The 16-bit
 * register is a companion kept in sync (TotalMix writes both).
 * The mute value is 0x3B.
 */
#define BF_MASTER_8_0DB			0xf3
#define BF_MASTER_8_MIN			0x73
#define BF_MASTER_MUTE			0x3b
#define BF_MASTER_UNMUTE		0xf3
/* -20 dB master, 8-bit and 16-bit: the exact pair the hardware DIM
 * button writes (cap_dim2.pcap), reused as the power-on default.
 */
#define BF_MASTER_MINUS20_8		0xcb
#define BF_MASTER_MINUS20_16		0x0333

/* Crosspoint fader curve: 0 dB = 0x16a0, +6 dB = 0x2d41 (fader curve,
 * DIFFERENT from the master 0x4000 top - see CALIBRATION.md).
 */
#define BF_FADER_0DB			0x16a0
#define BF_FADER_TOP			0x2d41

/* The crosspoint matrix sources (14 controls per output). */
struct bf_source {
	const char *name;
	u8 idx_l;
	u8 idx_r;
};

/* Crosspoint-source order + register block maps (babyfacepro-ctl.c). */
extern const struct bf_source bf_sources[14];
extern const u8 bf_xpoint_block[6];

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
	unsigned int frames_per_urb;
	unsigned int frame_bytes;	/* 56/40/32 for alt 1/2/3 */
	unsigned int rate;
	unsigned int alt;
	int pitch;			/* varispeed in 0.1% (-500..+500) */
	int stream_users;		/* PCM substreams sharing the stream */
	bool streaming;			/* URBs actually in flight */
	bool shutdown;
	atomic_t urb_err;		/* consecutive bad URBs (stops the stream) */
	struct work_struct stream_work;

	struct snd_pcm_substream *subs[2];
	unsigned long hw_ptr[2];
	unsigned long prev_period[2];

	/* mixer state (no gain readback exists - host-side mirror) */
	u8 flag_cnt;			/* 0xc000/0x4000/0x8000/0x0000 */
	u16 master[6][2];		/* cached 16-bit masters */
	bool muted[6];
	u16 xpoint[6][14][2];		/* cached crosspoints (out, src, L/R) */
};

/* The mixer state cached across interface re-probes/resume (see
 * babyfacepro.c's own comment on bf_state_save/bf_state_restore).
 * Grows alongside struct snd_usb_babyface as later patches in this
 * series add more mixer state to persist.
 */
struct bf_saved {
	struct list_head list;
	char key[32];
	u8 flag_cnt;
	u16 master[6][2];
	bool muted[6];
	u16 xpoint[6][14][2];
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

/* Mixer-state persistence across interface re-probes/resume. */
void bf_state_save(struct snd_usb_babyface *chip);
int bf_state_restore(struct snd_usb_babyface *chip);
void bf_state_purge(void);
int babyface_restore_state(struct snd_usb_babyface *chip);

/* -- babyfacepro-ctl.c ----------------------- */
extern const u16 bf_flag_cycle[4];
int bf_vendor_write_cycle(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx);
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
			      unsigned int blk);
int babyface_write_default_mixer(struct snd_usb_babyface *chip);
int bf_apply_masters(struct snd_usb_babyface *chip);
int bf_xpoint_write(struct snd_usb_babyface *chip, int out, int src,
		    u16 l, u16 r);
int babyface_create_masters(struct snd_usb_babyface *chip);
int babyface_create_xpoints(struct snd_usb_babyface *chip);

/* Master gain-law helpers - shared with the front-panel wheels once
 * the front panel lands.
 */
int bf_master_half_db(u16 vol16);	/* 16-bit master -> dBx2 */
int bf_master_16bit(int half_db);	/* dBx2 -> 16-bit master */
u8 bf_master_8bit(u16 vol16);		/* 16-bit master -> 8-bit companion */
