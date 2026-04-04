// SPDX-License-Identifier: GPL-2.0
/*
 * hid-omg-detect.c - Malicious HID Device Detection Kernel Module
 *
 * Detects O.MG Cable and similar BadUSB devices by combining:
 *   1. Keystroke timing entropy analysis (human vs machine rhythm)
 *   2. Plug-and-type detection (typing immediately after connect)
 *   3. USB descriptor fingerprinting (known bad VID/PIDs + anomalies)
 *
 * When suspicion score >= threshold, logs a kernel warning and suggests
 * blocking the device with usbguard.
 *
 * The driver is purely passive — it does not drop, modify, or delay any
 * HID events.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/math64.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zübeyr Almaho <zybo1000@gmail.com>");
MODULE_DESCRIPTION("O.MG Cable / Malicious HID Device Detector");

/* ============================================================
 * Tuneable parameters (pass as insmod args)
 * ============================================================ */
static int score_threshold = 70;
module_param(score_threshold, int, 0644);
MODULE_PARM_DESC(score_threshold,
	"Suspicion score (0-100) to trigger alert (default: 70)");

static int plug_type_ms = 500;
module_param(plug_type_ms, int, 0644);
MODULE_PARM_DESC(plug_type_ms,
	"Max ms after connect before first key = plug-and-type (default: 500)");

static int entropy_variance_low = 500;
module_param(entropy_variance_low, int, 0644);
MODULE_PARM_DESC(entropy_variance_low,
	"Variance (us^2) below which typing is machine-like (default: 500)");

/* ============================================================
 * Constants
 * ============================================================ */
#define DRIVER_NAME		"hid_omg_detect"
#define MAX_TIMING_SAMPLES	64
#define MIN_SAMPLES		10

/* ============================================================
 * Known suspicious VID/PID pairs (O.MG Cable, Rubber Ducky, etc.)
 * ============================================================ */
static const struct {
	u16 vid;
	u16 pid;
	const char *name;
} suspicious_ids[] = {
	{ 0x16c0, 0x27db, "O.MG Cable (classic)" },
	{ 0x1209, 0xa000, "O.MG Cable (Elite)" },
	{ 0x16c0, 0x27dc, "USB Rubber Ducky" },
	{ 0x1b4f, 0x9208, "LilyPad Arduino BadUSB" },
};

/* ============================================================
 * Per-device state (allocated on probe, freed on remove)
 * ============================================================ */
struct omg_device_state {
	struct hid_device  *hdev;

	/* --- connection timing --- */
	ktime_t		connect_time;
	ktime_t		first_key_time;
	bool		first_key_seen;

	/* --- keystroke interval circular buffer --- */
	ktime_t		last_key_time;
	s64		intervals_us[MAX_TIMING_SAMPLES];
	unsigned int	sample_count;
	unsigned int	sample_head;

	/* --- USB info (immutable after probe) --- */
	u16		vid;
	u16		pid;
	const char	*vid_pid_match_name;	/* non-NULL if VID/PID matched */
	bool		descriptor_anomaly;

	/* --- verdict --- */
	int		score;
	bool		alerted;

	spinlock_t	lock;
};

/* ============================================================
 * Math helpers
 * ============================================================ */
static void timing_stats(struct omg_device_state *s, s64 *mean, s64 *variance)
{
	unsigned int i, n;
	s64 sum = 0, sq = 0, m;

	n = min(s->sample_count, (unsigned int)MAX_TIMING_SAMPLES);

	if (n < 2) {
		*mean = 0;
		*variance = 0;
		return;
	}

	for (i = 0; i < n; i++)
		sum += s->intervals_us[i];
	m = div_s64(sum, n);

	for (i = 0; i < n; i++) {
		s64 d = s->intervals_us[i] - m;

		sq += d * d;
	}

	*mean     = m;
	*variance = div_s64(sq, n);
}

/* ============================================================
 * Signal 1: Timing entropy
 *
 * Humans: high variance (pauses, rhythm changes)
 * Machines: low variance (constant clock)
 *
 * Also penalises extremely fast consistent typing (mean < 15 ms).
 * ============================================================ */
static int score_entropy(struct omg_device_state *s)
{
	s64 mean, var;
	int pts = 0;

	if (s->sample_count < MIN_SAMPLES)
		return 0;

	timing_stats(s, &mean, &var);

	if (var < (s64)entropy_variance_low)
		pts += 35;
	else if (var < (s64)entropy_variance_low * 10)
		pts += 20;
	else if (var < (s64)entropy_variance_low * 40)
		pts += 8;

	if (mean > 0 && mean < 5000)
		pts += 25;
	else if (mean > 0 && mean < 15000)
		pts += 10;

	return min(pts, 60);
}

/* ============================================================
 * Signal 2: Plug-and-type
 *
 * A legitimate keyboard sits idle for a while after connect.
 * A script device starts injecting within milliseconds.
 * ============================================================ */
static int score_plug_type(struct omg_device_state *s)
{
	s64 delta_ms;

	if (!s->first_key_seen)
		return 0;

	delta_ms = ktime_to_ms(ktime_sub(s->first_key_time, s->connect_time));

	if (delta_ms < 100)
		return 30;
	if (delta_ms < (s64)plug_type_ms)
		return 15;
	if (delta_ms < 2000)
		return 5;

	return 0;
}

/* ============================================================
 * Signal 3: USB descriptor fingerprint
 * ============================================================ */
static int score_descriptor(struct omg_device_state *s)
{
	int pts = 0;

	if (s->vid_pid_match_name)
		pts += 50;

	if (s->descriptor_anomaly)
		pts += 20;

	return min(pts, 50);
}

/* ============================================================
 * Combine all signals and return per-signal breakdown.
 * Caller is responsible for alerting outside any lock.
 * ============================================================ */
struct omg_score_result {
	int entropy;
	int plug_type;
	int descriptor;
	int total;
	bool newly_alerted;
};

static void compute_score(struct omg_device_state *s,
			   struct omg_score_result *res)
{
	res->entropy   = score_entropy(s);
	res->plug_type = score_plug_type(s);
	res->descriptor = score_descriptor(s);
	res->total     = min(res->entropy + res->plug_type + res->descriptor,
			     100);

	s->score = res->total;
	res->newly_alerted = false;

	if (res->total >= score_threshold && !s->alerted) {
		s->alerted = true;
		res->newly_alerted = true;
	}
}

/* Emit alert outside of spinlock */
static void emit_alert(struct hid_device *hdev,
		       struct omg_device_state *s,
		       const struct omg_score_result *res)
{
	hid_warn(hdev, "=============================================\n");
	hid_warn(hdev, "!! SUSPICIOUS HID DEVICE DETECTED !!\n");
	hid_warn(hdev, "Device : %s\n", hdev->name);
	hid_warn(hdev, "VID/PID: %04x:%04x\n", s->vid, s->pid);
	if (s->vid_pid_match_name)
		hid_warn(hdev, "Match  : %s\n", s->vid_pid_match_name);
	hid_warn(hdev, "Score  : %d/100  (threshold=%d)\n",
		 res->total, score_threshold);
	hid_warn(hdev, "  Entropy      : %d pts\n", res->entropy);
	hid_warn(hdev, "  Plug-and-type: %d pts\n", res->plug_type);
	hid_warn(hdev, "  Descriptor   : %d pts\n", res->descriptor);
	hid_warn(hdev, "Action : sudo usbguard block-device <id>\n");
	hid_warn(hdev, "=============================================\n");
}

/* ============================================================
 * HID raw_event hook — called for every incoming HID report
 *
 * This runs in softirq/BH context — no sleeping locks allowed.
 * ============================================================ */
static int omg_raw_event(struct hid_device *hdev,
			 struct hid_report *report,
			 u8 *data, int size)
{
	struct omg_device_state *s = hid_get_drvdata(hdev);
	struct omg_score_result res;
	ktime_t now;
	unsigned long flags;
	bool keystroke = false;
	int i;

	if (!s)
		return 0;

	if (report->type != HID_INPUT_REPORT)
		return 0;

	/* Byte 0 = modifier, byte 1 = reserved, bytes 2-7 = keycodes */
	for (i = 2; i < size && i < 8; i++) {
		if (data[i] != 0) {
			keystroke = true;
			break;
		}
	}
	if (size > 0 && data[0] != 0)
		keystroke = true;

	if (!keystroke)
		return 0;

	now = ktime_get();

	spin_lock_irqsave(&s->lock, flags);

	if (!s->first_key_seen) {
		s->first_key_seen = true;
		s->first_key_time = now;
		s->last_key_time  = now;
	} else {
		s64 interval = ktime_to_us(ktime_sub(now, s->last_key_time));

		/* ignore idle gaps > 10 s */
		if (interval < 10000000LL) {
			s->intervals_us[s->sample_head] = interval;
			s->sample_head =
				(s->sample_head + 1) % MAX_TIMING_SAMPLES;
			if (s->sample_count < MAX_TIMING_SAMPLES)
				s->sample_count++;
		}
		s->last_key_time = now;
	}

	compute_score(s, &res);

	spin_unlock_irqrestore(&s->lock, flags);

	/* Alert outside the spinlock — hid_warn may sleep */
	if (res.newly_alerted)
		emit_alert(hdev, s, &res);

	return 0;
}

/* ============================================================
 * HID probe — device connected
 * ============================================================ */
static int omg_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct omg_device_state *s;
	struct usb_interface *intf;
	struct usb_device *udev;
	struct omg_score_result res;
	int ret, i;

	if (hdev->bus != BUS_USB)
		return -ENODEV;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	spin_lock_init(&s->lock);
	s->hdev         = hdev;
	s->connect_time = ktime_get();

	/* Extract USB descriptor info */
	intf = to_usb_interface(hdev->dev.parent);
	udev = interface_to_usbdev(intf);
	s->vid = le16_to_cpu(udev->descriptor.idVendor);
	s->pid = le16_to_cpu(udev->descriptor.idProduct);

	/* Check known bad VID/PID table (once, at probe time) */
	for (i = 0; i < ARRAY_SIZE(suspicious_ids); i++) {
		if (s->vid == suspicious_ids[i].vid &&
		    s->pid == suspicious_ids[i].pid) {
			s->vid_pid_match_name = suspicious_ids[i].name;
			break;
		}
	}

	/*
	 * Anomaly: legitimate HID keyboards use bDeviceClass = 0x00
	 * (class defined at interface level).  Any other value here
	 * for a device presenting as a keyboard is suspicious.
	 */
	s->descriptor_anomaly =
		(udev->descriptor.bDeviceClass != 0x00 &&
		 udev->descriptor.bDeviceClass != 0x03);

	hid_set_drvdata(hdev, s);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed: %d\n", ret);
		goto err_free;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "hid_hw_start failed: %d\n", ret);
		goto err_free;
	}

	hid_info(hdev, "Monitoring %s (VID=%04x PID=%04x%s%s)\n",
		 hdev->name, s->vid, s->pid,
		 s->vid_pid_match_name ? " KNOWN-BAD:" : "",
		 s->vid_pid_match_name ? s->vid_pid_match_name : "");

	if (s->descriptor_anomaly)
		hid_warn(hdev, "Descriptor anomaly: bDeviceClass=0x%02x\n",
			 udev->descriptor.bDeviceClass);

	/* Run initial descriptor-only score */
	compute_score(s, &res);
	if (res.newly_alerted)
		emit_alert(hdev, s, &res);

	return 0;

err_free:
	hid_set_drvdata(hdev, NULL);
	kfree(s);
	return ret;
}

/* ============================================================
 * HID remove — device disconnected
 * ============================================================ */
static void omg_remove(struct hid_device *hdev)
{
	struct omg_device_state *s = hid_get_drvdata(hdev);

	hid_hw_stop(hdev);

	if (s) {
		hid_info(hdev, "Removed %s (final score: %d/100)\n",
			 hdev->name, s->score);
		hid_set_drvdata(hdev, NULL);
		kfree(s);
	}
}

/* Match all USB HID devices — we inspect and score them all */
static const struct hid_device_id omg_table[] = {
	{ HID_USB_DEVICE(HID_ANY_ID, HID_ANY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, omg_table);

static struct hid_driver omg_driver = {
	.name      = DRIVER_NAME,
	.id_table  = omg_table,
	.probe     = omg_probe,
	.remove    = omg_remove,
	.raw_event = omg_raw_event,
};

module_hid_driver(omg_driver);
