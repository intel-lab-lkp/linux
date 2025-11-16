// SPDX-License-Identifier: GPL-2.0
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#include "ilitek_report.h"
#include "ilitek_crypto.h"

static bool is_debug_packet_id(u8 id, u8 _interface)
{
	return ((_interface == interface_i2c && id == 0xdb) ||
		(_interface == interface_usb && id == 0xaa));
}

static bool is_pen_packet_id(u8 id)
{
	return (id == 0x0c || id == 0x0d);
}

static void touch_decode(struct ilitek_ts_device *dev,
			 struct ilitek_report *report,
			 u8 *buf, u8 cnt)
{
	struct touch_fmt *parser, *finger;
	struct touch_iwb_fmt *parser_iwb;
	u8 i, j, offset;
	u32 size = dev->fmt.touch_size;

	offset = (dev->quirks & QUIRK_BRIDGE) ? 4 : 0;

	for (i = 0; i < cnt && i < dev->tp_info.max_fingers; i++) {
		finger = &report->touch.finger[i];

		do {
			if (dev->protocol.flag == PTL_V3 &&
			    dev->_interface == interface_i2c) {
				finger->id = i;
				finger->status = buf[1 + offset + i * 5] >> 7;
				finger->x = ((buf[1 + offset + i * 5] & 0x3F) << 8) +
					buf[2 + offset + i * 5];
				finger->y = ((buf[3 + offset + i * 5] & 0x3F) << 8) +
					buf[4 + offset + i * 5];
				finger->pressure = buf[5 + offset + i * 5];
				finger->height = 128;
				finger->width = 1;

				break;
			}

			parser = (struct touch_fmt *)
				(buf + 1 + i * size);
			parser_iwb = (struct touch_iwb_fmt *)
				(buf + 1 + i * size);

			finger->id = parser->id;
			finger->status = parser->status;
			finger->x = parser->x;
			finger->y = parser->y;

			if (dev->setting.default_format_enabled)
				break;

			switch (dev->tp_info.format) {
			case touch_fmt_0x1:
				finger->pressure = parser->pressure;
				break;
			case touch_fmt_0x2:
				finger->width = parser->width;
				finger->height = parser->height;
				break;
			case touch_fmt_0x3:
				finger->pressure = parser->pressure;
				finger->width = parser->width;
				finger->height = parser->height;
				break;
			case touch_fmt_0x4:
				finger->id = parser_iwb->id;
				finger->status =
					(parser_iwb->status == 0x7) ? 1 : 0;
				finger->x = parser_iwb->x;
				finger->y = parser_iwb->y;
				finger->width = parser_iwb->width;
				finger->height = parser_iwb->height;
				break;
			case touch_fmt_0x10:
				finger->width = le16(buf + i * size + 6);
				finger->height = le16(buf + i * size + 8);
				finger->algo = buf[i * size + 10];
				report->touch.algo = 0;
				break;
			case touch_fmt_0x11:
				finger->id =
					(!finger->id) ? 0 : finger->id - 1;
				break;
			}
		} while (false);

		TP_DBG(dev->id,
		       "[touch-report] id:%hhu, status:%hhu, x:%hu, y:%hu, p:%hhu, w:%hu, h:%hu, algo: 0x%hhx\n",
			finger->id, finger->status, finger->x, finger->y,
			finger->pressure, finger->width, finger->height,
			finger->algo);

		if (finger->id >= dev->tp_info.max_fingers) {
			TP_ERR(dev->id, "invalid touch id: %hhu >= %hhu\n",
				finger->id, dev->tp_info.max_fingers);
			return;
		}

		/*
		 * if x/y within key's range, skip touch range check.
		 */
		for (j = 0; j < dev->tp_info.key_num; j++) {
			if ((finger->x < dev->key.info.keys[j].x &&
			     finger->x > dev->key.info.keys[j].x +
				     dev->key.info.x_len) &&
			    (finger->y < dev->key.info.keys[j].y &&
			     finger->y > dev->key.info.keys[j].y +
				     dev->key.info.y_len))
				continue;

			goto skip_touch_range_check;
		}

		if (finger->status &&
		    (finger->x - 1 > dev->screen_info.x_max ||
		     finger->y - 1 > dev->screen_info.y_max ||
		     finger->x < dev->screen_info.x_min ||
		     finger->y < dev->screen_info.y_min)) {
			TP_ERR(dev->id, "Point[%d]: (%d, %d), Limit: (%d:%d, %d:%d) OOB\n",
				finger->id, finger->x, finger->y,
				dev->screen_info.x_min, dev->screen_info.x_max,
				dev->screen_info.y_min, dev->screen_info.y_max);
			return;
		}

skip_touch_range_check:
		continue;
	}

	report->touch.cnt = i;

	/*
	 * report touch event callback,
	 * which includes actual count of finger report just parsed above.
	 */
	if (report->cb.report_touch_event)
		report->cb.report_touch_event(&report->touch, report->_private);
}

static void pen_decode(struct ilitek_ts_device *dev,
		       struct ilitek_report *report,
		       u8 *buf)
{
	struct pen_fmt *parser = (struct pen_fmt *)(buf + 1);
	struct pen_fmt *pen = &report->pen.pen;

	report->pen.cnt = buf[61];
	report->pen.algo = buf[62];

	memcpy(pen, parser, sizeof(struct pen_fmt));

	switch (dev->tp_info.pen_format) {
	default:
		TP_DBG(dev->id,
		       "[stylus-report] state:0x%hhx, x:%hu, y:%hu, pressure: %hu, x_tilt: %hd, y_tilt: %hd, battery: %hhu\n",
			pen->modes, pen->x, pen->y, pen->pressure,
			pen->x_tilt, pen->y_tilt, pen->battery);
		break;
	case 1:
		TP_DBG(dev->id,
		       "[stylus-report] state:0x%hhx, x:%hu, y:%hu, pressure: %hu, x_tilt: %hd, y_tilt: %hd, battery: %hhu, barrel_pressure: %hu, idx: %hhu, color: %hhu, width: %hhu, style: %hhu\n",
			pen->modes, pen->x, pen->y, pen->pressure,
			pen->x_tilt, pen->y_tilt, pen->battery, pen->usi_1.barrel_pressure,
			pen->usi_1.idx, pen->usi_1.color, pen->usi_1.width,
			pen->usi_1.style);
		break;
	case 2:
		TP_DBG(dev->id,
		       "[stylus-report] state:0x%hhx, x:%hu, y:%hu, pressure: %hu, x_tilt: %hd, y_tilt: %hd, battery: %hhu, barrel_pressure: %hu, idx: %hhu, color24: %u, no_color: %hhu, width: %hhu, style: %hhu\n",
			pen->modes, pen->x, pen->y, pen->pressure,
			pen->x_tilt, pen->y_tilt, pen->battery, pen->usi_2.barrel_pressure,
			pen->usi_2.idx, le32(pen->usi_2.color_24, 3),
			pen->usi_2.no_color, pen->usi_2.width,
			pen->usi_2.style);
		break;
	}

	/* report pen event callback */
	if (report->cb.report_pen_event)
		report->cb.report_pen_event(&report->pen, report->_private);
}

static void dmsg_decode(struct ilitek_ts_device *dev,
			struct ilitek_report *report,
			u8 *buf, int buf_size)
{
	if ((int)buf[1] >= buf_size)
		return;

	buf[buf[1]] = '\0';
	TP_DBG(dev->id, "%s\n", (char *)(buf + 2));

	if (report->cb.report_dmsg)
		report->cb.report_dmsg((char *)(buf + 2),
					buf_size - 2, report->_private);
}

static void report_buf(struct ilitek_report *report, u8 *buf,
		       bool is_last)
{
	if (!report->cb.report_buf)
		return;

	report->cb.report_buf(buf, 64, is_last, report->_private);
}

/* return touch finger's count or negative value as error code */
static int report_get_raw_v3(struct ilitek_ts_device *dev,
			     struct ilitek_report *report,
			     u8 *buf, int buf_size)
{
	int error;

	u8 cnt;
	int idx = (dev->quirks & QUIRK_BRIDGE) ? 4 : 0;

	UNUSED(buf_size);

	if (dev->_interface == interface_i2c) {
		if (dev->quirks & QUIRK_BRIDGE) {
			error = read_interrupt_in(dev, buf, 64, 1000);
			if (error < 0)
				return error;

			if (buf[0] != 0x03 || buf[1] != 0xa3 || buf[2] != 0x10)
				return -EAGAIN;

			/* move algo byte to index 62 */
			buf[62] = buf[35];
		} else if (dev->quirks & QUIRK_DAEMON_I2C ||
			   dev->quirks & QUIRK_BRIDGE ||
			   dev->quirks & QUIRK_WIFI_ITS_I2C) {
			error = read_interrupt_in(dev, buf, 64, 1000);
			if (error < 0)
				return error;
		} else {
			dev->wbuf[0] = 0x10;
			error = write_then_read(dev, dev->wbuf, 1, buf, 32);
			if (error < 0)
				return error;

			report->touch.algo = buf[31];
			buf[31] = 0;

			if (buf[0] == 2) {
				error = write_then_read(dev, NULL, 0,
							 buf + 31, 20);
				if (error < 0)
					return error;
			}

			/* move algo byte to index 62 */
			buf[62] = report->touch.algo;
		}

		switch (dev->rbuf[idx]) {
		default:
		case 1:
			cnt = 6;
			break;
		case 0:
			cnt = 0;
			break;
		case 2:
			cnt = 10;
			break;
		}
	} else {
		error = read_interrupt_in(dev, buf, 64, 1000);
		if (error < 0)
			return error;

		cnt = buf[55];

		/* move algo byte to index 62 */
		buf[62] = buf[56];
	}

	report->touch.algo = buf[62];

	report->touch.dbg_size = 64;
	memcpy(report->touch.dbg, buf, 64);

	report_buf(report, buf, true);

	return cnt;
}

static bool is_iwb_fmt(struct ilitek_ts_device *dev)
{
	return dev->tp_info.format == touch_fmt_0x4 &&
		!dev->setting.default_format_enabled;
}

static bool need_skip_checksum(struct ilitek_ts_device *dev,
			       struct ilitek_report *report,
			       u8 packet_id, bool is_first)
{
	/*
	 * don't check checksum for below situation:
	 *   1. debug packet
	 *   2. normal mode pen packet w/ HID.
	 *   3. IWB format packet
	 */

	if (report->skip_checksum)
		return true;

	if (is_first) {
		if (is_debug_packet_id(packet_id, dev->_interface))
			return true;

		if ((dev->_interface == interface_usb ||
		     dev->_interface == interface_hid_over_i2c) &&
		    packet_id == 0x0d)
			return true;
	}

	if (is_iwb_fmt(dev))
		return true;

	return false;
}

/* return touch finger's count or negative value as error code */
static int report_get_raw_v6(struct ilitek_ts_device *dev,
			     struct ilitek_report *report,
			     u8 *buf, int buf_size)
{
	int error;
	u8 i, cnt;
	u8 size = dev->fmt.touch_size, max_cnt = dev->fmt.touch_max_cnt;
	u8 tmp[64];

	UNUSED(buf_size);

	error = read_interrupt_in(dev, tmp, 64, 1000);
	if (error < 0)
		return error;

	if (dev->tp_info.format == 5)
		ilitek_decrypt(tmp + 1, 48);

	/* set packet id to 0x48 forcely for using BRIDGE finger report */
	if (dev->quirks & QUIRK_BRIDGE && !is_pen_packet_id(tmp[0]))
		tmp[0] = 0x48;

	if (!need_skip_checksum(dev, report, tmp[0], true) &&
	    !is_checksum_matched(tmp[63], 0, 63, tmp, sizeof(tmp)))
		return -EILIPROTO;

	report->pen.algo = tmp[62];
	report->touch.algo = tmp[62];
	report->pen.dbg_size = 64;
	report->touch.dbg_size = 64;
	memcpy(report->pen.dbg, tmp, 64);
	memcpy(report->touch.dbg, tmp, 64);
	memcpy(buf, tmp, 64);

	/*
	 * no need to check contact count byte for debug packet and pen packet.
	 */
	if (is_pen_packet_id(tmp[0]) ||
	    is_debug_packet_id(tmp[0], dev->_interface)) {
		report_buf(report, tmp, true);
		return 0;
	}

	cnt = is_iwb_fmt(dev) ? tmp[55] : tmp[61];
	for (i = 1; i < DIV_ROUND_UP(cnt, max_cnt); i++) {
		report_buf(report, tmp, false);

		error = read_interrupt_in(dev, tmp, 64, 1000);
		if (error < 0)
			return error;

		if (dev->tp_info.format == 5)
			ilitek_decrypt(tmp + 1, 48);

		if (!need_skip_checksum(dev, report, tmp[0], false) &&
		    !is_checksum_matched(tmp[63], 0, 63, tmp, sizeof(tmp)))
			return -EILIPROTO;

		/* copy and skip the first rid byte */
		memcpy(buf + i * size * max_cnt + 1, tmp + 1, 63);
	}
	report_buf(report, tmp, true);

	return cnt;
}

int ilitek_report_update(void *handle, struct ilitek_report *report)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int cnt;

	if (!dev)
		return -EINVAL;

	/* initial report event */
	memset(&report->touch, 0, sizeof(report->touch));
	memset(&report->pen, 0, sizeof(report->pen));

	memset(dev->rbuf, 0, sizeof(dev->rbuf));

	switch (dev->protocol.flag) {
	default: return -EPERM;
	case PTL_V3:
		cnt = report_get_raw_v3(dev, report, dev->rbuf,
					 sizeof(dev->rbuf));
		if (cnt < 0)
			return cnt;

		break;

	case PTL_V6:
		cnt = report_get_raw_v6(dev, report, dev->rbuf,
					sizeof(dev->rbuf));
		if (cnt < 0)
			return cnt;

		/* pen packet (V6 only) */
		if (is_pen_packet_id(dev->rbuf[0])) {
			pen_decode(dev, report, dev->rbuf);
			return 0;
		}

		break;
	}

	/* debug message packet */
	if (is_debug_packet_id(dev->rbuf[0], dev->_interface)) {
		dmsg_decode(dev, report, dev->rbuf, sizeof(dev->rbuf));
		return 0;
	}

	touch_decode(dev, report, dev->rbuf, cnt);

	return 0;
}
