// SPDX-License-Identifier: GPL-2.0
/*
 * OnePlus / OPPO Pad pogo-pin keyboard driver (Tinno keyboard MCU)
 *
 * The keyboard hangs off a half-duplex UART (921600 8N1) carried over the
 * pogo pins: a single data wire is shared by the host TX and the keyboard
 * TX.  The host TX is gated onto the wire by a PMIC GPIO which is only
 * asserted while a frame is being sent; every host frame is therefore
 * echoed back on RX.  The same wire is also routed to a plain GPIO that is
 * used for attach detection while the keyboard is unpowered.
 *
 * Frame format ("one wire bus"):
 *
 *   55 x8 | F1 | src | dst | cmd | len | payload[len] | crc_hi crc_lo | FE | AA x4
 *
 * src/dst are 0xA1 (keyboard) and 0xA2 (pad).  CRC16 uses the IBM polynomial
 * 0x8005 (MSB first, init 0xC596) over F1 .. payload.  Replies carry cmd + 1.
 *
 * Unsolicited keyboard frames: 0x01 keys (HID boot report), 0x02 consumer
 * keys (two LE16 HID usages), 0x03 touchpad, 0x2F sync/heartbeat (~100 ms).
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define POGO_BAUD		921600

#define OWB_HEAD_SYNC		0x55
#define OWB_TAIL_SYNC		0xAA
#define OWB_START		0xF1
#define OWB_START_REPEAT	0xF2
#define OWB_END			0xFE
#define OWB_ADDR_KBD		0xA1
#define OWB_ADDR_PAD		0xA2

#define OWB_HEAD_SYNC_LEN	8
#define OWB_TAIL_SYNC_LEN	4
#define OWB_HDR_LEN		5	/* start, src, dst, cmd, len */
#define OWB_TRAILER_LEN		5	/* crc(2), end, tail sync(2) seen on rx */
#define OWB_MAX_PAYLOAD		128
#define OWB_RX_BUF_LEN		(OWB_HDR_LEN + OWB_MAX_PAYLOAD + OWB_TRAILER_LEN)
#define OWB_TX_BUF_LEN		(OWB_HEAD_SYNC_LEN + OWB_HDR_LEN + OWB_MAX_PAYLOAD + \
				 2 + 1 + OWB_TAIL_SYNC_LEN)

#define OWB_CRC_POLY		0x8005
#define OWB_CRC_INIT		0xC596

/* main commands */
#define OWB_CMD_KEYS		0x01
#define OWB_CMD_MEDIA_KEYS	0x02
#define OWB_CMD_TOUCHPAD	0x03
#define OWB_CMD_PARAM_SET	0x20
#define OWB_CMD_SYNC_UPLOAD	0x2F
#define OWB_CMD_GENERAL		0x3A
#define OWB_CMD_GENERAL_ACK	0x3B

/* 0x3A sub commands */
#define OWB_GEN_SLEEP		0x02
#define OWB_GEN_BATTERY_STATUS	0x0E
#define OWB_GEN_BRIGHTNESS	0x1C

/* sync upload sub commands */
#define OWB_SYNC_PLUG_IN	0x01
#define OWB_SYNC_HEARTBEAT	0x05

/* timing, all derived from the vendor driver */
#define POGO_TX_GATE_ON_US	450
#define POGO_TX_GATE_OFF_US	300
#define POGO_ECHO_TIMEOUT_MS	50
#define POGO_RESP_TIMEOUT_MS	100
#define POGO_XFER_RETRIES	3
#define POGO_DETECT_DEBOUNCE_MS	120
#define POGO_DETECT_SAMPLES	6	/* 6 x 20 ms, as the vendor driver */
#define POGO_DETECT_SAMPLE_MS	20
#define POGO_HB_POLL_MS		250
#define POGO_HB_TIMEOUT_MS	500	/* after the first sync frame */
#define POGO_BOOT_TIMEOUT_MS	2000	/* power on -> first sync frame */
#define POGO_RETRY_MIN_MS	1000
#define POGO_RETRY_MAX_MS	30000

#define POGO_TOUCH_FINGERS	5
#define POGO_TOUCH_REC_LEN	5
#define POGO_KEY_REPORT_LEN	8
#define POGO_MEDIA_REPORT_LEN	4
#define POGO_MAC_LEN		6
#define POGO_BACKLIGHT_MAX	100	/* MCU accepts 0..100 */

struct pogo_kbd {
	struct serdev_device *serdev;
	struct device *dev;

	struct gpio_desc *power_gpio;
	struct gpio_desc *tx_en_gpio;
	struct gpio_desc *wake_gpio;
	int wake_irq;
	bool wake_irq_enabled;

	/* rx frame assembly, serdev receive context only */
	u8 rx_buf[OWB_RX_BUF_LEN];
	unsigned int rx_len;
	unsigned int rx_expected;
	unsigned int sync_cnt;
	bool in_frame;

	/* host -> keyboard transfers */
	struct mutex io_lock;
	struct completion echo_done;
	struct completion resp_done;
	u8 tx_cmd;
	bool tx_pending;
	u8 resp[OWB_MAX_PAYLOAD];
	unsigned int resp_len;

	/* link state */
	bool powered;
	bool connected;
	bool hb_seen;		/* MCU left its plug-in phase */
	unsigned long last_rx;
	unsigned int retry_ms;
	u8 brand;
	u8 mac[POGO_MAC_LEN];
	int battery_level;

	struct delayed_work detect_work;
	struct delayed_work hb_work;
	struct work_struct plug_work;
	struct work_struct led_work;
	bool caps_led;
	struct led_classdev backlight;
	u8 brightness;

	/* input */
	struct mutex input_lock;
	struct input_dev *kbd;
	struct input_dev *tp;
	u8 old_keys[POGO_KEY_REPORT_LEN];
	u8 old_media[POGO_MEDIA_REPORT_LEN];
	u32 tp_max_x, tp_max_y, tp_res_x, tp_res_y;
	char kbd_name[64];
	char tp_name[64];
};

/* HID boot keyboard usage -> linux keycode, identical to usbkbd + vendor extras */
static const unsigned char pogo_keycode[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
	105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
	191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
	115, 114,  0,  0,  0, 121,  0, 89, 93, 124, 92, 94, 95,  0,  0,  0,
	122, 123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0, 111,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0, 179, 180,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0, 111,  0,  0,  0,  0,  0,  0,  0,
	 29, 42, 56, 125, 97, 54, 100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
	150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140,  0,  0,  0,  0
};

/* HID consumer usage -> linux keycode for the function row */
static const struct {
	u16 usage;
	u16 keycode;
} pogo_media_keys[] = {
	{ 0x070, KEY_BRIGHTNESSDOWN },
	{ 0x06f, KEY_BRIGHTNESSUP },
	{ 0x0e2, KEY_MUTE },
	{ 0x0ea, KEY_VOLUMEDOWN },
	{ 0x0e9, KEY_VOLUMEUP },
	{ 0x224, KEY_BACK },
	{ 0x38e, KEY_SCREENLOCK },
	{ 0x390, KEY_KBD_LAYOUT_NEXT },
	{ 0x391, KEY_MICMUTE },
	{ 0x392, KEY_TOUCHPAD_TOGGLE },
	{ 0x393, KEY_SEARCH },
	{ 0x394, KEY_FULL_SCREEN },
	{ 0x395, KEY_SELECTIVE_SCREENSHOT },
	{ 0x397, KEY_PROG1 },
	{ 0x398, KEY_PROG2 },
	{ 0x399, KEY_PROG3 },
	{ 0x0cd, KEY_PLAYPAUSE },
	{ 0x0b5, KEY_NEXTSONG },
	{ 0x0b6, KEY_PREVIOUSSONG },
	{ 0x244, KEY_APPSELECT },
	{ 0x2fa, KEY_ASSISTANT },
	{ 0x1d0, KEY_FN },
};

static const char * const pogo_brand_names[] = {
	"OPPO Pad 5 Pro Smart Keyboard",
	"OnePlus Pad 3 Pro Smart Keyboard",
	"OnePlus Pad 4 Smart Keyboard",
};

/* ------------------------------------------------------------------------ */
/* framing                                                                  */

static u16 owb_crc16(const u8 *buf, unsigned int len)
{
	u16 crc = OWB_CRC_INIT;
	unsigned int i, bit;

	for (i = 0; i < len; i++) {
		u8 data = buf[i];

		for (bit = 0; bit < 8; bit++) {
			bool feedback = (crc ^ (data << 8)) & 0x8000;

			crc <<= 1;
			if (feedback)
				crc ^= OWB_CRC_POLY;
			data <<= 1;
		}
	}
	return crc;
}

static unsigned int owb_build_frame(u8 *out, u8 cmd, const u8 *payload, u8 len)
{
	unsigned int n = 0;
	u16 crc;

	memset(out, OWB_HEAD_SYNC, OWB_HEAD_SYNC_LEN);
	n += OWB_HEAD_SYNC_LEN;
	out[n++] = OWB_START;
	out[n++] = OWB_ADDR_PAD;
	out[n++] = OWB_ADDR_KBD;
	out[n++] = cmd;
	out[n++] = len;
	memcpy(&out[n], payload, len);
	n += len;
	crc = owb_crc16(&out[OWB_HEAD_SYNC_LEN], OWB_HDR_LEN + len);
	out[n++] = crc >> 8;
	out[n++] = crc & 0xff;
	out[n++] = OWB_END;
	memset(&out[n], OWB_TAIL_SYNC, OWB_TAIL_SYNC_LEN);
	n += OWB_TAIL_SYNC_LEN;

	return n;
}

/* ------------------------------------------------------------------------ */
/* host -> keyboard                                                         */

/*
 * Send cmd/payload and optionally wait for the cmd + 1 reply.  The reply
 * (cmd, len, data...) is copied to @resp when non-NULL and its length is
 * returned.  Process context only; serialised by io_lock.
 */
static int pogo_xfer(struct pogo_kbd *kb, u8 cmd, const u8 *payload, u8 len,
		     u8 *resp, unsigned int resp_size)
{
	u8 frame[OWB_TX_BUF_LEN];
	unsigned int frame_len;
	int attempt, ret = -ETIMEDOUT;

	if (len > OWB_MAX_PAYLOAD)
		return -EINVAL;

	frame_len = owb_build_frame(frame, cmd, payload, len);
	print_hex_dump_debug("pogo tx: ", DUMP_PREFIX_NONE, 32, 1, frame, frame_len, false);

	mutex_lock(&kb->io_lock);
	for (attempt = 0; attempt < POGO_XFER_RETRIES; attempt++) {
		bool echoed;

		reinit_completion(&kb->echo_done);
		reinit_completion(&kb->resp_done);
		kb->tx_cmd = cmd;
		WRITE_ONCE(kb->tx_pending, true);

		gpiod_set_value_cansleep(kb->tx_en_gpio, 1);
		usleep_range(POGO_TX_GATE_ON_US, POGO_TX_GATE_ON_US + 50);

		ret = serdev_device_write(kb->serdev, frame, frame_len,
					  msecs_to_jiffies(POGO_RESP_TIMEOUT_MS));
		if (ret >= 0)
			serdev_device_wait_until_sent(kb->serdev,
						      msecs_to_jiffies(20));

		/*
		 * Release the wire as soon as the last bit is out: the MCU
		 * answers within a few hundred microseconds and would collide
		 * with our idle-high TX driver otherwise.  The echo is only
		 * checked afterwards, it is already queued in the tty buffer.
		 */
		usleep_range(POGO_TX_GATE_OFF_US, POGO_TX_GATE_OFF_US + 50);
		gpiod_set_value_cansleep(kb->tx_en_gpio, 0);

		echoed = wait_for_completion_timeout(&kb->echo_done,
				msecs_to_jiffies(POGO_ECHO_TIMEOUT_MS)) != 0;

		if (ret < 0) {
			dev_warn(kb->dev, "cmd 0x%02x write failed: %d\n", cmd, ret);
			continue;
		}
		if (!echoed) {
			dev_dbg(kb->dev, "cmd 0x%02x: no echo\n", cmd);
			ret = -EIO;
			continue;
		}
		if (!resp) {
			ret = 0;
			break;
		}
		if (wait_for_completion_timeout(&kb->resp_done,
				msecs_to_jiffies(POGO_RESP_TIMEOUT_MS))) {
			unsigned int n = min(kb->resp_len, resp_size);

			memcpy(resp, kb->resp, n);
			ret = n;
			break;
		}
		dev_dbg(kb->dev, "cmd 0x%02x: no reply\n", cmd);
		ret = -ETIMEDOUT;
	}
	WRITE_ONCE(kb->tx_pending, false);
	mutex_unlock(&kb->io_lock);

	return ret;
}

static int pogo_set_general(struct pogo_kbd *kb, u8 sub, u8 value)
{
	const u8 payload[] = { sub, 0x01, value };
	u8 resp[8];
	int ret;

	ret = pogo_xfer(kb, OWB_CMD_GENERAL, payload, sizeof(payload),
			resp, sizeof(resp));
	if (ret < 0)
		return ret;
	if (ret < 3 || resp[0] != OWB_CMD_GENERAL_ACK || resp[2] != sub)
		return -EPROTO;
	return 0;
}

static int pogo_set_leds(struct pogo_kbd *kb)
{
	/* parameter 0x0D "led state": caps, reserved, mute, mic */
	const u8 payload[] = { 0x0D, 0x04, kb->caps_led, 0x00, 0x00, 0x00 };

	return pogo_xfer(kb, OWB_CMD_PARAM_SET, payload, sizeof(payload),
			 NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* input reporting (input_lock held)                                        */

static void pogo_report_keys(struct pogo_kbd *kb, const u8 *rep)
{
	struct input_dev *input = kb->kbd;
	int i;

	for (i = 0; i < 8; i++)
		input_report_key(input, pogo_keycode[i + 224], (rep[0] >> i) & 1);

	for (i = 2; i < POGO_KEY_REPORT_LEN; i++) {
		u8 old = kb->old_keys[i], new = rep[i];

		if (old > 3 && !memchr(rep + 2, old, 6)) {
			if (pogo_keycode[old])
				input_report_key(input, pogo_keycode[old], 0);
			else
				dev_dbg(kb->dev, "unknown scancode %#x released\n", old);
		}
		if (new > 3 && !memchr(kb->old_keys + 2, new, 6)) {
			if (pogo_keycode[new])
				input_report_key(input, pogo_keycode[new], 1);
			else
				dev_dbg(kb->dev, "unknown scancode %#x pressed\n", new);
		}
	}
	input_sync(input);
	memcpy(kb->old_keys, rep, POGO_KEY_REPORT_LEN);
}

static int pogo_media_keycode(u16 usage)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pogo_media_keys); i++)
		if (pogo_media_keys[i].usage == usage)
			return pogo_media_keys[i].keycode;
	return 0;
}

static bool pogo_media_has(const u8 *rep, u16 usage)
{
	return get_unaligned_le16(rep) == usage ||
	       get_unaligned_le16(rep + 2) == usage;
}

static void pogo_report_media(struct pogo_kbd *kb, const u8 *rep)
{
	struct input_dev *input = kb->kbd;
	int i;

	for (i = 0; i < POGO_MEDIA_REPORT_LEN; i += 2) {
		u16 old = get_unaligned_le16(kb->old_media + i);
		u16 new = get_unaligned_le16(rep + i);
		int code;

		if (old && !pogo_media_has(rep, old)) {
			code = pogo_media_keycode(old);
			if (code)
				input_report_key(input, code, 0);
		}
		if (new && !pogo_media_has(kb->old_media, new)) {
			code = pogo_media_keycode(new);
			if (code)
				input_report_key(input, code, 1);
			else
				dev_dbg(kb->dev, "unknown consumer usage %#x\n", new);
		}
	}
	input_sync(input);
	memcpy(kb->old_media, rep, POGO_MEDIA_REPORT_LEN);
}

/*
 * Touchpad payload: fingers x {flags, x_lo, x_hi, y_lo, y_hi} followed by
 * an optional 16 bit pressure, a finger count byte and a button byte.
 * flags: [7:4] slot id, [1] down, [1:0] palm (2 = palm).
 */
static void pogo_report_touchpad(struct pogo_kbd *kb, const u8 *data,
				 unsigned int len)
{
	struct input_dev *input = kb->tp;
	unsigned int fingers, i;
	u8 buttons;

	if (len < 2)
		return;
	buttons = data[len - 1];
	fingers = data[len - 2];
	if (fingers > POGO_TOUCH_FINGERS ||
	    fingers * POGO_TOUCH_REC_LEN + 2 > len)
		return;

	for (i = 0; i < fingers; i++) {
		const u8 *rec = data + i * POGO_TOUCH_REC_LEN;
		unsigned int slot = rec[0] >> 4;
		bool down = rec[0] & BIT(1);
		unsigned int tool = (rec[0] & 0x03) == 0x02 ?
				    MT_TOOL_PALM : MT_TOOL_FINGER;

		if (slot >= POGO_TOUCH_FINGERS)
			continue;
		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, tool, down);
		if (down) {
			input_report_abs(input, ABS_MT_POSITION_X,
					 get_unaligned_le16(rec + 1));
			input_report_abs(input, ABS_MT_POSITION_Y,
					 get_unaligned_le16(rec + 3));
		}
	}
	input_mt_sync_frame(input);
	/* clickpad: libinput derives left/right from the finger position */
	input_report_key(input, BTN_LEFT, buttons & 0x03);
	input_sync(input);
}

static void pogo_release_all(struct pogo_kbd *kb)
{
	static const u8 zeros[POGO_KEY_REPORT_LEN];

	if (kb->kbd) {
		pogo_report_keys(kb, zeros);
		pogo_report_media(kb, zeros);
	}
	if (kb->tp) {
		input_mt_sync_frame(kb->tp);
		input_report_key(kb->tp, BTN_LEFT, 0);
		input_sync(kb->tp);
	}
}

static int pogo_kbd_event(struct input_dev *input, unsigned int type,
			  unsigned int code, int value)
{
	struct pogo_kbd *kb = input_get_drvdata(input);

	if (type != EV_LED || code != LED_CAPSL)
		return -EINVAL;

	kb->caps_led = !!value;
	schedule_work(&kb->led_work);
	return 0;
}

static int pogo_register_inputs(struct pogo_kbd *kb)
{
	struct input_dev *kbd, *tp;
	const char *name;
	int i, ret;

	name = (kb->brand >= 1 && kb->brand <= ARRAY_SIZE(pogo_brand_names)) ?
		pogo_brand_names[kb->brand - 1] : "Pogo Keyboard";
	snprintf(kb->kbd_name, sizeof(kb->kbd_name), "%s", name);
	snprintf(kb->tp_name, sizeof(kb->tp_name), "%s Touchpad", name);

	kbd = input_allocate_device();
	tp = input_allocate_device();
	if (!kbd || !tp) {
		ret = -ENOMEM;
		goto err_free;
	}

	kbd->name = kb->kbd_name;
	kbd->phys = "pogo/input0";
	kbd->dev.parent = kb->dev;
	kbd->id.bustype = BUS_HOST;
	kbd->id.vendor = 0x22d9;
	kbd->id.product = 0x3869;
	kbd->id.version = 0x0010;
	kbd->event = pogo_kbd_event;
	input_set_drvdata(kbd, kb);
	__set_bit(EV_KEY, kbd->evbit);
	__set_bit(EV_REP, kbd->evbit);
	__set_bit(EV_LED, kbd->evbit);
	__set_bit(LED_CAPSL, kbd->ledbit);
	for (i = 0; i < ARRAY_SIZE(pogo_keycode); i++)
		if (pogo_keycode[i])
			__set_bit(pogo_keycode[i], kbd->keybit);
	for (i = 0; i < ARRAY_SIZE(pogo_media_keys); i++)
		__set_bit(pogo_media_keys[i].keycode, kbd->keybit);

	tp->name = kb->tp_name;
	tp->phys = "pogo/input1";
	tp->dev.parent = kb->dev;
	tp->id.bustype = BUS_HOST;
	tp->id.vendor = 0x22d9;
	tp->id.product = 0x3869;
	tp->id.version = 0x0010;
	input_set_drvdata(tp, kb);
	__set_bit(EV_KEY, tp->evbit);
	__set_bit(BTN_LEFT, tp->keybit);
	__set_bit(INPUT_PROP_POINTER, tp->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, tp->propbit);
	input_set_abs_params(tp, ABS_MT_POSITION_X, 0, kb->tp_max_x, 0, 0);
	input_set_abs_params(tp, ABS_MT_POSITION_Y, 0, kb->tp_max_y, 0, 0);
	input_abs_set_res(tp, ABS_MT_POSITION_X, kb->tp_res_x);
	input_abs_set_res(tp, ABS_MT_POSITION_Y, kb->tp_res_y);
	ret = input_mt_init_slots(tp, POGO_TOUCH_FINGERS,
				  INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED);
	if (ret)
		goto err_free;

	ret = input_register_device(kbd);
	if (ret)
		goto err_free;
	ret = input_register_device(tp);
	if (ret) {
		input_unregister_device(kbd);
		kbd = NULL;
		goto err_free;
	}

	mutex_lock(&kb->input_lock);
	kb->kbd = kbd;
	kb->tp = tp;
	mutex_unlock(&kb->input_lock);
	return 0;

err_free:
	input_free_device(tp);
	input_free_device(kbd);
	return ret;
}

static void pogo_unregister_inputs(struct pogo_kbd *kb)
{
	struct input_dev *kbd, *tp;

	mutex_lock(&kb->input_lock);
	pogo_release_all(kb);
	kbd = kb->kbd;
	tp = kb->tp;
	kb->kbd = NULL;
	kb->tp = NULL;
	memset(kb->old_keys, 0, sizeof(kb->old_keys));
	memset(kb->old_media, 0, sizeof(kb->old_media));
	mutex_unlock(&kb->input_lock);

	if (tp)
		input_unregister_device(tp);
	if (kbd)
		input_unregister_device(kbd);
}

/* ------------------------------------------------------------------------ */
/* rx path (serdev receive context)                                         */

static void pogo_handle_sync(struct pogo_kbd *kb, const u8 *p, unsigned int len)
{
	bool replug = false;

	if (len < 2)
		return;

	if (p[0] == OWB_SYNC_PLUG_IN && p[1] == 0x02 && len >= 9) {
		kb->brand = p[2];
		memcpy(kb->mac, &p[3], POGO_MAC_LEN);
		/*
		 * The MCU repeats this frame until the host has talked to it;
		 * only a plug-in frame after regular heartbeats is a re-plug.
		 */
		replug = kb->connected && kb->hb_seen;
	} else if (p[0] == OWB_SYNC_HEARTBEAT && p[1] == 0x02 && len >= 10) {
		kb->brand = p[3];
		memcpy(kb->mac, &p[4], POGO_MAC_LEN);
		kb->hb_seen = true;
	}

	if (!kb->connected || replug) {
		kb->connected = true;
		kb->hb_seen = false;
		kb->retry_ms = POGO_RETRY_MIN_MS;
		schedule_work(&kb->plug_work);
	}
}

static void pogo_handle_frame(struct pogo_kbd *kb, const u8 *f, unsigned int n)
{
	u8 src = f[1], cmd = f[3], len = f[4];
	const u8 *payload = &f[OWB_HDR_LEN];

	if (src == OWB_ADDR_PAD) {
		/* our own frame echoed back through the shared wire */
		if (READ_ONCE(kb->tx_pending) && cmd == kb->tx_cmd)
			complete(&kb->echo_done);
		return;
	}
	if (src != OWB_ADDR_KBD)
		return;

	kb->last_rx = jiffies;

	switch (cmd) {
	case OWB_CMD_KEYS:
		if (len < POGO_KEY_REPORT_LEN)
			break;
		mutex_lock(&kb->input_lock);
		if (kb->kbd)
			pogo_report_keys(kb, payload);
		mutex_unlock(&kb->input_lock);
		break;
	case OWB_CMD_MEDIA_KEYS:
		if (len < POGO_MEDIA_REPORT_LEN)
			break;
		mutex_lock(&kb->input_lock);
		if (kb->kbd)
			pogo_report_media(kb, payload);
		mutex_unlock(&kb->input_lock);
		break;
	case OWB_CMD_TOUCHPAD:
		mutex_lock(&kb->input_lock);
		if (kb->tp)
			pogo_report_touchpad(kb, payload, len);
		mutex_unlock(&kb->input_lock);
		break;
	case OWB_CMD_SYNC_UPLOAD:
		pogo_handle_sync(kb, payload, len);
		break;
	default:
		if (cmd == OWB_CMD_GENERAL_ACK && len >= 4 &&
		    payload[0] == OWB_GEN_BATTERY_STATUS)
			kb->battery_level = payload[2];

		if (READ_ONCE(kb->tx_pending) && cmd == kb->tx_cmd + 1) {
			/* reply layout for callers: cmd, len, payload */
			kb->resp_len = min_t(unsigned int, len + 2, sizeof(kb->resp));
			memcpy(kb->resp, &f[3], kb->resp_len);
			complete(&kb->resp_done);
		} else {
			dev_dbg(kb->dev, "unhandled frame cmd 0x%02x len %u\n",
				cmd, len);
		}
		break;
	}
}

static void pogo_rx_frame_done(struct pogo_kbd *kb)
{
	const u8 *f = kb->rx_buf;
	unsigned int n = kb->rx_len;
	u16 crc = (f[n - 5] << 8) | f[n - 4];

	if (f[n - 3] != OWB_END || f[n - 2] != OWB_TAIL_SYNC ||
	    f[n - 1] != OWB_TAIL_SYNC) {
		dev_dbg(kb->dev, "bad frame trailer\n");
		return;
	}
	if (crc != owb_crc16(f, n - 5)) {
		dev_dbg(kb->dev, "bad crc %04x\n", crc);
		return;
	}
	pogo_handle_frame(kb, f, n);
}

static void pogo_rx_byte(struct pogo_kbd *kb, u8 c)
{
	if (!kb->in_frame) {
		if (c == OWB_HEAD_SYNC) {
			kb->sync_cnt++;
			return;
		}
		if ((c == OWB_START || c == OWB_START_REPEAT) &&
		    kb->sync_cnt >= 4) {
			kb->in_frame = true;
			kb->rx_len = 0;
			kb->rx_expected = 0;
			kb->rx_buf[kb->rx_len++] = c;
		}
		kb->sync_cnt = 0;
		return;
	}

	kb->rx_buf[kb->rx_len++] = c;

	if (kb->rx_len == OWB_HDR_LEN) {
		kb->rx_expected = OWB_HDR_LEN + c + OWB_TRAILER_LEN;
		if (kb->rx_expected > sizeof(kb->rx_buf)) {
			kb->in_frame = false;
			return;
		}
	}
	if (!kb->rx_expected || kb->rx_len < kb->rx_expected)
		return;

	kb->in_frame = false;
	pogo_rx_frame_done(kb);
}

static size_t pogo_receive_buf(struct serdev_device *serdev, const u8 *data,
			       size_t count)
{
	struct pogo_kbd *kb = serdev_device_get_drvdata(serdev);
	size_t i;

	print_hex_dump_debug("pogo rx: ", DUMP_PREFIX_NONE, 32, 1, data, count, false);
	for (i = 0; i < count; i++)
		pogo_rx_byte(kb, data[i]);
	return count;
}

static const struct serdev_device_ops pogo_serdev_ops = {
	.receive_buf = pogo_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

/* ------------------------------------------------------------------------ */
/* power / attach state machine (process context)                           */

static void pogo_wake_irq_enable(struct pogo_kbd *kb, bool enable)
{
	if (enable == kb->wake_irq_enabled)
		return;
	if (enable)
		enable_irq(kb->wake_irq);
	else
		disable_irq(kb->wake_irq);
	kb->wake_irq_enabled = enable;
}

static void pogo_power_on(struct pogo_kbd *kb)
{
	dev_dbg(kb->dev, "power on\n");
	kb->connected = false;
	kb->hb_seen = false;
	kb->battery_level = -1;
	kb->last_rx = jiffies;
	kb->powered = true;
	gpiod_set_value_cansleep(kb->power_gpio, 1);
	mod_delayed_work(system_dfl_wq, &kb->hb_work,
			 msecs_to_jiffies(POGO_HB_POLL_MS));
}

static void pogo_power_off(struct pogo_kbd *kb)
{
	dev_dbg(kb->dev, "power off\n");
	kb->powered = false;
	gpiod_set_value_cansleep(kb->power_gpio, 0);
	if (kb->connected) {
		kb->connected = false;
		pogo_unregister_inputs(kb);
		dev_info(kb->dev, "keyboard detached\n");
	}
}

static void pogo_detect_work(struct work_struct *work)
{
	struct pogo_kbd *kb = container_of(work, struct pogo_kbd,
					   detect_work.work);

	int i;

	if (kb->powered)
		return;

	/* the unpowered keyboard holds the wire low; require a stable level */
	for (i = 0; i < POGO_DETECT_SAMPLES; i++) {
		if (!gpiod_get_value_cansleep(kb->wake_gpio))
			break;
		msleep(POGO_DETECT_SAMPLE_MS);
	}
	if (i == POGO_DETECT_SAMPLES) {
		pogo_power_on(kb);
		return;
	}

	kb->retry_ms = POGO_RETRY_MIN_MS;
	pogo_wake_irq_enable(kb, true);
	/* the edge may have happened before the irq was armed */
	if (gpiod_get_value_cansleep(kb->wake_gpio))
		mod_delayed_work(system_dfl_wq, &kb->detect_work,
				 msecs_to_jiffies(POGO_DETECT_DEBOUNCE_MS));
}

static void pogo_hb_work(struct work_struct *work)
{
	struct pogo_kbd *kb = container_of(work, struct pogo_kbd, hb_work.work);
	unsigned int timeout = kb->connected ? POGO_HB_TIMEOUT_MS :
					       POGO_BOOT_TIMEOUT_MS;

	if (!kb->powered)
		return;

	if (time_before(jiffies, kb->last_rx + msecs_to_jiffies(timeout))) {
		schedule_delayed_work(&kb->hb_work,
				      msecs_to_jiffies(POGO_HB_POLL_MS));
		return;
	}

	dev_dbg(kb->dev, "heartbeat lost (%s)\n",
		kb->connected ? "connected" : "booting");
	pogo_power_off(kb);

	/* re-arm: retry while the wake line still reports a keyboard */
	schedule_delayed_work(&kb->detect_work, msecs_to_jiffies(kb->retry_ms));
	kb->retry_ms = min(kb->retry_ms * 2, POGO_RETRY_MAX_MS);
}

static void pogo_plug_work(struct work_struct *work)
{
	struct pogo_kbd *kb = container_of(work, struct pogo_kbd, plug_work);
	int ret;

	if (!kb->powered || !kb->connected)
		return;

	if (!kb->kbd) {
		ret = pogo_register_inputs(kb);
		if (ret) {
			dev_err(kb->dev, "failed to register input devices: %d\n",
				ret);
			return;
		}
		dev_info(kb->dev, "keyboard attached: %s (brand %u, mac %pM)\n",
			 kb->kbd_name, kb->brand, kb->mac);
	}

	/*
	 * Report the host as awake: this is what moves the MCU from the
	 * repeated plug-in sync frames to regular heartbeats.  Then restore
	 * the host side state on the (possibly re-plugged) MCU.
	 */
	ret = pogo_set_general(kb, OWB_GEN_SLEEP, 0);
	if (ret)
		dev_warn(kb->dev, "wake notify failed: %d\n", ret);
	pogo_set_leds(kb);
	if (kb->brightness)
		pogo_set_general(kb, OWB_GEN_BRIGHTNESS, kb->brightness);
}

/* ------------------------------------------------------------------------ */
/* keyboard backlight as a LED class device (picked up by UPower/GNOME)    */

static int pogo_backlight_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct pogo_kbd *kb = container_of(cdev, struct pogo_kbd, backlight);

	kb->brightness = value;
	/* when detached just remember the level; plug_work restores it */
	if (!kb->connected)
		return 0;
	return pogo_set_general(kb, OWB_GEN_BRIGHTNESS, value);
}

static int pogo_register_backlight(struct pogo_kbd *kb)
{
	struct led_init_data init_data = {
		/* no colour: yields "pogo-keyboard::kbd_backlight" */
		.default_label = ":" LED_FUNCTION_KBD_BACKLIGHT,
		.devicename = "pogo-keyboard",
	};

	kb->backlight.max_brightness = POGO_BACKLIGHT_MAX;
	kb->backlight.brightness_set_blocking = pogo_backlight_set;
	kb->backlight.flags = LED_CORE_SUSPENDRESUME;

	return devm_led_classdev_register_ext(kb->dev, &kb->backlight, &init_data);
}

static void pogo_led_work(struct work_struct *work)
{
	struct pogo_kbd *kb = container_of(work, struct pogo_kbd, led_work);

	if (kb->connected)
		pogo_set_leds(kb);
}

static irqreturn_t pogo_wake_isr(int irq, void *data)
{
	struct pogo_kbd *kb = data;

	/* the wire carries UART traffic once powered; stay quiet until then */
	disable_irq_nosync(irq);
	kb->wake_irq_enabled = false;
	mod_delayed_work(system_dfl_wq, &kb->detect_work,
			 msecs_to_jiffies(POGO_DETECT_DEBOUNCE_MS));
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------------ */
/* sysfs: keyboard battery and link state                                   */

static ssize_t battery_level_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct pogo_kbd *kb = dev_get_drvdata(dev);

	if (!kb->connected)
		return -ENODEV;
	/* only keyboards with a battery push 0x0E status frames */
	if (kb->battery_level < 0)
		return -ENODATA;
	return sysfs_emit(buf, "%d\n", kb->battery_level);
}
static DEVICE_ATTR_RO(battery_level);

static ssize_t connected_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct pogo_kbd *kb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", kb->connected);
}
static DEVICE_ATTR_RO(connected);

static struct attribute *pogo_attrs[] = {
	&dev_attr_battery_level.attr,
	&dev_attr_connected.attr,
	NULL
};
ATTRIBUTE_GROUPS(pogo);

/* ------------------------------------------------------------------------ */
/* probe / remove                                                           */

static int pogo_parse_dt(struct pogo_kbd *kb)
{
	struct device *dev = kb->dev;
	u32 val[2];

	kb->power_gpio = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(kb->power_gpio))
		return dev_err_probe(dev, PTR_ERR(kb->power_gpio), "power gpio\n");

	kb->tx_en_gpio = devm_gpiod_get(dev, "tx-enable", GPIOD_OUT_LOW);
	if (IS_ERR(kb->tx_en_gpio))
		return dev_err_probe(dev, PTR_ERR(kb->tx_en_gpio),
				     "tx-enable gpio\n");

	kb->wake_gpio = devm_gpiod_get(dev, "wake", GPIOD_IN);
	if (IS_ERR(kb->wake_gpio))
		return dev_err_probe(dev, PTR_ERR(kb->wake_gpio), "wake gpio\n");

	kb->wake_irq = gpiod_to_irq(kb->wake_gpio);
	if (kb->wake_irq < 0)
		return dev_err_probe(dev, kb->wake_irq, "wake irq\n");

	kb->tp_max_x = 2560;
	kb->tp_max_y = 1440;
	if (!device_property_read_u32_array(dev, "touchpad-xy-max", val, 2)) {
		kb->tp_max_x = val[0];
		kb->tp_max_y = val[1];
	}
	if (!device_property_read_u32_array(dev, "touchpad-xy-resolution",
					    val, 2)) {
		kb->tp_res_x = val[0];
		kb->tp_res_y = val[1];
	}
	return 0;
}

static int pogo_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct pogo_kbd *kb;
	int ret;

	kb = devm_kzalloc(dev, sizeof(*kb), GFP_KERNEL);
	if (!kb)
		return -ENOMEM;

	kb->serdev = serdev;
	kb->dev = dev;
	kb->battery_level = -1;
	kb->retry_ms = POGO_RETRY_MIN_MS;
	mutex_init(&kb->io_lock);
	mutex_init(&kb->input_lock);
	init_completion(&kb->echo_done);
	init_completion(&kb->resp_done);
	INIT_DELAYED_WORK(&kb->detect_work, pogo_detect_work);
	INIT_DELAYED_WORK(&kb->hb_work, pogo_hb_work);
	INIT_WORK(&kb->plug_work, pogo_plug_work);
	INIT_WORK(&kb->led_work, pogo_led_work);
	serdev_device_set_drvdata(serdev, kb);

	ret = pogo_parse_dt(kb);
	if (ret)
		return ret;

	serdev_device_set_client_ops(serdev, &pogo_serdev_ops);
	ret = devm_serdev_device_open(dev, serdev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to open serdev\n");

	ret = serdev_device_set_baudrate(serdev, POGO_BAUD);
	if (ret != POGO_BAUD)
		dev_warn(dev, "baudrate set to %d instead of %d\n", ret, POGO_BAUD);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set parity\n");

	ret = pogo_register_backlight(kb);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register backlight\n");

	ret = devm_request_irq(dev, kb->wake_irq, pogo_wake_isr,
			       IRQF_TRIGGER_FALLING | IRQF_NO_AUTOEN,
			       dev_name(dev), kb);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request wake irq\n");

	/* evaluate the current attach state; enables the irq when idle */
	schedule_delayed_work(&kb->detect_work,
			      msecs_to_jiffies(POGO_DETECT_DEBOUNCE_MS));

	dev_info(dev, "pogo keyboard link ready (wake irq %d)\n", kb->wake_irq);
	return 0;
}

static void pogo_remove(struct serdev_device *serdev)
{
	struct pogo_kbd *kb = serdev_device_get_drvdata(serdev);

	pogo_wake_irq_enable(kb, false);
	cancel_delayed_work_sync(&kb->detect_work);
	cancel_delayed_work_sync(&kb->hb_work);
	cancel_work_sync(&kb->plug_work);
	cancel_work_sync(&kb->led_work);
	pogo_power_off(kb);
}

static const struct of_device_id pogo_of_match[] = {
	{ .compatible = "oneplus,pogo-keyboard" },
	{ }
};
MODULE_DEVICE_TABLE(of, pogo_of_match);

static struct serdev_device_driver pogo_driver = {
	.probe = pogo_probe,
	.remove = pogo_remove,
	.driver = {
		.name = "oneplus-pogo-kbd",
		.of_match_table = pogo_of_match,
		.dev_groups = pogo_groups,
	},
};
module_serdev_device_driver(pogo_driver);

MODULE_DESCRIPTION("OnePlus/OPPO Pad pogo-pin keyboard driver");
MODULE_LICENSE("GPL");
