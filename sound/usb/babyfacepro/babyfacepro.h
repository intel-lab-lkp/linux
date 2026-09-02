/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RME Babyface Pro FS - proprietary-mode USB audio driver
 *
 * The Babyface Pro FS presents two personalities on the USB bus: a
 * class-compliant one (handled by snd-usb-audio) and a proprietary one
 * (VID 0x2a39 / PID 0x3fc0) whose PCM stream runs on INTERRUPT
 * endpoints (interface 5, ep 0x01 OUT / 0x82 IN).  Isochronous
 * transfers are rejected there with EINVAL, and snd-usb-audio has no
 * interrupt-PCM path, so this driver is standalone (snd-usb-caiaq-style
 * interrupt streaming) instead of an snd-usb-audio quirk.
 *
 * The protocol (vendor requests + 14x32-bit frame layout) was
 * reverse-engineered from Windows captures and validated on hardware -
 * tools/usbdump/PROTOCOL.md is the authoritative reference.
 *
 * Stream notes (hardware-validated 2026-08):
 *   - frames_per_urb is tunable 8..1024 (multiple of 8) but must be at
 *     least one alt packet wide - the device delivers IN data in
 *     alt-sized packets (448/640/1024 B for alt 1/2/3), smaller URBs
 *     get -EOVERFLOW (babble).  So frames_per_urb >= 8/16/32 for
 *     alt 1/2/3; the driver rejects violating rates in hw_params.
 *   - Validated sweep 256->128->64->32->16 (<= 128 kHz): with nurbs=8 the
 *     period floor is 32 frames (0.67 ms @ 48 kHz) without glitches;
 *     nurbs=16 drops it to 16 frames (0.33 ms).  Soaks (5-15 min,
 *     2026-08-25) refine this: period 32 is the zero-glitch floor
 *     (0 xruns both directions); period 16 is rock-solid on playback
 *     but the capture side drops ~1 buffer per 7 s (0.67 ms each -
 *     any scheduler hiccup overruns a 0.33 ms ring) - fine for
 *     monitoring, not for clean recording.  Defaults (256x8) match
 *     the RME TotalMix 256-sample buffer; the low-latency profile is
 *     16x16.
 *   - The device only advances the stream while BOTH endpoints have a
 *     pending URB - IN and OUT are always submitted as a pair.
 *   - Sample rate = SET_INTERFACE(5, alt) only; the alt is a bandwidth
 *     class (alt 1 = 32/44.1/48/64/88.2 kHz, alt 2 = 96/128 kHz,
 *     alt 3 = 176.4/192 kHz), not a 1:1 rate code.
 */

#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define USB_VENDOR_RME			0x2a39
#define USB_PRODUCT_BABYFACE_PRO_FS	0x3fc0

/* The proprietary audio interface (interface 5, interrupt endpoints). */
#define BF_IFACE			5
#define BF_EP_OUT			0x01
#define BF_EP_IN			0x82

#define BF_ALT_1			1	/* 32/44.1/48/64/88.2 kHz, 448-B packets */
#define BF_ALT_2			2	/* 96/128 kHz, 640-B packets */
#define BF_ALT_3			3	/* 176.4/192 kHz, 1024-B packets */

/* Default stream geometry - conservative, matches the RME TotalMix
 * 256-sample buffer.  Both are tunable via module params; the
 * low-latency profile (validated) is frames_per_urb=16 nurbs=16.
 */
#define BF_FRAMES_PER_URB_DEFAULT	256
#define BF_NURBS_DEFAULT		8

/* Front-panel poll interval default - Windows polls the 5-register
 * status set at ~50 cycles/s (20 ms); match that.  Tunable via the
 * panel_poll_ms module param for reviewers/distros who want a slower
 * (or faster) rate than the Windows-matching default.
 */
#define BF_PANEL_POLL_MS_DEFAULT	20

#define BF_WORDS_PER_FRAME		14	/* 14 x 32-bit words per frame */

/* Consecutive URB errors (CRC/babble/protocol or a failed resubmit)
 * before the stream is stopped and the apps get a clean -EPIPE.
 */
#define BF_URB_ERR_STOP			3

/* Vendor requests (bmRequestType 0x40, value in wValue, no data phase). */
#define BF_REQ_KEEPALIVE		0x10	/* settings word / stream trigger */
#define BF_REQ_STATUS			0x11	/* read 4 B */
#define BF_REQ_CROSSPOINT		0x12	/* 16-bit crosspoint / master */
#define BF_REQ_SESSION_STOP		0x13	/* disarm - never sent mid-run */
#define BF_REQ_SESSION_ARM		0x14
#define BF_REQ_REG_CLEAR		0x16	/* cold-init register clear */
#define BF_REQ_PREAMP			0x17	/* 48V/PAD state + readback */
#define BF_REQ_GAIN			0x1a	/* 8-bit gain / master companion */
#define BF_REQ_DDS			0x1b	/* clock quads */
#define BF_REQ_STATUS_2			0x1c	/* read 4 B */
#define BF_REQ_SESSION_START		0x1d
#define BF_REQ_PREAMP_COMMIT		0x21	/* commit after 0x17 */
#define BF_REQ_LOOPBACK			0x15	/* per-output-channel flag */

/* Loopback map width (captured 2026-08-25, cap_loopback2.pcap):
 * TotalMix writes the FULL 30-channel 0x15 map on every toggle (ON =
 * the pair at 0x0001 + the other 28 at 0x0000; OFF = all 0x0000).
 * wIdx = 2xout_index: AN1/2 = 0/1, PH3/4 = 2/3, AS1/2 = 4/5, ...
 */
#define BF_LOOPBACK_CHANNELS		30

/* Register addresses. */
#define BF_REG_PREAMP			0x003f
#define BF_REG_MASTER_16		0x03e0	/* + 2*out (bReq 0x12) */
#define BF_REG_MASTER_8			0x0004	/* + 2*out (bReq 0x1a) */
#define BF_REG_GAIN			0x0000	/* + mic 0-3 (bReq 0x1a) */
#define BF_REG_CROSS_BASE_L		0x0034	/* + 0x34*out + src (bReq 0x12) */
#define BF_REG_CROSS_BASE_R		0x004e	/* + 0x34*out + src */
#define BF_REG_CROSS_STRIDE		0x0034
#define BF_REG_KEEPALIVE_SETTINGS	0x05cf
#define BF_REG_KEEPALIVE_INIT		0x05ff

/* The "cross" register block within each output: the L-registers sit at
 * odd offsets 5..23 and the R-registers at even offsets 4..22 (the stereo
 * source pairs that can be cross-linked).  bf_crosspoint_clear_cross()
 * zeroes them because the cold-init clear does not cover them.
 */
#define BF_CROSS_L_FIRST		5
#define BF_CROSS_L_LAST			23
#define BF_CROSS_R_FIRST		4
#define BF_CROSS_R_LAST			22

/* Front-panel readback (babyfacepro-ctl.c): 0x17 read at wIdx 0x0000 - the index
 * the Windows driver polls (cap_buttons2.pcap).  byte0 = preamp 48V/PAD,
 * byte1 = OUT sel + DIM/MIX bits, byte2 = IN sel + wheel counter,
 * byte3 = button flash (see babyfacepro-ctl.c for the full layout).
 */
#define BF_REG_PANEL_READ		0x0000
#define BF_PANEL_IN_SHIFT		4
#define BF_PANEL_IN_CH12		0x04
#define BF_PANEL_IN_CH34		0x05
#define BF_PANEL_IN_OPT			0x06
/* OUT selection - the gain-display-mode encoding (cap_dim.pcap);
 * babyfacepro-ctl.c also accepts the base-mode 0x01/0x02 (cap_buttons.pcap).
 */
#define BF_PANEL_OUT_CH12		0x04
#define BF_PANEL_OUT_PHONES		0x05
#define BF_PANEL_OUT_OPT		0x06
#define BF_PANEL_FLASH_IN		0x41
#define BF_PANEL_FLASH_SET		0x42
#define BF_PANEL_FLASH_MIX		0x44
#define BF_PANEL_FLASH_OUT		0x48
#define BF_PANEL_FLASH_SELECT		0x50
#define BF_PANEL_FLASH_DIM		0x60
#define BF_PANEL_BTN_NONE		0
#define BF_PANEL_BTN_IN			1
#define BF_PANEL_BTN_SET		2
#define BF_PANEL_BTN_MIX		3
#define BF_PANEL_BTN_OUT		4
#define BF_PANEL_BTN_SELECT		5
#define BF_PANEL_BTN_DIM		6

/* Preamp state byte (0x17, wIdx 0x003F - full state, verified).
 * NOTE 2026-08-26 (cap_reflevel3.pcap): the 0x0C "base" is NOT a
 * constant - it is the Instr 3/4 REF-LEVEL bits (bits 2-3, +4dBu =
 * 0x0C set; -10dBV/Boost = clear; Boost additionally commits 0x21
 * wVal 0x0003).  Keeping it always set = forcing the default +4dBu,
 * which is correct for the driver (no ref-level control).
 */
#define BF_PREAMP_REF_4DBU		0x000c
#define BF_PREAMP_BASE			BF_PREAMP_REF_4DBU
#define BF_PREAMP_48V_MIC1		0x0001
#define BF_PREAMP_48V_MIC2		0x0002
#define BF_PREAMP_PAD_MIC1		0x0010
#define BF_PREAMP_PAD_MIC2		0x0020

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

/* The front-panel gain/display family (0x1A, wIdx 0x000A + mic 0-3;
 * cap_panel/cap_mix.pcap): in gain mode the wheel writes the "ADC
 * gain" here (drives the same preamp as the GUI 0x0000+mic); in MIX
 * (fader) mode the same registers carry the VU DISPLAY shadow -
 * TotalMix writes the monitoring level display value (0..~31) and the
 * card lights the input VU segments accordingly (hardware-verified
 * 2026-08-26 live: sweeping 0x1A values moved the input VU).
 */
#define BF_REG_PANEL_GAIN		0x000a

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

/* Calibrated preamp gain: 65 dB over 20 raw steps (3.25 dB/step). */
#define BF_GAIN_MAX_DB			65

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
	int stream_users;		/* PCM substreams sharing the stream */
	bool streaming;			/* URBs actually in flight */
	bool shutdown;
	atomic_t urb_err;		/* consecutive bad URBs (stops the stream) */
	struct work_struct stream_work;

	struct snd_pcm_substream *subs[2];
	unsigned long hw_ptr[2];
	unsigned long prev_period[2];

	/* mixer state (no gain readback exists - host-side mirror) */
	u16 preamp;			/* 48V/PAD bits, base 0x0c */
	u8 gain[4];			/* preamp gain in dB 0-65/9 (raw derived
					 * at write: mic 3.25 dB/step, instr
					 * 0.5 dB/step)
					 */
	u8 gain_cycle;			/* 0x20/0x00/0x40 transaction counter */
	u8 flag_cnt;			/* 0xc000/0x4000/0x8000/0x0000 */
	u16 master[6][2];		/* cached 16-bit masters */
	bool muted[6];
	u16 dim_saved[2];		/* pre-DIM Phones master (out 1 L/R) */
	bool dim;			/* DIM engaged (fixed -20 dB on Phones) */
	u16 xpoint[6][14][2];		/* cached crosspoints (out, src, L/R) */
	int pitch;			/* varispeed in 0.1% (-500..+500) */
	bool loopback[6];
	bool an12;			/* AN 1>2 copy */
	bool linked;			/* AN1/2 input link */
	bool ms_proc;			/* MS processor engaged */
	int width;			/* width knob -100..+100 */
	u16 fx_send;			/* FX send level 0..0x1000 */

	/* DSP EQ (babyfacepro-ctl.c) - 4 analog-input strips, params kept in state */
	struct bf_eq_channel {
		bool on;		/* EQ engaged (else identity blocks) */
		s32 slope_db;		/* low-cut slope 6/12/18/24 (0 = off) */
		s32 lc_hz;		/* low-cut freq, 0 = off */
		u32 lc_raw;		/* cached 0x38 word */
		u8 slope;		/* cached slope byte (2^n - 1) */
		s32 band_type[3];	/* 0 off, 1 bell, 2 low shelf, 3 high shelf */
		s32 band_freq[3];	/* Hz */
		s32 band_q[3];		/* Q x 100 */
		s32 band_gain[3];	/* dB x 10 */
		s32 words[3][4];	/* cached c0..c3 */
		s32 shared;		/* cached c4 (shared by the slots) */
	} eq[4];

	/* front panel (babyfacepro-ctl.c) - 0x17 readback poll */
	struct delayed_work panel_work;
	unsigned int panel_poll_ms;	/* front-panel poll interval, module param */
	u8 panel_prev[4];		/* last 0x17 snapshot */
	bool panel_seen;		/* first snapshot taken */
	bool panel_select_armed;	/* device SELECT cycle armed (IN switch disarms) */
	unsigned long panel_start;	/* jiffies at panel_start (boot re-assert) */
	int panel_button;		/* latched button event (consumed on get) */
	int panel_wheel;		/* accumulated wheel delta (consumed on get) */
	int panel_in;			/* enum: 0 unknown, 1 Ch1/2, 2 Ch3/4, 3 Opt */
	int panel_out;			/* enum: 0 unknown, 1 Ch1/2, 2 Phones, 3 Opt */
	bool panel_mix;			/* MIX engaged - HOST-latched (like TotalMix):
					 * set by the 0x44 flash ack, NOT by the readback
					 * 0x80 bit (the raw press has none)
					 */
	bool panel_dim;			/* DIM sticky (byte1 bit 0x20) */
	bool panel_saw_fader;		/* device observed in fader mode (byte2 0x0x)
					 * - gates the device-driven MIX exit
					 */
	int panel_select;		/* SELECT state: 0 L, 1 R, 2 both, 3 none
					 * (host-tracked - not in the readback)
					 */
	int panel_sel_hold;		/* consecutive ticks with byte3 = 0x50
					 * (SELECT held > 200 ms = the OUT-balance
					 * gesture; a tap flashes only ~100-150 ms,
					 * selhold_probe2 - no engaged bit)
					 */
	u16 panel_mix_raw;		/* MIX-mode monitoring level (fader raw) */
	u8 panel_mix_disp[4];		/* MIX-mode VU display shadow per mic
					 * (0x1A 0x000A+mic - written on change
					 * so the input VU follows the wheel)
					 */
	struct snd_kcontrol *panel_kctl[7]; /* for snd_ctl_notify */
};

struct bf_saved {
	struct list_head list;
	char key[32];
	u16 preamp;
	u8 gain[4];
	u8 gain_cycle;
	u8 flag_cnt;
	u16 master[6][2];
	bool muted[6];
	u16 xpoint[6][14][2];
	int pitch;
	bool loopback[6];
	bool an12;
	bool linked;
	bool ms_proc;
	int width;
	u16 fx_send;
	bool dim;
};

struct bf_rate {
	unsigned int rate;
	unsigned int alt;
	unsigned int frame_bytes;
	unsigned int min_fpu;	/* frames/URB floor = one alt packet (448/640/1024 B) */
};

/* Sample-rate / alt classes (babyfacepro.c). */
const struct bf_rate *bf_rate_lookup(unsigned int rate);

/* -- shared driver state ---------------------- */
extern const u16 bf_flag_cycle[4];
extern const struct bf_source bf_sources[14];

/* babyfacepro-ctl.c - the DSP EQ (struct snd_usb_babyface is defined above). */
void bf_eq_band_words(s32 *w, int type, s32 freq_hz, s32 q100,
		      s32 gain_x10, s32 fs);
void bf_eq_reupload(struct snd_usb_babyface *chip);
int babyface_create_eq(struct snd_usb_babyface *chip);
extern const u8 bf_xpoint_block[6];
extern const struct snd_pcm_hw_constraint_list bf_rates_constraint;

/* -- babyfacepro.c ------------------------ */
int bf_vendor_write(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx);
int bf_vendor_write_cycle(struct snd_usb_babyface *chip, u8 req, u16 val, u16 idx);
int bf_vendor_read(struct snd_usb_babyface *chip, u8 req, u16 idx, u8 *buf);
int bf_cold_init(struct snd_usb_babyface *chip);
int bf_crosspoint_clear_cross(struct snd_usb_babyface *chip,
			      unsigned int blk);
const struct bf_rate *bf_rate_lookup(unsigned int rate);
void babyface_stream_kill(struct snd_usb_babyface *chip);
void babyface_pcm_stop_both(struct snd_usb_babyface *chip, snd_pcm_state_t state);
void babyface_stream_work(struct work_struct *work);

/* -- babyfacepro-ctl.c ----------------------- */
int babyface_write_default_mixer(struct snd_usb_babyface *chip);
int bf_apply_masters(struct snd_usb_babyface *chip);
int bf_loopback_write_map(struct snd_usb_babyface *chip, int out, bool on);
int bf_preamp_state_write(struct snd_usb_babyface *chip);
int babyface_create_controls(struct snd_usb_babyface *chip);
int babyface_create_xpoints(struct snd_usb_babyface *chip);
int babyface_create_flags(struct snd_usb_babyface *chip);

/* Master + gain law helpers - shared with the front-panel wheels. */
int bf_master_half_db(u16 vol16);	/* 16-bit master -> dBx2 */
int bf_master_16bit(int half_db);	/* dBx2 -> 16-bit master */
u8 bf_master_8bit(u16 vol16);		/* 16-bit master -> 8-bit companion */
int bf_gain_max_db(int mic);
int bf_gain_db(int mic, u8 raw);	u8 bf_gain_raw(int mic, int db);

/* -- babyfacepro-ctl.c ----------------------- */
int babyface_create_panel(struct snd_usb_babyface *chip);
void babyface_panel_start(struct snd_usb_babyface *chip);
void babyface_panel_stop(struct snd_usb_babyface *chip);
void babyface_panel_work(struct work_struct *work);

/* -- babyfacepro.c ------------------------ */
void bf_state_save(struct snd_usb_babyface *chip);
int bf_state_restore(struct snd_usb_babyface *chip);
void bf_state_purge(void);
int babyface_restore_state(struct snd_usb_babyface *chip);
int bf_state_apply_flags(struct snd_usb_babyface *chip);
