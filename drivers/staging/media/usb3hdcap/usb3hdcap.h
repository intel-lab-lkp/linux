/* SPDX-License-Identifier: GPL-2.0 */
#ifndef USB3HDCAP_H
#define USB3HDCAP_H

#include <linux/usb.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-v4l2.h>

struct snd_card;
struct snd_pcm_substream;

#define USB3HDCAP_V4L2_STDS (V4L2_STD_NTSC | V4L2_STD_PAL)

/* NTSC SD frame format */
#define SD_WIDTH       720
#define NTSC_HEIGHT    480
/* YUYV = 4 bytes/2 pixels */
#define DEFAULT_BPL    (SD_WIDTH * 2)
/* PAL SD frame format */
#define PAL_HEIGHT    576

enum usb3hdcap_input {
	INPUT_COMPOSITE = 0,
	INPUT_SVIDEO    = 1,
	INPUT_COMPONENT = 2,
	INPUT_HDMI      = 3,
};

/* I2C 8-bit addresses */
#define ADDR_MST3367 0x9c
#define ADDR_TW9900  0x88
#define ADDR_CS53L21 0x94
#define ADDR_CPLD    0x98
#define ADDR_MCU     0x64 /* Nuvoton NUC100 on XCAPTURE-1 */

/* TW9900 registers */
#define TW9900_CSTATUS    0x01
#define TW9900_INFORM     0x02
#define TW9900_OPFORM     0x03
#define TW9900_OPCNTL_I   0x05
#define TW9900_ACNTL_I    0x06
#define TW9900_CROP_HI    0x07
#define TW9900_VDELAY_LO  0x08
#define TW9900_VACTIVE_LO 0x09
#define TW9900_HDELAY_LO  0x0a
#define TW9900_HACTIVE_LO 0x0b
#define TW9900_CNTRL2     0x0d
#define TW9900_SDT_NOISE  0x0e
#define TW9900_LUMA_CTL   0x0f
#define TW9900_BRIGHT     0x10
#define TW9900_CONTRAST   0x11
#define TW9900_SHARPNESS  0x12
#define TW9900_SAT_U      0x13
#define TW9900_SAT_V      0x14
#define TW9900_HUE        0x15
#define TW9900_VBICNTL    0x19
#define TW9900_ACNTL_II   0x1a
#define TW9900_OPCNTL_II  0x1b
#define TW9900_SDT        0x1c
#define TW9900_SDTR       0x1d
#define TW9900_VCNTL1     0x28
#define TW9900_VCNTL2     0x29
#define TW9900_MISC1      0x2d
#define TW9900_MISC2      0x2f
#define TW9900_CSTATUS_II 0x31
#define TW9900_ANAPLLCTL  0x4c
#define TW9900_VVBI       0x55
#define TW9900_HSBEGIN    0x6b
#define TW9900_HSEND      0x6c
#define TW9900_OVSDLY     0x6d
#define TW9900_OVSEND     0x6e

/* TW9900 bit definitions */
#define TW9900_NINTL         0x08
#define TW9900_STDNOW_SHIFT  4
#define TW9900_STDNOW_MASK   0x70
#define TW9900_STD_NTSC_M    0
#define TW9900_STD_PAL_BDGHI 1
#define TW9900_STD_SECAM     2
#define TW9900_STD_NTSC_4_43 3
#define TW9900_STD_PAL_M     4
#define TW9900_STD_PAL_CN    5
#define TW9900_STD_PAL_60    6

/* CS53L21 registers */
#define CS53L21_MIC_PWR_CTL  0x03
#define CS53L21_IFACE_CTL    0x04
#define CS53L21_ADC_IN_SEL   0x07
#define CS53L21_SPE_CTL      0x09
#define CS53L21_ALC_PGAA     0x0a
#define CS53L21_ALC_PGAB     0x0b
#define CS53L21_ADCA_ATT     0x0c
#define CS53L21_ADCB_ATT     0x0d
#define CS53L21_ADCA_MIX_VOL 0x0e
#define CS53L21_ADCB_MIX_VOL 0x0f
#define CS53L21_CH_MIXER     0x18
#define CS53L21_ALC_EN_ATK   0x1c

/* Vendor request codes */
#define REQ_I2C    0xc0
#define REQ_GPIO   0xc1
#define REQ_INIT   0xc2
#define REQ_STREAM 0xc6
#define REQ_UNK_C7 0xc7

/* Isochronous transfer parameters */
#define EP_VIDEO     0x83
#define NUM_XFERS    8
#define NUM_ISO_PKTS 64

/*
 * Stream framing: two element types in the byte stream, both
 * starting with ff 00:
 *   SAV (ff 00 00 XY): bpl-1 pixel bytes, -1 because the XY is actually
 *   in the first pixel's V position... weird but it's correct
 *   Marker (ff 00 ff NN):  audio - NN = number of stereo s16le
 *                          samples, audio data = NN * 4 bytes
 */
#define SAV_LEN        4
#define MARKER_LEN     4
#define PARSE_BUF_SIZE (2 * 1024 * 1024)

/* BT.656 SAV XY bits */
#define BT656_F_BIT 0x40
#define BT656_V_BIT 0x20

struct hdcap_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct usb3hdcap {
	struct device *dev;
	struct usb_device *usb_dev;
	struct v4l2_device v4l2_dev;
	struct video_device video_dev;
	struct vb2_queue vb2q;

	struct v4l2_ctrl_handler ctrl;
	struct v4l2_ctrl *ctrl_brightness;
	struct v4l2_ctrl *ctrl_contrast;
	struct v4l2_ctrl *ctrl_saturation;
	struct v4l2_ctrl *ctrl_hue;
	struct v4l2_ctrl *ctrl_sharpness;
	struct v4l2_ctrl *ctrl_rx_power;

	/* protects V4L2 ioctls and device state */
	struct mutex v4l2_lock;
	/* protects videobuf2 queue operations */
	struct mutex vb2q_lock;

	/* buffer list */
	spinlock_t buflock;
	struct list_head bufs;

	/* ISO URBs */
	int iso_size;
	struct urb *isoc_urbs[NUM_XFERS];

	/* frame tracking */
	unsigned int sequence;

	/* input selection + format */
	enum usb3hdcap_input input;
	v4l2_std_id std;
	v4l2_std_id requested_std;
	int width;
	int height;
	int interlaced;
	int bpl; /* width * 2 */

	/* stream parser state */
	u8 *parse_buf;
	int parse_len;
	struct hdcap_buf *cur_buf;
	int frame_line;
	int synced;
	int was_blanking;
	int is_second_field;

	/* hardware init state */
	int hw_inited;
	int has_mcu; /* Nuvoton MCU present (XCAPTURE-1) */

	/* MST3367 state */
	int mst_current_bank;
	struct v4l2_dv_timings detected_timings;
	int detected_timings_present;
	struct v4l2_dv_timings requested_timings;
	int requested_timings_present;

	/* ALSA audio */
	struct snd_card *snd;
	struct snd_pcm_substream *snd_substream;
	atomic_t snd_stream;
	size_t snd_buffer_pos;
	size_t snd_period_pos;

	/* diagnostics */
	unsigned int iso_cb_count;
	unsigned long iso_bytes;
	unsigned int markers_found;
	unsigned int frames_delivered;
};

/* USB control helpers (usb3hdcap-core.c) */
int vendor_out(struct usb3hdcap *hdcap, u8 request, u16 value, u16 index,
	       u8 *data, u16 len);
int u3hc_i2c_write(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 val);
int u3hc_i2c_read(struct usb3hdcap *hdcap, u8 addr, u8 reg);
int u3hc_i2c_rmw(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 mask, u8 bits);
int u3hc_i2c_rmw_get_old(struct usb3hdcap *hdcap, u8 addr, u8 reg, u8 mask, u8 bits, u8 *old);
int usb3hdcap_hw_init(struct usb3hdcap *hdcap);

/* Composite (usb3hdcap-composite.c) */
int usb3hdcap_composite_init(struct usb3hdcap *hdcap);

/* HDMI + Component (usb3hdcap-hdmi.c) */
int usb3hdcap_hdmi_init(struct usb3hdcap *hdcap);
int usb3hdcap_component_init(struct usb3hdcap *hdcap);
void mst3367_write_csc(struct usb3hdcap *hdcap);

/* Video/streaming (usb3hdcap-video.c) */
extern const struct vb2_ops usb3hdcap_vb2_ops;

/* Audio (usb3hdcap-audio.c) */
int usb3hdcap_cs53l21_init(struct usb3hdcap *hdcap);
int usb3hdcap_audio_init(struct usb3hdcap *hdcap);
void usb3hdcap_audio_free(struct usb3hdcap *hdcap);
void usb3hdcap_audio_data(struct usb3hdcap *hdcap, const u8 *data, int len);

#endif
