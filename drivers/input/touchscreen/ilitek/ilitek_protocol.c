// SPDX-License-Identifier: GPL-2.0
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#include "ilitek_protocol.h"

typedef int (*protocol_func_t)(struct ilitek_ts_device *, void *);

struct protocol_map {
	u8 cmd;
	u8 flag;
	protocol_func_t func;
	const char *desc;
};

static struct {
	unsigned int size;
	unsigned int max_cnt;
} touch_fmts[touch_fmt_max];

static struct {
	unsigned int size;
	unsigned int max_cnt;
} pen_fmts[pen_fmt_max];

#define X(_cmd, _protocol, _cmd_id, _api) \
	static int _api(struct ilitek_ts_device *, void *);
ILITEK_CMD_MAP
#undef X

#define X(_cmd, _protocol, _cmd_id, _api) {_cmd, _protocol, _api, #_cmd_id},
struct protocol_map protocol_maps[] = { ILITEK_CMD_MAP };
#undef X

u16 le16(const u8 *p)
{
	return p[0] | p[1] << 8;
}

u16 be16(const u8 *p)
{
	return p[1] | p[0] << 8;
}

u32 le32(const u8 *p, int bytes)
{
	u32 val = 0;

	while (bytes--)
		val += (p[bytes] << (8 * bytes));

	return val;
}

u32 be32(const u8 *p, int bytes)
{
	u32 val = 0;

	while (bytes--)
		val = (val << 8) | (*p++);

	return val;
}

static bool is_2501x(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!dev)
		return false;

	if (!strcmp(dev->mcu_info.ic_name, "25011") ||
	    !strcmp(dev->mcu_info.ic_name, "25012"))
		return true;

	return false;
}

bool is_29xx(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!dev)
		return false;

	if (!strcmp(dev->mcu_info.ic_name, "2900") ||
	    !strcmp(dev->mcu_info.ic_name, "2901") ||
	    !strcmp(dev->mcu_info.ic_name, "2910") ||
	    !strcmp(dev->mcu_info.ic_name, "2911") ||
	    !strcmp(dev->mcu_info.ic_name, "2531") ||
	    !strcmp(dev->mcu_info.ic_name, "2532") ||
	    !strcmp(dev->mcu_info.ic_name, "2921") ||
	    !strcmp(dev->mcu_info.ic_name, "2901M") ||
	    is_2501x(handle))
		return true;

	return false;
}

bool _is_231x(char *ic_name)
{
	if (!strcmp(ic_name, "2312") || !strcmp(ic_name, "2315"))
		return true;

	return false;
}

bool is_231x(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!dev)
		return false;

	return _is_231x(dev->mcu_info.ic_name);
}

bool has_hw_key(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!handle || !dev->tp_info.key_num)
		return false;

	if (dev->key.info.mode == key_hw ||
	    dev->key.info.mode == key_hsw)
		return true;

	return false;
}

u8 get_protocol_ver_flag(u32 ver)
{
	if (((ver >> 16) & 0xFF) == 0x3 ||
	     (ver & 0xFFFF00) == BL_PROTOCOL_V1_6 ||
	     (ver & 0xFFFF00) == BL_PROTOCOL_V1_7)
		return PTL_V3;

	if (((ver >> 16) & 0xFF) == 0x6 ||
	    (ver & 0xFFFF00) == BL_PROTOCOL_V1_8)
		return PTL_V6;

	return PTL_ANY;
}

void grid_reset(struct grids *grid)
{
	grid->mc.need_update = false;
	grid->sc_x.need_update = false;
	grid->sc_y.need_update = false;
	grid->pen_x.need_update = false;
	grid->pen_y.need_update = false;

	grid->key_mc.need_update = false;
	grid->key_x.need_update = false;
	grid->key_y.need_update = false;

	grid->self.need_update = false;

	if (grid->mc.data)
		_memset(grid->mc.data, 0,
		       grid->mc.X * grid->mc.Y * sizeof(s32));
	if (grid->sc_x.data)
		_memset(grid->sc_x.data, 0,
		grid->sc_x.X * grid->sc_x.Y * sizeof(s32));
	if (grid->sc_y.data)
		_memset(grid->sc_y.data, 0,
		grid->sc_y.X * grid->sc_y.Y * sizeof(s32));
	if (grid->pen_x.data)
		_memset(grid->pen_x.data, 0,
		grid->pen_x.X * grid->pen_x.Y * sizeof(s32));
	if (grid->pen_y.data)
		_memset(grid->pen_y.data, 0,
		grid->pen_y.X * grid->pen_y.Y * sizeof(s32));

	if (grid->key_mc.data)
		_memset(grid->key_mc.data, 0,
		grid->key_mc.X * grid->key_mc.Y * sizeof(s32));
	if (grid->key_x.data)
		_memset(grid->key_x.data, 0,
		grid->key_x.X * grid->key_x.Y * sizeof(s32));
	if (grid->key_y.data)
		_memset(grid->key_y.data, 0,
		grid->key_y.X * grid->key_y.Y * sizeof(s32));

	if (grid->self.data)
		_memset(grid->self.data, 0,
		grid->self.X * grid->self.Y * sizeof(s32));

	grid->dmsg.pen_need_update = false;
	grid->dmsg.touch_need_update = false;
	_memset(grid->dmsg.touch, 0, sizeof(grid->dmsg.touch));
	_memset(grid->dmsg.pen, 0, sizeof(grid->dmsg.pen));
}

void grid_free(struct grids *grid)
{
	if (grid->mc.data)
		CFREE(grid->mc.data);
	if (grid->sc_x.data)
		CFREE(grid->sc_x.data);
	if (grid->sc_y.data)
		CFREE(grid->sc_y.data);
	if (grid->pen_x.data)
		CFREE(grid->pen_x.data);
	if (grid->pen_y.data)
		CFREE(grid->pen_y.data);

	if (grid->key_mc.data)
		CFREE(grid->key_mc.data);
	if (grid->key_x.data)
		CFREE(grid->key_x.data);
	if (grid->key_y.data)
		CFREE(grid->key_y.data);

	if (grid->self.data)
		CFREE(grid->self.data);
}

int grid_alloc(void *handle, struct grids *grid)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int X, Y, key;

	_memset(grid, 0, sizeof(*grid));

	if (!dev)
		return -EINVAL;

	X = dev->tp_info.x_ch;
	Y = dev->tp_info.y_ch;
	key = dev->tp_info.key_num;

	grid->mc.data = (s32 *)CALLOC(X * Y, sizeof(s32));
	grid->sc_x.data = (s32 *)CALLOC(X, sizeof(s32));
	grid->sc_y.data = (s32 *)CALLOC(Y, sizeof(s32));
	grid->pen_x.data = (s32 *)CALLOC(X * 8, sizeof(s32));
	grid->pen_y.data = (s32 *)CALLOC(Y * 8, sizeof(s32));
	grid->key_mc.data = (s32 *)CALLOC(key, sizeof(s32));
	grid->key_x.data = (s32 *)CALLOC(key, sizeof(s32));
	grid->key_y.data = (s32 *)CALLOC(1, sizeof(s32));
	grid->self.data = (s32 *)CALLOC(4, sizeof(s32));
	if (!grid->mc.data || !grid->sc_x.data || !grid->sc_y.data ||
	    !grid->pen_x.data || !grid->pen_y.data || !grid->key_mc.data ||
	    !grid->key_x.data || !grid->key_y.data || !grid->self.data)
		goto err_free;

	grid->mc.X = X; grid->mc.Y = Y;
	grid->sc_x.X = X; grid->sc_x.Y = 1;
	grid->sc_y.X = 1; grid->sc_y.Y = Y;
	grid->pen_x.X = X; grid->pen_x.Y = 8;
	grid->pen_y.X = 8; grid->pen_y.Y = Y;

	grid->key_mc.X = key; grid->key_mc.Y = 1;
	grid->key_x.X = key; grid->key_x.Y = 1;
	grid->key_y.X = 1; grid->key_y.Y = 1;

	grid->self.X = 4, grid->self.Y = 1;

	grid_reset(grid);

	return 0;

err_free:
	grid_free(grid);

	return -ENOMEM;
}

static u16 update_crc(u16 crc, u8 newbyte)
{
	char i;
		const u16 crc_poly = 0x8408;

	crc ^= newbyte;

	for (i = 0; i < 8; i++) {
		if (crc & 0x01)
			crc = (crc >> 1) ^ crc_poly;
		else
			crc = crc >> 1;
	}

	return crc;
}

u16 get_crc(u32 start, u32 end,
	    u8 *buf, u32 buf_size)
{
	u16 crc = 0;
	u32 i;

	if (end > buf_size || start > buf_size) {
		TP_WARN(NULL, "start/end addr: 0x%x/0x%x buf size: 0x%x OOB\n",
			start, end, buf_size);
		return 0;
	}

	for (i = start; i < end && i < buf_size; i++)
		crc = update_crc(crc, buf[i]);

	return crc;
}

u32 get_checksum(u32 start, u32 end,
		 u8 *buf, u32 buf_size)
{
	u32 sum = 0;
	u32 i;

	if (end > buf_size || start > buf_size) {
		TP_WARN(NULL, "start/end addr: 0x%x/0x%x buf size: 0x%x OOB\n",
			start, end, buf_size);
		return 0;
	}

	for (i = start; i < end && i < buf_size; i++)
		sum += buf[i];

	return sum;
}

bool is_checksum_matched(u8 checksum, int start, int end,
			 u8 *buf, int buf_size)
{
	u8 check;

	check = ~(get_checksum(start, end, buf, buf_size)) + 1;
	if (check != checksum) {
		TP_ERR_ARR(NULL, "[data]", TYPE_U8, end - start, buf + start);
		TP_ERR(NULL, "checksum : 0x%02x/0x%02x not matched\n",
			check, checksum);
		return false;
	}

	return true;
}

bool support_mcu_info(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if ((dev->ic[0].mode == BL_MODE && dev->protocol.ver < 0x010803) ||
	    (dev->ic[0].mode == AP_MODE && dev->protocol.ver < 0x060009))
		return false;

	return true;
}

bool support_sensor_id(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if ((dev->ic[0].mode == BL_MODE && dev->protocol.ver < 0x010803) ||
		(dev->ic[0].mode == AP_MODE && dev->protocol.ver < 0x060004))
		return false;

	return true;
}

bool support_production_info(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if ((dev->ic[0].mode == BL_MODE && dev->protocol.ver < 0x010803) ||
	    (dev->ic[0].mode == AP_MODE && dev->protocol.ver < 0x060007))
		return false;

	return true;
}

bool support_fwid(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if ((dev->ic[0].mode == BL_MODE && dev->protocol.ver < 0x010802) ||
		(dev->ic[0].mode == AP_MODE && dev->protocol.ver < 0x060007))
		return false;

	return true;
}

bool support_power_status(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if ((dev->ic[0].mode == BL_MODE) ||
	    (dev->ic[0].mode == AP_MODE && dev->protocol.ver < 0x06000a))
		return false;

	return true;
}

int bridge_set_int_monitor(void *handle, bool enable)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 wbuf[64];

	_memset(wbuf, 0, sizeof(wbuf));
	wbuf[0] = 0x03;
	wbuf[1] = 0xf3;
	wbuf[2] = (enable) ? 0x01 : 0x00;

	return write_then_read_direct(dev, wbuf, 3, NULL, 0);
}

int bridge_set_test_mode(void *handle, bool enable)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 wbuf[64];

	_memset(wbuf, 0, sizeof(wbuf));
	wbuf[0] = 0x03;
	wbuf[1] = 0xf2;
	wbuf[2] = (enable) ? 0x01 : 0x00;

	return write_then_read_direct(dev, wbuf, 3, NULL, 0);
}

int reset_helper(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	bool need_re_enum = true;

	if (dev->_interface == interface_i2c) {
		/* sw reset if no reset-gpio found */
		if (!dev->cb.hw_reset ||
		    dev->cb.hw_reset(dev->reset_time, dev->_private) < 0)
			return api_protocol_set_cmd(dev, SET_SW_RST,
						    &need_re_enum);

		return 0;
	}

	return api_protocol_set_cmd(dev, SET_SW_RST, &need_re_enum);
}

static int re_enum_helper(struct ilitek_ts_device *dev, u8 enum_type)
{
	int error;
	int retry = 5;

	if (!dev->cb.re_enum)
		return -EINVAL;

	do {
		error = dev->cb.re_enum(enum_type, dev->_private);
		if (!error)
			return 0;

		TP_WARN(dev->id, "re-enum failed, error: %d, retry: %d\n", error, retry);
		dev->cb.delay_ms(500);
	} while (!dev->setting.no_retry && retry--);

	TP_ERR(dev->id, "re-enum retry failed\n");

	return -ENODEV;
}

int read_interrupt_in(void *handle, u8 *buf, int rlen,
		      unsigned int timeout_ms)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	if (!dev->cb.read_interrupt_in)
		return -EINVAL;

	error = dev->cb.read_interrupt_in(buf, rlen, timeout_ms,
					   dev->_private);
	if (error < 0)
		return error;

	TP_PKT_ARR(dev->id, "[int-in]:", TYPE_U8, rlen, buf);

	return 0;
}

int read_ctrl_in(void *handle, u8 cmd, u8 *buf, int rlen,
		 unsigned int timeout_ms)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	if (!dev->cb.read_ctrl_in)
		return -EINVAL;

	if (dev->quirks & QUIRK_BRIDGE) {
		_memset(dev->wbuf, 0, 64);

		dev->wbuf[0] = 0x03;
		dev->wbuf[1] = 0xA4;
		dev->wbuf[2] = 0;
		dev->wbuf[3] = 0;
		dev->wbuf[4] = (rlen + 6) & 0xFF;
		dev->wbuf[5] = ((rlen + 6) >> 8) & 0xFF;
		dev->wbuf[6] = cmd;

		error = dev->cb.write_then_read_direct(dev->wbuf, 64,
							dev->rbuf, 64, dev->_private);
		if (error < 0)
			return error;
	}

	error = dev->cb.read_ctrl_in(buf, rlen, timeout_ms, dev->_private);
	if (error < 0)
		return error;

	TP_PKT_ARR(dev->id, "[ctrl-in]:", TYPE_U8, rlen, buf);

	return 0;
}

int write_then_read(void *handle, u8 *cmd, int wlen,
		    u8 *buf, int rlen)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	if (!dev->cb.write_then_read)
		return -EINVAL;

	if (wlen > 0)
		TP_PKT_ARR(dev->id, "[wbuf]:", TYPE_U8, wlen, cmd);

	if (!wlen && (dev->quirks & QUIRK_WIFI_ITS_I2C ||
	    dev->quirks & QUIRK_BRIDGE)) {
		_memset(dev->wbuf, 0, 64);

		dev->wbuf[0] = 0x03;
		dev->wbuf[1] = 0xA3;
		dev->wbuf[2] = 0;
		dev->wbuf[3] = rlen;

		error = write_then_read_direct(dev, dev->wbuf, 64, NULL, 0);
		if (error < 0)
			return error;
	}

	error = dev->cb.write_then_read(cmd, wlen, buf, rlen, dev->_private);

	if (rlen > 0)
		TP_PKT_ARR(dev->id, "[rbuf]:", TYPE_U8, rlen, buf);

	return (error < 0) ? error : 0;
}

int write_then_read_direct(void *handle, u8 *cmd, int wlen,
			   u8 *buf, int rlen)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	if (!dev->cb.write_then_read_direct)
		return -EINVAL;

	if (wlen > 0)
		TP_PKT_ARR(dev->id, "[direct-wbuf]:", TYPE_U8, wlen, cmd);

	error = dev->cb.write_then_read_direct(cmd, wlen, buf, rlen,
					       dev->_private);

	if (rlen > 0)
		TP_PKT_ARR(dev->id, "[direct-rbuf]:", TYPE_U8, rlen, buf);

	return error;
}

int write_then_wait_ack(void *handle, u8 *cmd, int wlen, int timeout_ms)
{
	int error;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	struct ilitek_ts_callback *cb = &dev->cb;

	u8 ack_cmd;

	TP_DBG(dev->id, "cmd: 0x" PFMT_X8 ", tout_ms: %d\n",
		cmd[0], timeout_ms);

	if (dev->quirks & QUIRK_WAIT_ACK_DELAY) {
		error = write_then_read(dev, cmd, wlen, NULL, 0);
		if (error < 0)
			return error;

		cb->delay_ms(dev->setting.wait_ack_delay);
		return 0;
	}

	if (dev->setting.no_INT_ack) {
		/* prevent bridge int handling flow affecting the following read */
		if (dev->quirks & QUIRK_BRIDGE)
			bridge_set_int_monitor(dev, false);

		error = write_then_read(dev, cmd, wlen, NULL, 0);
		if (error < 0)
			return error;

		/*
		* for no-INT-ack flow, add delay to prevent
		* interrupting FW flow too soon, while FW should
		* be handling previous write command. ex. 0xcd/ 0xc3
		*/
		cb->delay_ms(5);

		goto check_busy;
	}

	if (!cb->init_ack || !cb->wait_ack)
		return -EINVAL;

	cb->init_ack(timeout_ms, dev->_private);
	if (dev->quirks & QUIRK_BRIDGE)
		bridge_set_int_monitor(dev, true);

	error = write_then_read(dev, cmd, wlen, NULL, 0);
	if (error < 0)
		return error;

	ack_cmd = (cmd[0] == CMD_ACCESS_SLAVE) ? cmd[2] : cmd[0];
	error = cb->wait_ack(ack_cmd, timeout_ms, dev->_private);

	if (dev->quirks & QUIRK_BRIDGE)
		bridge_set_int_monitor(dev, false);

	/* cmd[0] should be ILITEK cmd code */
	if (error < 0) {
		TP_WARN(dev->id, "wait 0x" PFMT_X8 " ack %d ms timeout, err: %d\n",
			cmd[0], timeout_ms, error);

		if (dev->setting.no_retry)
			return -EILITIME;

		goto check_busy;
	}

	return 0;

check_busy:
	return api_check_busy(dev, timeout_ms, 10);
}

/* Common APIs */
static int api_protocol_get_scrn_res(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct ilitek_screen_info *screen_info;

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 28);
	if (error < 0)
		return error;

	screen_info = (struct ilitek_screen_info *)dev->rbuf;

	dev->screen_info.x_min = screen_info->x_min;
	dev->screen_info.y_min = screen_info->y_min;
	dev->screen_info.x_max = screen_info->x_max;
	dev->screen_info.y_max = screen_info->y_max;

	TP_DBG(dev->id, "screen x: " PFMT_U16 "~" PFMT_U16 ", screen y: " PFMT_U16 "~" PFMT_U16 "\n",
		dev->screen_info.x_min, dev->screen_info.x_max,
		dev->screen_info.y_min, dev->screen_info.y_max);

	dev->screen_info.pressure_min = 0;
	dev->screen_info.pressure_max = 0;
	dev->screen_info.x_tilt_min = 0;
	dev->screen_info.x_tilt_max = 0;
	dev->screen_info.y_tilt_min = 0;
	dev->screen_info.y_tilt_max = 0;
	if (dev->protocol.ver > 0x60006) {
		dev->screen_info.pressure_min = screen_info->pressure_min;
		dev->screen_info.pressure_max = screen_info->pressure_max;
		dev->screen_info.x_tilt_min = screen_info->x_tilt_min;
		dev->screen_info.x_tilt_max = screen_info->x_tilt_max;
		dev->screen_info.y_tilt_min = screen_info->y_tilt_min;
		dev->screen_info.y_tilt_max = screen_info->y_tilt_max;

		dev->screen_info.pen_x_min = screen_info->pen_x_min;
		dev->screen_info.pen_y_min = screen_info->pen_y_min;
		dev->screen_info.pen_x_max = screen_info->pen_x_max;
		dev->screen_info.pen_y_max = screen_info->pen_y_max;
	}

	return 0;
}

static int api_protocol_get_tp_info_v3(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct ilitek_tp_info_v3 *tp_info;

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 15);
	if (error < 0)
		return error;

	tp_info = (struct ilitek_tp_info_v3 *)dev->rbuf;

	dev->tp_info.block_num = 2;
	dev->tp_info.x_resolution = tp_info->x_resolution;
	dev->tp_info.y_resolution = tp_info->y_resolution;
	dev->tp_info.x_ch = tp_info->x_ch;
	dev->tp_info.y_ch = tp_info->y_ch;
	dev->tp_info.max_fingers = tp_info->max_fingers;
	dev->tp_info.key_num = tp_info->key_num;

	dev->tp_info.support_modes = tp_info->support_modes;
	if (dev->tp_info.support_modes > 3 || !dev->tp_info.support_modes)
		dev->tp_info.support_modes = 1;

	TP_DBG(dev->id, "touch ch.(start/end) x: " PFMT_U8 "/" PFMT_U8 ", y: " PFMT_U8 "/" PFMT_U8 "\n",
		tp_info->touch_start_y, tp_info->touch_end_y,
		tp_info->touch_start_x, tp_info->touch_end_x);

	if (dev->tp_info.key_num) {
		/* check v3 key is virtual or hw keys */
		dev->key.info.mode =
			(tp_info->touch_start_y == 0xff &&
			 tp_info->touch_end_y == 0xff &&
			 tp_info->touch_start_x == 0xff &&
			 tp_info->touch_end_x == 0xff) ?
			key_hw : key_vitual;
	}

	return 0;
}

static int api_protocol_get_tp_info_v6(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct ilitek_tp_info_v6 *tp_info;
	u8 i;

#define X(_enum, _code, _name) {_code, _name},
	const struct {
		const int code;
		const char *str;
	} pen_modes[] = { STYLUS_MODES };
#undef X

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 21);
	if (error < 0)
		return error;

	tp_info = (struct ilitek_tp_info_v6 *)dev->rbuf;
	dev->tp_info.x_resolution = tp_info->x_resolution;
	dev->tp_info.y_resolution = tp_info->y_resolution;
	dev->tp_info.x_ch = tp_info->x_ch;
	dev->tp_info.y_ch = tp_info->y_ch;
	dev->tp_info.max_fingers = tp_info->max_fingers;
	dev->tp_info.key_num = tp_info->key_num;
	dev->tp_info.ic_num = tp_info->ic_num;
	dev->tp_info.format = tp_info->format;
	dev->tp_info.support_modes = tp_info->support_modes;
	dev->tp_info.die_num = tp_info->die_num;

	if (dev->tp_info.format == 5)
		api_protocol_set_cmd(dev, GET_CRYPTO_INFO, NULL);

	if (dev->tp_info.ic_num > ARRAY_SIZE(dev->ic)) {
		TP_ERR(dev->id, "invalid ic_num: " PFMT_U8 "\n", dev->tp_info.ic_num);
		return -EINVAL;
	}
	TP_MSG(dev->id, "[Panel Information] Chip count: %u\n",
		dev->tp_info.ic_num);

	if (dev->tp_info.max_fingers > 40) {
		TP_ERR(dev->id, "invalid max tp: %d > 40\n",
			dev->tp_info.max_fingers);
		return -EINVAL;
	}

	if (dev->protocol.ver < 0x60003)
		return 0;

	dev->tp_info.block_num = tp_info->block_num;
	TP_MSG(dev->id, "[Panel Information] Block Number: " PFMT_U8 "\n",
		dev->tp_info.block_num);

	if (dev->protocol.ver < 0x60007)
		return 0;

	dev->tp_info.pen_modes = tp_info->pen_modes;

	_memset(dev->pen_mode, 0, sizeof(dev->pen_mode));
	if (!dev->tp_info.pen_modes)
		_strcpy(dev->pen_mode, "Disable",
			sizeof(dev->pen_mode));
	for (i = 0; i < ARRAY_SIZE(pen_modes); i++) {
		if (!(tp_info->pen_modes & pen_modes[i].code))
			continue;

		if (_strlen(dev->pen_mode))
			_strcat(dev->pen_mode, ",", sizeof(dev->pen_mode));

		_strcat(dev->pen_mode, pen_modes[i].str,
			sizeof(dev->pen_mode));
	}

	TP_DBG(dev->id, "pen_modes: " PFMT_U8 "\n", dev->tp_info.pen_modes);
	TP_MSG(dev->id, "[Panel Information] Pen Mode: " PFMT_C8 "\n",
		dev->pen_mode);

	dev->tp_info.pen_format = tp_info->pen_format;
	dev->tp_info.pen_x_resolution = tp_info->pen_x_resolution;
	dev->tp_info.pen_y_resolution = tp_info->pen_y_resolution;
	TP_MSG(dev->id, "[Panel Information] Pen Format: 0x" PFMT_X8 "\n",
		dev->tp_info.pen_format);
	TP_MSG(dev->id, "[Panel Information] Pen X/Y resolution: " PFMT_U16 "/" PFMT_U16 "\n",
		dev->tp_info.pen_x_resolution,
		dev->tp_info.pen_y_resolution);

	return 0;
}

static int api_protocol_get_tp_info(struct ilitek_ts_device *dev, void *data)
{
	int error;

#define X(_enum, _id, _size, _cnt)	\
	touch_fmts[_id].size = _size;	\
	touch_fmts[_id].max_cnt = _cnt;

	ILITEK_TOUCH_REPORT_FORMAT;
#undef X

#define X(_enum, _id, _size, _cnt)	\
	pen_fmts[_id].size = _size;	\
	pen_fmts[_id].max_cnt = _cnt;

	ILITEK_PEN_REPORT_FORMAT;
#undef X

	if (dev->protocol.flag == PTL_V3)
		error = api_protocol_get_tp_info_v3(dev, data);
	else if (dev->protocol.flag == PTL_V6)
		error = api_protocol_get_tp_info_v6(dev, data);
	else
		return -EINVAL;

	if (error < 0)
		return error;

	if (dev->tp_info.max_fingers > 40) {
		TP_ERR(dev->id, "invalid max fingers: %d > 40\n",
			dev->tp_info.max_fingers);
		return -EINVAL;
	}

	switch (dev->tp_info.format) {
	case touch_fmt_0x1:
	case touch_fmt_0x2:
	case touch_fmt_0x3:
	case touch_fmt_0x4:
	case touch_fmt_0x10:
		if (dev->setting.default_format_enabled)
			goto default_fmt_enabled;

		dev->fmt.touch_size = touch_fmts[dev->tp_info.format].size;
		dev->fmt.touch_max_cnt = touch_fmts[dev->tp_info.format].max_cnt;
		break;

default_fmt_enabled:
	default:
	case touch_fmt_0x11:
	case touch_fmt_0x0:
		dev->fmt.touch_size = touch_fmts[touch_fmt_0x0].size;
		dev->fmt.touch_max_cnt = touch_fmts[touch_fmt_0x0].max_cnt;
		break;
	}

	switch (dev->tp_info.pen_format) {
	case pen_fmt_0x1:
	case pen_fmt_0x2:
		dev->fmt.pen_size = pen_fmts[dev->tp_info.pen_format].size;
		dev->fmt.pen_max_cnt = pen_fmts[dev->tp_info.pen_format].max_cnt;
		break;
	default:
	case pen_fmt_0x0:
		dev->fmt.pen_size = pen_fmts[pen_fmt_0x0].size;
		dev->fmt.pen_max_cnt = pen_fmts[pen_fmt_0x0].max_cnt;
		break;
	}

	TP_MSG(dev->id, "[Panel Information] X/Y resolution: " PFMT_U16 "/" PFMT_U16 "\n",
		dev->tp_info.x_resolution, dev->tp_info.y_resolution);
	TP_MSG(dev->id, "[Panel Information] X/Y channel: " PFMT_U16 "/" PFMT_U16 "\n",
		dev->tp_info.x_ch, dev->tp_info.y_ch);
	TP_MSG(dev->id, "[Panel Information] Support " PFMT_U8 " Fingers\n",
		dev->tp_info.max_fingers);
	TP_MSG(dev->id, "[Panel Information] Support " PFMT_U8 " Keys\n",
		dev->tp_info.key_num);

	TP_MSG(dev->id, "[Panel Information] Support " PFMT_U8 " modes\n",
		dev->tp_info.support_modes);

	TP_DBG(dev->id, "touch format: 0x" PFMT_X8 ", size: %u bytes, max cnt: %u per packet\n",
		dev->tp_info.format, dev->fmt.touch_size,
		dev->fmt.touch_max_cnt);

	if (dev->tp_info.key_num > 0) {
		error = api_protocol_set_cmd(dev, GET_KEY_INFO, NULL);
		if (error < 0)
			return error;
	}

	return 0;
}

static int api_protocol_get_key_info_v3(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	struct ilitek_key_info_v3 *key_info;
	unsigned int i;

	UNUSED(data);

	/* Only i2c interface has key for V3 */
	if (dev->_interface != interface_i2c)
		return 0;

	if (dev->tp_info.key_num > 20) {
		TP_ERR(dev->id, "key count: " PFMT_U8 " invalid\n", dev->tp_info.key_num);
		return -EINVAL;
	}

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 29);
	if (error < 0)
		return error;

	for (i = 0; dev->tp_info.key_num > 5U &&
	     i < DIV_ROUND_UP(dev->tp_info.key_num, 5U) - 1U; i++) {
		TP_MSG(dev->id, "read keyinfo again, i: %u\n", i);
		error = write_then_read(dev, NULL, 0,
					dev->rbuf + 29 + 5 * i, 25);
		if (error < 0)
			return error;
	}

	key_info = (struct ilitek_key_info_v3 *)dev->rbuf;
	dev->key.info.x_len = be16(key_info->x_len);
	dev->key.info.y_len = be16(key_info->y_len);
	TP_MSG(dev->id, "key_x_len: " PFMT_U16 ", key_y_len: " PFMT_U16 "\n",
		dev->key.info.x_len, dev->key.info.y_len);

	for (i = 0; i < dev->tp_info.key_num; i++) {
		dev->key.info.keys[i].id = key_info->keys[i].id;
		dev->key.info.keys[i].x = be16(key_info->keys[i].x);
		dev->key.info.keys[i].y = be16(key_info->keys[i].y);
		TP_MSG(dev->id, "key[%u] id: " PFMT_U8 ", x: " PFMT_U16 ", y: " PFMT_U16 "\n", i,
			dev->key.info.keys[i].id, dev->key.info.keys[i].x,
			dev->key.info.keys[i].y);
	}

	return 0;
}

static int api_protocol_get_key_info_v6(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	struct ilitek_key_info_v6 *key_info;
	unsigned int i, offset;

	UNUSED(data);

	if (dev->tp_info.key_num > ARRAY_SIZE(dev->key.info.keys)) {
		TP_ERR(dev->id, "exception keycount " PFMT_U8 " > %d\n", dev->tp_info.key_num,
			(int)ARRAY_SIZE(dev->key.info.keys));
		return -EINVAL;
	}

	switch (dev->_interface) {
	case interface_i2c:
		if (dev->quirks & QUIRK_WIFI_ITS_I2C ||
		    dev->quirks & QUIRK_BRIDGE) {
			error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
			if (error < 0)
				return error;
			error = read_ctrl_in(dev, CMD_GET_KEY_INFO,
					     dev->rbuf,
					     5 + dev->tp_info.key_num * 5, 2000);
			if (error < 0)
				return error;
			offset = (dev->quirks & QUIRK_BRIDGE) ? 1 : 0;
		} else {
			error = write_then_read(dev, dev->wbuf, 1,
						dev->rbuf, 5 + dev->tp_info.key_num * 5);
			if (error < 0)
				return error;
			offset = 0;
		}
		break;

	case interface_usb:
		error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
		if (error < 0)
			return error;
		error = write_then_read(dev, NULL, 0, dev->rbuf, 256);
		if (error < 0)
			return error;
		offset = 6;
		break;
	case interface_hid_over_i2c:
		error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
		if (error < 0)
			return error;
		error = write_then_read(dev, NULL, 0, dev->rbuf, 256);
		if (error < 0)
			return error;
		offset = 4;
		break;
	default: return -EINVAL;
	};

	key_info = (struct ilitek_key_info_v6 *)(dev->rbuf + offset);
	dev->key.info.mode = key_info->mode;
	TP_MSG(dev->id, "[Panel Information] key mode: " PFMT_U8 "\n", dev->key.info.mode);

	dev->key.info.x_len = key_info->x_len;
	dev->key.info.y_len = key_info->y_len;
	TP_MSG(dev->id, "key_x_len: " PFMT_U16 ", key_y_len: " PFMT_U16 "\n",
		dev->key.info.x_len, dev->key.info.y_len);

	for (i = 0; i < dev->tp_info.key_num; i++) {
		dev->key.info.keys[i].id = key_info->keys[i].id;
		dev->key.info.keys[i].x = key_info->keys[i].x;
		dev->key.info.keys[i].y = key_info->keys[i].y;
		TP_MSG(dev->id, "key[%u] id: " PFMT_U8 ", x: " PFMT_U16 ", y: " PFMT_U16 "\n", i,
			dev->key.info.keys[i].id, dev->key.info.keys[i].x,
			dev->key.info.keys[i].y);
	}

	return 0;
}

static int api_protocol_get_key_info(struct ilitek_ts_device *dev, void *data)
{
	if (dev->protocol.flag == PTL_V3)
		return api_protocol_get_key_info_v3(dev, data);
	else if (dev->protocol.flag == PTL_V6)
		return api_protocol_get_key_info_v6(dev, data);

	return -EINVAL;
}

static int api_protocol_get_ptl_ver(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	dev->protocol.flag = PTL_V6;
	dev->reset_time = 1000;
	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 3);
	if (error < 0)
		return error;

	dev->protocol.ver = (dev->rbuf[0] << 16) + (dev->rbuf[1] << 8) +
			     dev->rbuf[2];
	TP_MSG(dev->id, "[Protocol Version]: %x.%x.%x\n",
		(dev->protocol.ver >> 16) & 0xFF,
		(dev->protocol.ver >> 8) & 0xFF,
		dev->protocol.ver & 0xFF);

	dev->protocol.flag = get_protocol_ver_flag(dev->protocol.ver);
	switch (dev->protocol.flag) {
	case PTL_V3:
		dev->reset_time = 200;
		break;
	case PTL_V6:
		dev->reset_time = 600;
		break;
	default:
		TP_ERR(dev->id, "unrecognized protocol ver.: 0x%x\n",
			dev->protocol.ver);
		return -EINVAL;
	}

	return 0;
}

static int api_protocol_get_fw_ver(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 8);
	if (error < 0)
		return error;

	_memcpy(dev->fw_ver, dev->rbuf, 8);

	if (dev->ic[0].mode == BL_MODE) {
		TP_MSG_ARR(dev->id, "[BL Firmware Version]", TYPE_U8,
			   8, dev->fw_ver);
	} else {
		TP_MSG_ARR(dev->id, "[FW Version]", TYPE_U8, 4, dev->fw_ver);
		TP_MSG_ARR(dev->id, "[Customer Version]", TYPE_U8,
			   4, dev->fw_ver + 4);
	}

	return 0;
}

static int api_protocol_get_mcu_mode(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u8 i, ic_num = (data) ? *(u8 *)data : 1;

	if (ic_num > ARRAY_SIZE(dev->ic))
		return -EINVAL;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 2 * ic_num);
	if (error < 0)
		return error;

	for (i = 0; i < ic_num; i++) {
		dev->ic[i].mode = dev->rbuf[i * 2];

		if (dev->ic[i].mode == AP_MODE)
			_sprintf(dev->ic[i].mode_str, 0, "AP");
		else if (dev->ic[i].mode == BL_MODE)
			_sprintf(dev->ic[i].mode_str, 0, "BL");
		else
			_sprintf(dev->ic[i].mode_str, 0, "UNKNOWN");
	}

	TP_MSG(dev->id, "[Current Mode] Master: 0x" PFMT_X8 " " PFMT_C8 "\n",
		dev->ic[0].mode, dev->ic[0].mode_str);
	for (i = 1; i < ic_num; i++)
		TP_MSG(dev->id, "[Current Mode] Slave[" PFMT_U8 "]: 0x" PFMT_X8 " " PFMT_C8 "\n",
			i, dev->ic[i].mode, dev->ic[i].mode_str);

	return 0;
}

static int api_protocol_power_status(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u8 func, lvd_level_sel;

	if (!data)
		return -EFAULT;

	if (!support_power_status(dev)) {
		_memset(&dev->pwr, 0, sizeof(dev->pwr));
		return 0;
	}

	func = ((*(u16 *)data) >> 8) & 0xff;
	lvd_level_sel = (*(u16 *)data) & 0xff;

	dev->wbuf[1] = func;
	switch (func) {
	/* get level select */
	case 0x02:
		dev->wbuf[2] = lvd_level_sel;
		return write_then_read(dev, dev->wbuf, 3, NULL, 0);

	/* clear flag */
	case 0x00:
		return write_then_read(dev, dev->wbuf, 2, NULL, 0);

	/* get flag */
	case 0x01:
		error = write_then_read(dev, dev->wbuf, 2,
					dev->rbuf, 4);
		if (error < 0)
			return error;
		break;

	default:
		return -EINVAL;
	}

	dev->pwr.header = be16(dev->rbuf);
	dev->pwr.vdd33_lvd_flag = dev->rbuf[2];
	dev->pwr.vdd33_lvd_level_sel = dev->rbuf[3];

	TP_DBG(dev->id, "[Power-Status] header: 0x" PFMT_X16 ", flag: 0x" PFMT_X8 ", level_sel: 0x" PFMT_X8 "\n",
		dev->pwr.header, dev->pwr.vdd33_lvd_flag,
		dev->pwr.vdd33_lvd_level_sel);

	return 0;
}

static int api_protocol_get_sensor_id(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	/* return 0 to skip error check */
	if (!support_sensor_id(dev))
		return 0;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 3);
	if (error < 0)
		return error;

	dev->sensor.header = be16(dev->rbuf);
	dev->sensor.id = dev->rbuf[2];

	TP_MSG(dev->id, "[Sensor ID] header: 0x" PFMT_X16 ", id: 0x" PFMT_X8 "\n",
		dev->sensor.header,
		(u8)(dev->sensor.id & dev->setting.sensor_id_mask));

	return 0;
}

static int api_protocol_get_product_info(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	/* return 0 to skip error check */
	if (!support_production_info(dev))
		return 0;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 8);
	if (error < 0)
		return error;

	_memcpy(dev->product_info, dev->rbuf, 8);

	TP_MSG_ARR(dev->id, "[Production Info]", TYPE_U8, 8, dev->product_info);

	return 0;
}

static int api_protocol_get_fwid(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	/* return 0 to skip error check */
	if (!support_fwid(dev))
		return 0;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 4);
	if (error < 0)
		return error;

	dev->customer_id = le16(dev->rbuf);
	dev->fwid = le16(dev->rbuf + 2);

	TP_MSG(dev->id, "[Customer ID] 0x%04x\n", dev->customer_id);
	TP_MSG(dev->id, "[FWID] 0x%04x\n", dev->fwid);

	return 0;
}

static int api_protocol_get_crypto_info(struct ilitek_ts_device *dev,
					void *data)
{
	u16 __MAYBE_UNUSED crypto_ver;
	u32 crypto_opt;

	UNUSED(data);

	/*
	 * encrypted report format should be supported after AP v6.0.8
	 * set report format to 0 if protocol version not matched or
	 * crypto info say it's not supported.
	 */
	if (dev->protocol.ver < 0x060008 ||
	    write_then_read(dev, dev->wbuf, 1, dev->rbuf, 6) < 0) {
		dev->tp_info.format = 0;
		return 0;
	}

	crypto_ver = le16(dev->rbuf);
	crypto_opt = le32(dev->rbuf + 2, 4);

	TP_MSG(dev->id, "[Encrypt Ver.] 0x%x\n", crypto_ver);
	TP_MSG(dev->id, "[Encrypt Options] 0x%x\n", crypto_opt);

	if (!(crypto_opt & 1))
		dev->tp_info.format = 0;

	return 0;
}

static int api_protocol_get_hid_info(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	if (dev->protocol.ver < 0x060009)
		return 0;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 6);
	if (error < 0)
		return error;

	_memcpy(&dev->hid_info, dev->rbuf, sizeof(dev->hid_info));

	TP_MSG(dev->id, "vid/pid/rev: 0x%x/0x%x/0x%x\n",
		dev->hid_info.vid, dev->hid_info.pid, dev->hid_info.rev);

	return 0;
}

static bool is_special_char(char c)
{
	return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9')) ? false : true;
}

static int api_protocol_get_mcu_ver(struct ilitek_ts_device *dev, void *data)
{
	int error;
	unsigned int i;

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif
	struct __PACKED__ mcu_ver {
		u16 ic_name;
		u8 df_start_addr[3];
		u8 df_size;

		char module_name[26];
	} *parser;

#ifdef _WIN32
#pragma pack()
#endif

	UNUSED(data);

	/*
	 * GET_MCU_INFO (0x62) cmd support V6 and BL > v1.8.2 and AP > v6.0.7
	 * otherwise, use GET_MCU_VER (0x61) cmd
	 */
	if (dev->protocol.flag == PTL_V6 && support_mcu_info(dev)) {
		error = api_protocol_set_cmd(dev, GET_MCU_INFO, NULL);
		if (error < 0)
			return error;
	} else {
		error = write_then_read(dev, dev->wbuf, 1,
					dev->rbuf, 32);
		if (error < 0)
			return error;

		parser = (struct mcu_ver *)dev->rbuf;

		_memset(dev->mcu_info.ic_name, 0,
			sizeof(dev->mcu_info.ic_name));
		_sprintf(dev->mcu_info.ic_name, 0, "%04x", parser->ic_name);

		_memset(dev->mcu_info.module_name, 0,
			sizeof(dev->mcu_info.module_name));
		_memcpy(dev->mcu_info.module_name, parser->module_name,
			sizeof(parser->module_name));
	}

	if (dev->protocol.flag == PTL_V6) {
		if (is_29xx(dev)) {
			/* modify reset time to 100ms for 29xx ICs */
			dev->reset_time = 100;

			/* set mm_addr for bin file update */
			dev->mcu_info.mm_addr =
				is_2501x(dev) ? MM_ADDR_2501X : MM_ADDR_29XX;
			dev->mcu_info.min_addr = START_ADDR_29XX;
			dev->mcu_info.max_addr = END_ADDR_LEGO;
		} else {
			dev->mcu_info.mm_addr = MM_ADDR_LEGO;
			dev->mcu_info.min_addr = START_ADDR_LEGO;
			dev->mcu_info.max_addr = END_ADDR_LEGO;
		}
	}

	for (i = 0; i < sizeof(dev->mcu_info.module_name); i++) {
		if (is_special_char(dev->mcu_info.module_name[i]))
			dev->mcu_info.module_name[i] = 0;
	}
	if (!strcmp(dev->mcu_info.ic_name, "2133"))
		_sprintf(dev->mcu_info.ic_name, 0, "2132S");

	_memset(dev->mcu_info.ic_full_name, 0,
		sizeof(dev->mcu_info.ic_full_name));
	_sprintf(dev->mcu_info.ic_full_name, 0,
		"ILI" PFMT_C8, dev->mcu_info.ic_name);

	TP_MSG(dev->id, "[MCU Kernel Version] " PFMT_C8 "\n",
		dev->mcu_info.ic_full_name);
	TP_MSG(dev->id, "[Module Name]: [" PFMT_C8 "]\n",
		dev->mcu_info.module_name);

	return 0;
}

static int api_protocol_get_mcu_info(struct ilitek_ts_device *dev, void *data)
{
	int error;
	unsigned int i;

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif
	struct __PACKED__ mcu_info {
		char ic_name[5];
		char mask_ver[2];
		u8 mm_addr[3];
		char module_name[18];
		u8 reserve[4];
	} *parser;

#ifdef _WIN32
#pragma pack()
#endif

	UNUSED(data);

	/*
	 * GET_MCU_INFO (0x62) cmd only support V6 and BL > v1.8.2 and AP > v6.0.7
	 * otherwise, return 0 to skip this command.
	 */
	if (dev->protocol.flag != PTL_V6 || !support_mcu_info(dev))
		return 0;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 32);
	if (error < 0)
		return error;

	parser = (struct mcu_info *)dev->rbuf;

	_memset(dev->mcu_info.ic_name, 0, sizeof(dev->mcu_info.ic_name));
	_memcpy(dev->mcu_info.ic_name, parser->ic_name,
		sizeof(parser->ic_name));

	_memcpy(dev->mcu_info.module_name, parser->module_name,
		sizeof(parser->module_name));
	dev->mcu_info.mm_addr = le32(parser->mm_addr, 3);

	for (i = 0; i < sizeof(dev->mcu_info.module_name); i++) {
		if (is_special_char(dev->mcu_info.module_name[i]))
			dev->mcu_info.module_name[i] = 0;
	}

	return 0;
}

static int api_protocol_set_fs_info(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct freq_settings *freq = (struct freq_settings *)data;

	if (!data)
		return -EINVAL;

	dev->wbuf[1] = freq->sine.start & 0xFF;
	dev->wbuf[2] = freq->sine.start >> 8;
	dev->wbuf[3] = freq->sine.end & 0xFF;
	dev->wbuf[4] = freq->sine.end >> 8;
	dev->wbuf[5] = freq->sine.step;
	dev->wbuf[6] = freq->mc_swcap.start & 0xFF;
	dev->wbuf[7] = freq->mc_swcap.start >> 8;
	dev->wbuf[8] = freq->mc_swcap.end & 0xFF;
	dev->wbuf[9] = freq->mc_swcap.end >> 8;
	dev->wbuf[10] = freq->mc_swcap.step;
	dev->wbuf[11] = freq->sc_swcap.start & 0xFF;
	dev->wbuf[12] = freq->sc_swcap.start >> 8;
	dev->wbuf[13] = freq->sc_swcap.end & 0xFF;
	dev->wbuf[14] = freq->sc_swcap.end >> 8;
	dev->wbuf[15] = freq->sc_swcap.step;
	dev->wbuf[16] = freq->frame_cnt & 0xFF;
	dev->wbuf[17] = freq->frame_cnt >> 8;
	dev->wbuf[18] = freq->scan_type;

	if (dev->protocol.ver < 0x60005)
		return write_then_read(dev, dev->wbuf, 19, NULL, 0);

	do {
		if (dev->protocol.ver < 0x60009) {
			error = write_then_read(dev, dev->wbuf, 19,
						dev->rbuf, 5);
			break;
		}

		dev->wbuf[16] = freq->dump1.start & 0xFF;
		dev->wbuf[17] = freq->dump1.start >> 8;
		dev->wbuf[18] = freq->dump1.end & 0xFF;
		dev->wbuf[19] = freq->dump1.end >> 8;
		dev->wbuf[20] = freq->dump1.step;
		dev->wbuf[21] = freq->dump1_val;
		dev->wbuf[22] = freq->dump2.start & 0xFF;
		dev->wbuf[23] = freq->dump2.start >> 8;
		dev->wbuf[24] = freq->dump2.end & 0xFF;
		dev->wbuf[25] = freq->dump2.end >> 8;
		dev->wbuf[26] = freq->dump2.step;
		dev->wbuf[27] = freq->dump2_val;
		dev->wbuf[28] = freq->frame_cnt & 0xFF;
		dev->wbuf[29] = freq->frame_cnt >> 8;
		dev->wbuf[30] = freq->scan_type;

		if (dev->protocol.ver < 0x6000a) {
			error = write_then_read(dev, dev->wbuf, 31,
						dev->rbuf, 5);
			break;
		}

		dev->wbuf[31] = freq->mc_frame_cnt & 0xFF;
		dev->wbuf[32] = freq->mc_frame_cnt >> 8;
		dev->wbuf[33] = freq->dump_frame_cnt & 0xFF;
		dev->wbuf[34] = freq->dump_frame_cnt >> 8;

		error = write_then_read(dev, dev->wbuf, 35, dev->rbuf, 5);

	} while (0);

	if (error < 0)
		return error;

	freq->packet_steps = le16(dev->rbuf + 3);

	if (dev->rbuf[0] != 0x5a || dev->rbuf[1] != 0xa5 || dev->rbuf[2]) {
		TP_ERR(dev->id, "invalid header: 0x" PFMT_X8 "-0x" PFMT_X8 "-0x" PFMT_X8 ", total steps: " PFMT_U16 "\n",
			dev->rbuf[0], dev->rbuf[1], dev->rbuf[2],
			freq->packet_steps);
		return -EFAULT;
	}

	return 0;
}

static int api_protocol_set_short_info(struct ilitek_ts_device *dev, void *data)
{
	struct short_settings *_short = (struct short_settings *)data;

	if (!data)
		return -EINVAL;

	TP_DBG(dev->id, "[short info] dump1: 0x" PFMT_X8 ", dump2: 0x" PFMT_X8 ", vref: 0x" PFMT_X8 ", postidle: 0x" PFMT_X16 "\n",
		_short->dump_1, _short->dump_2,
		_short->v_ref_L, _short->post_idle);

	dev->wbuf[1] = _short->dump_1;
	dev->wbuf[2] = _short->dump_2;
	dev->wbuf[3] = _short->v_ref_L;
	dev->wbuf[4] = _short->post_idle & 0xFF;
	dev->wbuf[5] = (_short->post_idle >> 8) & 0xFF;

	return write_then_read(dev, dev->wbuf, 6, NULL, 0);
}

static int api_protocol_set_open_info(struct ilitek_ts_device *dev, void *data)
{
	struct open_settings *open = (struct open_settings *)data;
	int wlen = 1;

	if (!data)
		return -EINVAL;

	TP_DBG(dev->id,
		"[open info] freq.: " PFMT_U16 ", gain: 0x" PFMT_X8 ", gain_rfb: 0x" PFMT_X8
		", afe_res_sel: 0x" PFMT_X8 ", mc_fsel: 0x" PFMT_X8 "\n",
		open->freq, open->gain, open->gain_rfb,
		open->afe_res_sel, open->mc_fsel);

	dev->wbuf[wlen++] = open->freq & 0xFF;
	dev->wbuf[wlen++] = (open->freq >> 8) & 0xFF;
	dev->wbuf[wlen++] = open->gain;
	dev->wbuf[wlen++] = open->gain_rfb;
	dev->wbuf[wlen++] = open->afe_res_sel;
	dev->wbuf[wlen++] = open->mc_fsel;

	if (dev->protocol.ver > 0x060009) {
		TP_DBG(dev->id, "[open info] frame: " PFMT_U16 "\n",
			open->frame);

		dev->wbuf[wlen++] = open->frame & 0xFF;
		dev->wbuf[wlen++] = (open->frame >> 8) & 0xFF;
	}

	return write_then_read(dev, dev->wbuf, wlen, NULL, 0);
}

static int api_protocol_set_charge_info(struct ilitek_ts_device *dev,
					void *data)
{
	int error, i;
	struct charge_curve_settings *curve =
		(struct charge_curve_settings *)data;

	if (!data)
		return -EINVAL;

	TP_DBG(dev->id, "charge-curve info. scan mode: 0x" PFMT_U8 "\n",
		curve->scan_mode);

	dev->wbuf[1] = curve->scan_mode;
	dev->wbuf[2] = curve->dump.start & 0xFF;
	dev->wbuf[3] = (curve->dump.start >> 8) & 0xFF;
	dev->wbuf[4] = curve->dump.end & 0xFF;
	dev->wbuf[5] = (curve->dump.end >> 8) & 0xFF;
	dev->wbuf[6] = curve->dump.step;
	dev->wbuf[7] = curve->dump.post_idle & 0xFF;
	dev->wbuf[8] = (curve->dump.post_idle >> 8) & 0xFF;
	dev->wbuf[9] = curve->dump.fix_val & 0xFF;
	dev->wbuf[10] = (curve->dump.fix_val >> 8) & 0xFF;
	dev->wbuf[11] = curve->charge.start & 0xFF;
	dev->wbuf[12] = (curve->charge.start >> 8) & 0xFF;
	dev->wbuf[13] = curve->charge.end & 0xFF;
	dev->wbuf[14] = (curve->charge.end >> 8) & 0xFF;
	dev->wbuf[15] = curve->charge.step;
	dev->wbuf[16] = curve->charge.post_idle & 0xFF;
	dev->wbuf[17] = (curve->charge.post_idle >> 8) & 0xFF;
	dev->wbuf[18] = curve->charge.fix_val & 0xFF;
	dev->wbuf[19] = (curve->charge.fix_val >> 8) & 0xFF;
	dev->wbuf[20] = curve->c_sub & 0xFF;
	dev->wbuf[21] = (curve->c_sub >> 8) & 0xFF;
	dev->wbuf[22] = curve->frame_cnt & 0xFF;
	dev->wbuf[23] = (curve->frame_cnt >> 8) & 0xFF;

	for (i = 0; i < (int)ARRAY_SIZE(curve->pt); i++) {
		dev->wbuf[24 + i * 4] = curve->pt[i].x & 0xFF;
		dev->wbuf[24 + i * 4 + 1] = (curve->pt[i].x >> 8) & 0xFF;
		dev->wbuf[24 + i * 4 + 2] = curve->pt[i].y & 0xFF;
		dev->wbuf[24 + i * 4 + 3] = (curve->pt[i].y >> 8) & 0xFF;
	}

	error = write_then_read(dev, dev->wbuf, 60, dev->rbuf, 5);
	if (error < 0)
		return error;

	curve->packet_steps = le16(dev->rbuf + 3);

	if (dev->rbuf[0] != AP_MODE || dev->rbuf[1] != 0xa5 || dev->rbuf[2]) {
		TP_ERR(dev->id, "invalid header: 0x" PFMT_X8 "-0x" PFMT_X8 "-0x" PFMT_X8 ", total steps: " PFMT_U16 "\n",
			dev->rbuf[0], dev->rbuf[1], dev->rbuf[2],
			curve->packet_steps);
		return -EFAULT;
	}

	return 0;
}

static int api_protocol_set_p2p_info(struct ilitek_ts_device *dev, void *data)
{
	struct p2p_settings *p2p = (struct p2p_settings *)data;
	int wlen = 1;

	if (!data)
		return -EINVAL;

	TP_DBG(dev->id, "[p2p info] frame_cnt.: " PFMT_U16 ", type: 0x" PFMT_X8 "\n",
		p2p->frame_cnt, p2p->type);

	dev->wbuf[wlen++] = p2p->frame_cnt & 0xFF;
	dev->wbuf[wlen++] = (p2p->frame_cnt >> 8) & 0xFF;

	if (dev->protocol.ver > 0x060009) {
		dev->wbuf[wlen++] = p2p->type & 0xFF;
		dev->wbuf[wlen++] = p2p->freq & 0xFF;
		dev->wbuf[wlen++] = (p2p->freq >> 8) & 0xFF;
	}

	return write_then_read(dev, dev->wbuf, wlen, NULL, 0);
}

static int api_protocol_set_pen_fs_info(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	struct freq_settings *freq = (struct freq_settings *)data;

	if (!data)
		return -EINVAL;

	dev->wbuf[1] = freq->pen.mode;
	dev->wbuf[2] = freq->pen.start & 0xFF;
	dev->wbuf[3] = freq->pen.start >> 8;
	dev->wbuf[4] = freq->pen.end & 0xFF;
	dev->wbuf[5] = freq->pen.end >> 8;
	dev->wbuf[6] = freq->frame_cnt & 0xFF;
	dev->wbuf[7] = freq->frame_cnt >> 8;

	error = write_then_read(dev, dev->wbuf, 8, dev->rbuf, 0);
	if (error < 0)
		return error;

	return 0;
}

static int api_protocol_get_core_ver(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 8);
	if (error < 0)
		return error;

	_memcpy(dev->core_ver, dev->rbuf, 8);

	TP_MSG_ARR(dev->id, "[CoreVersion]", TYPE_U8, 4, dev->core_ver);

	return 0;
}

static int api_protocol_get_tuning_ver(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 8);
	if (error < 0)
		return error;

	_memcpy(dev->tuning_ver, dev->rbuf, 4);

	TP_MSG_ARR(dev->id, "[TurningVersion]", TYPE_U8, 4, dev->tuning_ver);

	return 0;
}

static int api_protocol_set_sw_reset(struct ilitek_ts_device *dev, void *data)
{
	int error;
	int wlen = 1;
	bool need_re_enum = (data) ? *(bool *)data : false;
	bool force_reset = (!data) ? true : false;

	/* make sure touch report in default I2C-HID mode after force reset */
	if (dev->_interface == interface_hid_over_i2c && !force_reset)
		return 0;

	dev->wbuf[1] = 0;
	error = write_then_read(dev, dev->wbuf, wlen, dev->rbuf, 0);
	if (error < 0)
		return error;

	dev->cb.delay_ms(dev->reset_time);

	if (dev->_interface == interface_usb && need_re_enum)
		return re_enum_helper(dev, enum_sw_reset);

	return 0;
}

static int api_protocol_get_sys_busy(struct ilitek_ts_device *dev, void *data)
{
	int error;

	if (data)
		*(u8 *)data = 0;

	_memset(dev->rbuf, 0, 64);
	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 1);
	if (error < 0)
		return error;

	if (data)
		*(u8 *)data = dev->rbuf[0];

	return 0;
}

static int api_protocol_get_ap_crc_v6(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u8 i, ic_num = (data) ? *(u8 *)data : 1;

	if (ic_num > ARRAY_SIZE(dev->ic))
		return -EINVAL;

	/*
	 * No need to get/print AP CRC by 0xC7 in BL mode,
	 * and 2501x ICs would get wrong crc in BL.
	 */
	if (dev->ic[0].mode != AP_MODE)
		return 0;

	error = write_then_read(dev, dev->wbuf, 1,
				dev->rbuf, 2 * ic_num);
	if (error < 0)
		return error;

	dev->ic[0].crc[0] = le16(dev->rbuf);
	TP_MSG(dev->id, "[FW CRC] Master: 0x%x\n", dev->ic[0].crc[0]);

	for (i = 1; i < ic_num; i++) {
		dev->ic[i].crc[0] = le16(dev->rbuf + 2 * i);
		TP_MSG(dev->id, "[FW CRC] Slave[" PFMT_U8 "]: 0x%x\n",
			i, dev->ic[i].crc[0]);
	}

	return 0;
}

static int api_protocol_get_ap_crc_v3(struct ilitek_ts_device *dev, void *data)
{
	int error, rlen;

	UNUSED(data);

	rlen = (is_231x(dev)) ? 4 : 2;

	if (dev->_interface == interface_i2c) {
		error = write_then_read(dev, dev->wbuf, 1, NULL, 0);
		if (error < 0)
			return error;
		dev->cb.delay_ms(600);
		error = write_then_read(dev, NULL, 0, dev->rbuf, rlen);
		if (error < 0)
			return error;
	} else {
		error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, rlen);
		if (error < 0)
			return error;
	}

	dev->ic[0].crc[0] = le16(dev->rbuf);
	if (is_231x(dev))
		dev->ic[0].crc[0] |= (le16(dev->rbuf + 2) << 16);

	TP_MSG(dev->id, "[Check Code] AP: 0x%x\n", dev->ic[0].crc[0]);

	return 0;
}

static int api_protocol_get_ap_crc(struct ilitek_ts_device *dev, void *data)
{
	if (dev->protocol.flag == PTL_V6)
		return api_protocol_get_ap_crc_v6(dev, data);
	else if (dev->protocol.flag == PTL_V3)
		return api_protocol_get_ap_crc_v3(dev, data);

	return -EINVAL;
}

static int api_protocol_set_mode_v3(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u8 mode = dev->wbuf[1];

	UNUSED(data);

	error = write_then_read(dev, dev->wbuf, 2, NULL, 0);
	if (error < 0)
		return error;

	/*
	 * Bridge with V3 IC need to set bridge into/out test mode additionally.
	 */
	if (dev->quirks & QUIRK_BRIDGE)
		return bridge_set_test_mode(dev, (mode) ? true : false);

	return 0;
}

static int api_protocol_write_enable(struct ilitek_ts_device *dev, void *data)
{
	int error;
	bool in_ap = (data) ? *(bool *)data : true;

	error = write_then_read(dev, dev->wbuf,
				(in_ap) ? 3 : 10, NULL, 0);
	if (error < 0)
		return error;

	/*
	 * V3 need AP/BL mode switch delay
	 */
	if (in_ap)
		dev->cb.delay_ms(is_231x(dev) ? 1000 : 100);
	else
		dev->cb.delay_ms(10);

	return 0;
}

static int api_protocol_write_data_v3(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 33, NULL, 0);
}

static int api_protocol_get_df_crc(struct ilitek_ts_device *dev,  void *data)
{
	int error;

	UNUSED(data);

	dev->ic[0].crc[1] = 0;
	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 4);
	if (error < 0)
		return error;

	dev->ic[0].crc[1] = le16(dev->rbuf + 2) << 16 | le16(dev->rbuf);
	TP_MSG(dev->id, "[Check Code] Data: 0x%x\n", dev->ic[0].crc[1]);

	return 0;
}

static int api_protocol_set_mode_v6(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 3, NULL, 0);
}

static int api_protocol_get_crc_by_addr(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	u8 type = (data) ? *(u8 *)data : 0;
	u32 start, end, t_ms;

	dev->wbuf[1] = type;

	if (type == CRC_CALCULATE) {
		start = le32(dev->wbuf + 2, 3);
		end = le32(dev->wbuf + 5, 3);
		t_ms = ((end - start) / 4096 + 1) * TOUT_CD * TOUT_CD_RATIO;

		error = write_then_wait_ack(dev, dev->wbuf, 8, t_ms);
		if (error < 0)
			return error;
		type = CRC_GET;
		return api_protocol_set_cmd(dev, GET_BLK_CRC_ADDR, &type);
	}

	return write_then_read(dev, dev->wbuf, 2, dev->rbuf, 2);
}

static int api_protocol_get_crc_by_num(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	u8 type = (data) ? *(u8 *)data : 0;
	u32 t_ms = (dev->wbuf[2] == 0) ? TOUT_CF_BLOCK_0 : TOUT_CF_BLOCK_N;

	dev->wbuf[1] = type;

	if (type == CRC_CALCULATE) {
		error = write_then_wait_ack(dev, dev->wbuf, 3, t_ms);
		if (error < 0)
			return error;
		type = CRC_GET;
		return api_protocol_set_cmd(dev, GET_BLK_CRC_NUM, &type);
	}

	return write_then_read(dev, dev->wbuf, 2, dev->rbuf, 2);
}

static int api_protocol_read_flash(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u32 code = *(u32 *)data;
	bool prepare;
	int rlen;

	if (dev->ic[0].mode != BL_MODE)
		return -EINVAL;

	if (dev->protocol.flag == PTL_V3) {
		if ((dev->protocol.ver & 0xFFFF00) == BL_PROTOCOL_V1_7 &&
		     dev->fw_ver[3] < 3) {
			TP_ERR(dev->id, "BL: 0x%x, FW: 0x" PFMT_X8 "-0x" PFMT_X8 "-0x" PFMT_X8 "-0x" PFMT_X8 " not support cmd: 0x" PFMT_X8 "\n",
				dev->protocol.ver, dev->fw_ver[0],
				dev->fw_ver[1], dev->fw_ver[2], dev->fw_ver[3],
				dev->wbuf[0]);
			return -EINVAL;
		}

		return write_then_read(dev, dev->wbuf, 1, dev->rbuf, 32);
	}

	if (!data)
		return -EINVAL;

	prepare = (code >> 16) ? true : false;
	rlen = code & 0xFFFF;

	if (prepare) {
		error = write_then_read(dev, dev->wbuf, 2, NULL, 0);
		dev->cb.delay_ms(100);

		return error;
	}

	if (dev->_interface == interface_i2c)
		error = write_then_read(dev, dev->wbuf, 2, dev->rbuf, rlen);
	else
		error = write_then_read(dev, NULL, 0, dev->rbuf, rlen);

	return error;
}

static int api_protocol_set_flash_addr(struct ilitek_ts_device *dev, void *data)
{
	int error;
	u32 addr = *(u32 *)data;

	if (!data)
		return -EINVAL;

	if (dev->protocol.flag == PTL_V3) {
		dev->wbuf[3] = addr & 0xFF;
		dev->wbuf[2] = (addr >> 8) & 0xFF;
		dev->wbuf[1] = (addr >> 16) & 0xFF;

		error = write_then_read(dev, dev->wbuf, 4, NULL, 0);
		if (error < 0)
			return error;

		dev->cb.delay_ms(5);

		return 0;
	}

	dev->wbuf[1] = addr & 0xFF;
	dev->wbuf[2] = (addr >> 8) & 0xFF;
	dev->wbuf[3] = (addr >> 16) & 0xFF;

	return write_then_read(dev, dev->wbuf, 4, NULL, 0);
}

static int api_protocol_set_data_len(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 3, NULL, 0);
}

static int api_protocol_set_flash_enable(struct ilitek_ts_device *dev,
					 void *data)
{
	int error;
	u8 type = (data) ? *(u8 *)data : 0;
	int wlen, rlen;
	bool in_ap = ((type & 0x1) != 0) ? true : false;
	bool is_slave = ((type & 0x2) != 0) ? true : false;

	u32 set_start, set_end, get_start, get_end;

	if (!is_slave) {
		wlen = (in_ap) ? 3 : 9;
		rlen = (in_ap || dev->protocol.ver < 0x010803) ? 0 : 6;

		set_start = le32(dev->wbuf + 3, 3);
		set_end = le32(dev->wbuf + 6, 3);

		error = write_then_read(dev, dev->wbuf, wlen,
					dev->rbuf, rlen);
		if (error < 0)
			return error;

		if (in_ap || dev->protocol.ver < 0x010803)
			return 0;

		get_start = le32(dev->rbuf, 3);
		get_end = le32(dev->rbuf + 3, 3);

		if (set_start != get_start || set_end != get_end) {
			TP_ERR(dev->id, "start/end addr.: 0x%x/0x%x vs. 0x%x/0x%x not match\n",
				set_start, set_end, get_start, get_end);
			return -EINVAL;
		}

		return 0;
	}

	error = write_then_wait_ack(dev, dev->wbuf, 9,
				    TOUT_CC_SLAVE * TOUT_CC_SLAVE_RATIO);
	if (error < 0)
		return error;
	dev->cb.delay_ms(2000);

	return (dev->_interface == interface_usb) ?
		re_enum_helper(dev, enum_sw_reset) : 0;
}

static int api_protocol_write_data_v6(struct ilitek_ts_device *dev, void *data)
{
	int wlen;

	if (!data)
		return -EINVAL;

	wlen = *(int *)data;

	return write_then_wait_ack(dev, dev->wbuf, wlen, TOUT_C3 * TOUT_C3_RATIO);
}

static int api_protocol_write_data_m2v(struct ilitek_ts_device *dev, void *data)
{
	int wlen;

	if (!data)
		return -EINVAL;

	wlen = *(int *)data;

	return write_then_wait_ack(dev, dev->wbuf, wlen, 30000);
}

static int api_protocol_access_slave(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct ilitek_slave_access *access;

	if (!data)
		return -EINVAL;

	access = (struct ilitek_slave_access *)data;

	dev->wbuf[1] = access->slave_id;
	dev->wbuf[2] = access->func;
	_memset(dev->rbuf, 0, sizeof(dev->rbuf));

	switch (access->func) {
	case CMD_GET_AP_CRC:
		error = write_then_read(dev, dev->wbuf, 3, dev->rbuf, 4);
		*((u32 *)access->data) = le32(dev->rbuf, 4);
		break;

	case CMD_GET_MCU_MOD:
		error = write_then_read(dev, dev->wbuf, 3, dev->rbuf, 1);
		*((u8 *)access->data) = dev->rbuf[0];
		break;

	case CMD_GET_FW_VER:
		error = write_then_read(dev, dev->wbuf, 3, dev->rbuf, 8);
		_memcpy((u8 *)access->data, dev->rbuf, 8);

		break;

	case CMD_WRITE_ENABLE:
		dev->wbuf[3] = ((u8 *)access->data)[0];
		dev->wbuf[4] = ((u8 *)access->data)[1];
		dev->wbuf[5] = ((u8 *)access->data)[2];
		dev->wbuf[6] = ((u8 *)access->data)[3];
		dev->wbuf[7] = ((u8 *)access->data)[4];
		dev->wbuf[8] = ((u8 *)access->data)[5];

		error = write_then_wait_ack(dev, dev->wbuf, 9, 5000);
		break;

	default:
		error = write_then_wait_ack(dev, dev->wbuf, 3, 5000);
		break;
	};

	return error;
}

static int api_protocol_set_ap_mode(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	if (dev->cb.mode_switch_notify)
		dev->cb.mode_switch_notify(true, false, dev->_private);

	error = write_then_read(dev, dev->wbuf, 1, NULL, 0);

	if (dev->cb.mode_switch_notify)
		dev->cb.mode_switch_notify(false, false, dev->_private);

	return error;
}

static int api_protocol_set_bl_mode(struct ilitek_ts_device *dev, void *data)
{
	int error;

	UNUSED(data);

	if (dev->cb.mode_switch_notify)
		dev->cb.mode_switch_notify(true, false, dev->_private);

	error = write_then_read(dev, dev->wbuf, 1, NULL, 0);

	if (dev->cb.mode_switch_notify)
		dev->cb.mode_switch_notify(false, true, dev->_private);

	return error;
}

static int api_protocol_set_idle(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 2, NULL, 0);
}

static int api_protocol_set_sleep(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 1, NULL, 0);
}

static int api_protocol_set_wakeup(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 1, NULL, 0);
}

static int api_protocol_set_func_mode(struct ilitek_ts_device *dev, void *data)
{
	int error;
	bool get = (data) ? *(bool *)data : true;

	if (!data)
		return -EINVAL;

	if (get) {
		error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 3);
		if (error < 0)
			return error;

		dev->func.header = be16(dev->rbuf);
		dev->func.mode = dev->rbuf[2];
		TP_MSG(dev->id, "[FW Mode] 0x" PFMT_X8 "\n", dev->func.mode);

		return 0;
	}

	if (dev->protocol.flag == PTL_V3) {
		error = write_then_read(dev, dev->wbuf, 4, NULL, 0);
		if (error < 0)
			return error;
		error = api_check_busy(dev, 1000, 10);
		if (error < 0)
			return error;
		return 0;
	} else if (dev->protocol.flag == PTL_V6) {
		error = write_then_wait_ack(dev, dev->wbuf, 4,
					    TOUT_68 * TOUT_68_RATIO);
		if (error < 0)
			return error;
		return 0;
	}

	return -EINVAL;
}

static int api_protocol_c_model_info(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	if (dev->protocol.ver < 0x060008)
		return write_then_read(dev, dev->wbuf, 12, NULL, 0);

	return write_then_read(dev, dev->wbuf, 18, NULL, 0);
}

static int api_protocol_tuning_para_v3(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_read(dev, dev->wbuf, 2, NULL, 0);
}

static int api_protocol_tuning_para_v6(struct ilitek_ts_device *dev, void *data)
{
	int error;
	struct tuning_para_settings tuning =
		*(struct tuning_para_settings *)data;
	u32 wlen;

	int header;
	int tout_ms;

	if (!data)
		return -EINVAL;

	dev->wbuf[1] = tuning.func;
	dev->wbuf[2] = tuning.ctrl;
	dev->wbuf[3] = tuning.type;

	if (tuning.func == 0x0) {
		wlen = 4;
		tout_ms = TOUT_65_READ * TOUT_65_READ_RATIO;

		switch (tuning.ctrl) {
		case 0x3: case 0x5: case 0x10:
			wlen += tuning.len;
			tout_ms = TOUT_65_WRITE * TOUT_65_WRITE_RATIO;

			//TODO: add memory range check
			_memcpy(dev->wbuf + 4, tuning.buf, tuning.len);
			break;
		}

		return write_then_wait_ack(dev, dev->wbuf, wlen, tout_ms);
	}

	switch (tuning.ctrl) {
	case 0x2: case 0x4:
		error = write_then_read(dev, dev->wbuf, 4, NULL, 0);
		if (error < 0)
			return error;
		error = read_ctrl_in(dev, CMD_TUNING_PARA_V6,
				     dev->rbuf, 1024, 5000);
		if (error < 0)
			return error;

		header = (dev->_interface == interface_i2c) ? 5 : 6;
		header = (dev->quirks & QUIRK_BRIDGE) ? 6 : header;

		_memcpy(tuning.buf, dev->rbuf + header, tuning.len);

		break;
	}

	return 0;
}

static int api_protocol_set_cdc_init_v3(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	int wlen;
	struct cdc_settings *set = (struct cdc_settings *)data;

	if (!data)
		return -EINVAL;

	if (set->is_freq) {
		dev->wbuf[1] = 0x0F;
		dev->wbuf[2] = set->freq.sine.start;
		dev->wbuf[3] = set->freq.sine.end;
		dev->wbuf[4] = set->freq.sine.step;

		error = write_then_read(dev, dev->wbuf, 5, NULL, 0);
		if (error < 0)
			return error;

		dev->cb.delay_ms(200);
	} else {
		dev->wbuf[1] = set->cmd;
		dev->wbuf[2] = 0;
		dev->wbuf[3] = set->config & 0xFF;
		wlen = 4;

		if (set->config & 0xFF00) {
			dev->wbuf[3] = (set->config >> 8) & 0xFF;
			dev->wbuf[4] = set->config & 0xFF;
			wlen = 5;
		}

		error = write_then_read(dev, dev->wbuf, wlen, NULL, 0);
		if (error < 0)
			return error;

		dev->cb.delay_ms(10);
	}

	return api_check_busy(dev, 15000, 10);
}

static int api_protocol_get_cdc_v6(struct ilitek_ts_device *dev, void *data)
{
	UNUSED(data);

	return write_then_wait_ack(dev, dev->wbuf, 1, TOUT_F2 * TOUT_F2_RATIO);
}

static int api_protocol_set_cdc_init_v6(struct ilitek_ts_device *dev,
					void *data)
{
	struct cdc_settings *set = (struct cdc_settings *)data;
	int wlen = 1, tout_ms;

	if (!data)
		return -EINVAL;

	dev->wbuf[wlen++] = set->cmd;

	if (set->is_freq) {
		tout_ms = set->freq.sine.steps * TOUT_F1_FREQ_MC;
		tout_ms += (set->freq.mc_swcap.steps * TOUT_F1_FREQ_SC);
		tout_ms += (set->freq.sc_swcap.steps * TOUT_F1_FREQ_SC);
		tout_ms += (set->freq.dump1.steps * TOUT_F1_FREQ_SC);
		tout_ms += (set->freq.dump2.steps * TOUT_F1_FREQ_SC);

		tout_ms += (set->freq.pen.steps * TOUT_F1_FREQ_MC);

		tout_ms *= MAX(set->freq.frame_cnt, 1);
		tout_ms *= TOUT_F1_FREQ_RATIO;

		return write_then_wait_ack(dev, dev->wbuf, wlen, tout_ms);
	}

	if (set->is_p2p) {
		tout_ms = MAX(set->p2p.frame_cnt, 1) *
			TOUT_F1_OTHER * TOUT_F1_OTHER_RATIO;
		return write_then_wait_ack(dev, dev->wbuf, wlen, tout_ms);
	}

	if (dev->protocol.ver > 0x60008)
		dev->wbuf[wlen++] = set->config & 0xFF;

	tout_ms = TOUT_F1_OTHER * TOUT_F1_OTHER_RATIO;
	if (set->is_curve) {
		tout_ms = set->curve.dump.steps * TOUT_F1_CURVE;
		tout_ms += (set->curve.charge.steps * TOUT_F1_CURVE);
		tout_ms *= MAX(set->curve.frame_cnt, 1);
		tout_ms *= TOUT_F1_CURVE_RATIO;
	} else if (set->is_short) {
		tout_ms = TOUT_F1_SHORT * TOUT_F1_SHORT_RATIO;
	} else if (set->is_open) {
		tout_ms = MAX(set->open.frame, 1) *
			TOUT_F1_OPEN * TOUT_F1_OPEN_RATIO;
	} else if (set->is_key && dev->protocol.ver < 0x6000a) {
		tout_ms = TOUT_F1_KEY;
	}

	return write_then_wait_ack(dev, dev->wbuf, wlen, tout_ms);
}

static int api_protocol_get_cdc_info_v3(struct ilitek_ts_device *dev,
					void *data)
{
	int error;
	u32 *cdc_info = (u32 *)data;

	if (!data)
		return -EINVAL;

	error = write_then_read(dev, dev->wbuf, 1, dev->rbuf, 4);
	if (error < 0)
		return error;

	*cdc_info = le32(dev->rbuf, 4);

	return 0;
}

int api_protocol_set_cmd(void *handle, u8 idx, void *data)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	if (!dev || idx >= ARRAY_SIZE(protocol_maps))
		return -EINVAL;

	if (!(dev->protocol.flag & protocol_maps[idx].flag) &&
	    protocol_maps[idx].flag != PTL_ANY) {
		TP_ERR(dev->id, "Unexpected cmd: " PFMT_C8 " for 0x" PFMT_X8 " only, now is 0x" PFMT_X8 "\n",
			protocol_maps[idx].desc, protocol_maps[idx].flag,
			dev->protocol.flag);
		return -EINVAL;
	}

	dev->wbuf[0] = protocol_maps[idx].cmd;
	error = protocol_maps[idx].func(dev, data);
	if (error < 0) {
		TP_ERR(dev->id, "failed to execute cmd: 0x" PFMT_X8 " " PFMT_C8 ", err: %d\n",
			protocol_maps[idx].cmd, protocol_maps[idx].desc, error);
		return error;
	}

	return 0;
}

int api_set_ctrl_mode(void *handle, u8 mode, bool eng, bool force)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;
	u8 cmd = 0;

	if (!force && dev->fw_mode == mode)
		return 0;

	_memset(dev->wbuf, 0, sizeof(dev->wbuf));

	if (dev->protocol.flag == PTL_V3) {
		/* V3 only support suspend and normal mode */
		if (mode != mode_normal &&
		    mode != mode_suspend &&
		    mode != mode_test)
			return -EPROTONOSUPPORT;
		dev->wbuf[1] = (mode == mode_normal) ? 0x00 : 0x01;
		cmd = SET_TEST_MOD;
	} else if (dev->protocol.flag == PTL_V6) {
		dev->wbuf[1] = mode;
		dev->wbuf[2] = (eng) ? 0x01 : 0x00;
		cmd = SET_MOD_CTRL;
	}

	error = api_protocol_set_cmd(dev, cmd, NULL);
	if (error < 0)
		return error;

	/* switch from test to normal mode should wait 1 sec. delay */
	if (dev->protocol.flag == PTL_V6 &&
	    dev->fw_mode == mode_test && mode == mode_normal)
		dev->cb.delay_ms(1000);
	else
		dev->cb.delay_ms(100);

	dev->fw_mode = mode;

	return 0;
}

u16 api_get_block_crc_by_addr(void *handle, u8 type,
			      u32 start, u32 end)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	_memset(dev->wbuf, 0, 64);

	dev->wbuf[2] = start;
	dev->wbuf[3] = (start >> 8) & 0xFF;
	dev->wbuf[4] = (start >> 16) & 0xFF;
	dev->wbuf[5] = end & 0xFF;
	dev->wbuf[6] = (end >> 8) & 0xFF;
	dev->wbuf[7] = (end >> 16) & 0xFF;
	if (api_protocol_set_cmd(dev, GET_BLK_CRC_ADDR, &type) < 0)
		return 0;

	return le16(dev->rbuf);
}

u16 api_get_block_crc_by_num(void *handle, u8 type,
			     u8 block_num)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	_memset(dev->wbuf, 0, 64);

	dev->wbuf[2] = block_num;
	if (api_protocol_set_cmd(dev, GET_BLK_CRC_NUM, &type) < 0)
		return 0;

	return le16(dev->rbuf);
}

int api_set_data_len(void *handle, u16 data_len)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	_memset(dev->wbuf, 0, 64);

	dev->wbuf[1] = data_len & 0xFF;
	dev->wbuf[2] = (data_len >> 8) & 0xFF;

	return api_protocol_set_cmd(dev, SET_DATA_LEN, NULL);
}

int api_write_enable_v6(void *handle, bool in_ap, bool is_slave,
			u32 start, u32 end)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 type;

	_memset(dev->wbuf, 0, 64);
	dev->wbuf[1] = 0x5A;
	dev->wbuf[2] = 0xA5;
	dev->wbuf[3] = start & 0xFF;
	dev->wbuf[4] = (start >> 8) & 0xFF;
	dev->wbuf[5] = start >> 16;
	dev->wbuf[6] = end & 0xFF;
	dev->wbuf[7] = (end >> 8) & 0xFF;
	dev->wbuf[8] = end >> 16;

	type = (in_ap) ? 0x1 : 0x0;
	type |= (is_slave) ? 0x2 : 0x0;

	return api_protocol_set_cmd(dev, SET_FLASH_EN, &type);
}

int api_write_data_v6(void *handle, int wlen)
{
	return api_protocol_set_cmd(handle, WRITE_DATA_V6, &wlen);
}

int api_access_slave(void *handle, u8 id, u8 func, void *data)
{
	struct ilitek_slave_access access;

	access.slave_id = id;
	access.func = func;
	access.data = data;

	return api_protocol_set_cmd(handle, ACCESS_SLAVE, &access);
}

int api_write_enable_v3(void *handle, bool in_ap, bool write_ap,
			u32 end, u32 checksum)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	_memset(dev->wbuf, 0, 64);
	dev->wbuf[1] = 0x5A;
	dev->wbuf[2] = 0xA5;
	dev->wbuf[3] = (write_ap) ? 0x0 : 0x1;
	dev->wbuf[4] = (end >> 16) & 0xFF;
	dev->wbuf[5] = (end >> 8) & 0xFF;
	dev->wbuf[6] = end & 0xFF;
	dev->wbuf[7] = (checksum >> 16) & 0xFF;
	dev->wbuf[8] = (checksum >> 8) & 0xFF;
	dev->wbuf[9] = checksum & 0xFF;

	return api_protocol_set_cmd(dev, WRITE_ENABLE, &in_ap);
}

int api_write_data_v3(void *handle)
{
	return api_protocol_set_cmd(handle, WRITE_DATA_V3, NULL);
}

int api_check_busy(void *handle, int timeout_ms, int delay_ms)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 busy;

	/* retry 2 times at least */
	int i = MAX(DIV_ROUND_UP(timeout_ms, delay_ms), 2);

	_memset(dev->wbuf, 0, 64);

	while (i--) {
		api_protocol_set_cmd(dev, GET_SYS_BUSY, &busy);
		if (busy == ILITEK_TP_SYSTEM_READY)
			return 0;

		/* delay ms for each check busy */
		dev->cb.delay_ms(delay_ms);

		/* if caller set no_retry then skip check busy retry */
		if (dev->setting.no_retry)
			break;
	}

	TP_WARN(dev->id, "check busy timeout: %d ms, state: 0x" PFMT_X8 "\n",
		timeout_ms, busy);

	return -EILIBUSY;
}

int api_to_bl_mode(void *handle, bool to_bl,
		   u32 start, u32 end)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int cnt = 0, retry = 15;
	const u8 target_mode = (to_bl) ? BL_MODE : AP_MODE;

	do {
		if (api_protocol_set_cmd(dev, GET_MCU_MOD, NULL) < 0)
			continue;

		if (dev->ic[0].mode == target_mode)
			goto success_change_mode;

		if (to_bl) {
			if (dev->protocol.flag == PTL_V3 &&
			    api_write_enable_v3(dev, true, false, 0, 0) < 0)
				continue;
			else if (dev->protocol.flag == PTL_V6 &&
				 api_write_enable_v6(dev, true, false,
						     0, 0) < 0)
				continue;

			api_protocol_set_cmd(dev, SET_BL_MODE, NULL);
		} else {
			if (dev->protocol.flag == PTL_V3 &&
			    api_write_enable_v3(dev, true, false, 0, 0) < 0)
				continue;
			else if (dev->protocol.flag == PTL_V6 &&
				 api_write_enable_v6(dev, false, false,
						     start, end) < 0)
				continue;

			api_protocol_set_cmd(dev, SET_AP_MODE, NULL);
		}

		switch (dev->_interface) {
		case interface_hid_over_i2c:
		case interface_i2c:
			dev->cb.delay_ms(1000 + 100 * cnt);
			break;
		case interface_usb:
			re_enum_helper(dev, enum_ap_bl);
			break;
		}
	} while (!dev->setting.no_retry && cnt++ < retry);

	TP_ERR(dev->id, "current mode: 0x" PFMT_X8 ", change to " PFMT_C8 " mode failed\n",
		dev->ic[0].mode, (to_bl) ? "BL" : "AP");
	return -EFAULT;

success_change_mode:
	TP_MSG(dev->id, "current mode: 0x" PFMT_X8 " " PFMT_C8 " mode\n",
		dev->ic[0].mode, (to_bl) ? "BL" : "AP");

	/* update fw ver. in AP/BL mode */
	api_protocol_set_cmd(dev, GET_FW_VER, NULL);

	/* update protocol ver. in AP/BL mode */
	api_protocol_set_cmd(dev, GET_PTL_VER, NULL);

	return 0;
}

int api_set_idle(void *handle, bool enable)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	_memset(dev->wbuf, 0, 64);
	dev->wbuf[1] = (enable) ? 1 : 0;
	return api_protocol_set_cmd(dev, SET_MCU_IDLE, NULL);
}

int api_set_func_mode(void *handle, u8 mode)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;
	bool get = false;

	_memset(dev->wbuf, 0, 64);

	switch (dev->protocol.flag) {
	case PTL_V3:
		dev->wbuf[1] = 0x55;
		dev->wbuf[2] = 0xAA;
		break;
	case PTL_V6:
		dev->wbuf[1] = 0x5A;
		dev->wbuf[2] = 0xA5;
		break;
	default:
		TP_ERR(dev->id, "unrecognized protocol: %x, flag: " PFMT_U8 "",
			dev->protocol.ver, dev->protocol.flag);
		return -EINVAL;
	}
	dev->wbuf[3] = mode;

	if (dev->protocol.ver < 0x30400) {
		TP_ERR(dev->id, "protocol: 0x%x not support\n",
			dev->protocol.ver);
		return -EINVAL;
	}

	error = api_protocol_set_cmd(dev, SET_FUNC_MOD, &get);
	if (error < 0)
		return error;
	error = api_get_func_mode(dev);
	if (error < 0)
		return error;

	return (dev->func.mode == mode) ? 0 : -EFAULT;
}

int api_get_func_mode(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	bool get = true;

	return api_protocol_set_cmd(dev, SET_FUNC_MOD, &get);
}

int api_erase_data_v3(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int error;

	_memset(dev->wbuf, 0xff, sizeof(dev->wbuf));

	TP_INFO(dev->id, "erase data flash for " PFMT_C8 ", mode: " PFMT_X8 "\n",
		dev->mcu_info.ic_name, dev->ic[0].mode);

	if (is_231x(dev)) {
		/* V3 231x only support erase data flash in AP mode */
		if (dev->ic[0].mode != AP_MODE) {
			TP_WARN(dev->id, "invalid mode: " PFMT_X8 " for data erase\n",
				dev->ic[0].mode);
			return 0;
		}

		error = api_write_enable_v3(dev, true, false, 0, 0);
		if (error < 0)
			return error;

		dev->cb.delay_ms(100);

		dev->wbuf[1] = 0x02;
		error = api_protocol_set_cmd(dev, TUNING_PARA_V3, NULL);
		if (error < 0)
			return error;

		switch (dev->_interface) {
		case interface_usb:
			return re_enum_helper(dev, enum_ap_bl);
		default:
			dev->cb.delay_ms(1500);
			break;
		}
	} else {
		/* V3 251x only support erase data flash in BL mode */
		if (dev->ic[0].mode != BL_MODE) {
			TP_WARN(dev->id, "invalid mode: " PFMT_X8 " for data erase\n",
				dev->ic[0].mode);
			return 0;
		}

		error = api_write_enable_v3(dev, false, false, 0xf01f, 0);
		if (error < 0)
			return error;

		dev->cb.delay_ms(5);

		_memset(dev->wbuf + 1, 0xFF, 32);
		error = api_write_data_v3(dev);
		if (error < 0)
			return error;

		dev->cb.delay_ms(500);
	}

	return 0;
}

static int api_read_flash_v3(struct ilitek_ts_device *dev, u8 *buf,
			     u32 start, u32 len)
{
	int error;
	u32 addr, end = start + len, copied;

	for (addr = start, copied = 0; addr < end;
	     addr += 32, copied += 32) {
		error = api_protocol_set_cmd(dev, SET_ADDR, &addr);
		if (error < 0)
			return error;
		error = api_protocol_set_cmd(dev, READ_FLASH, NULL);
		if (error < 0)
			return error;

		_memcpy(buf + copied, dev->rbuf, 32);
	}

	return 0;
}

static int api_read_flash_v6(struct ilitek_ts_device *dev, u8 *buf,
			     u32 start, u32 len)
{
	int error;
	u32 code, addr, end = start + len, copied;
	u16 data_len;

	if (dev->ic[0].mode != BL_MODE)
		return -EINVAL;

	for (addr = start, copied = 0; addr < end;
	     addr += data_len, copied += data_len) {
		if (end - addr > 1024)
			data_len = 2048;
		else if (end - addr > 256)
			data_len = 1024;
		else if (end - addr > 64)
			data_len = 256;
		else
			data_len = 64;

		error = api_set_data_len(dev, data_len);
		if (error < 0)
			return error;
		error = api_protocol_set_cmd(dev, SET_ADDR, &addr);
		if (error < 0)
			return error;

		dev->wbuf[1] = 0x1; code = 1 << 16;
		error = api_protocol_set_cmd(dev, READ_FLASH, &code);
		if (error < 0)
			return error;

		dev->wbuf[1] = 0x0; code = data_len & 0xFFFF;
		error = api_protocol_set_cmd(dev, READ_FLASH, &code);
		if (error < 0)
			return error;

		if (dev->_interface == interface_hid_over_i2c)
			_memcpy(buf + copied, dev->rbuf + 5, data_len);
		else
			_memcpy(buf + copied, dev->rbuf, data_len);
	}

	return 0;
}

int api_read_flash(void *handle, u8 *buf,
		   u32 start, u32 len)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (dev->protocol.flag == PTL_V3)
		return api_read_flash_v3(dev, buf, start, len);

	return api_read_flash_v6(dev, buf, start, len);
}

int _api_read_mp_result(void *handle)
{
	int error;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	struct tuning_para_settings tuning;
	u8 *buf = (u8 *)(&dev->mp);
	u32 addr = (is_29xx(dev)) ? 0x2e000 : 0x3e000;

	if (dev->ic[0].mode == BL_MODE)
		return api_read_flash_v6(dev, buf, addr, 1000);

	/* 1000 bytes data/ 2 bytes crc/ 1 bytes checksum */
	tuning.len = 1003;
	tuning.buf = buf;

	tuning.func = 0x0; tuning.ctrl = 0x4; tuning.type = 0x10;
	error = api_protocol_set_cmd(dev, TUNING_PARA_V6, &tuning);
	if (error < 0)
		return error;

	tuning.func = 0x1; tuning.ctrl = 0x4; tuning.type = 0x10;

	return api_protocol_set_cmd(dev, TUNING_PARA_V6, &tuning);
}

int api_read_mp_result(void *handle)
{
	int error;

	api_set_ctrl_mode(handle, mode_suspend, false, true);
	error = _api_read_mp_result(handle);
	api_set_ctrl_mode(handle, mode_normal, false, true);

	return error;
}

int _api_write_mp_result(void *handle, struct mp_station *mp)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	struct tuning_para_settings tuning;
	u8 *buf = (u8 *)mp;
	u16 crc;

	crc = get_crc(0, 1000, buf, sizeof(struct mp_station));
	mp->crc = crc;

	tuning.func = 0x0;
	tuning.ctrl = 0x5;
	tuning.type = 0x10;
	tuning.buf = buf;
	tuning.len = 1002;

	return api_protocol_set_cmd(dev, TUNING_PARA_V6, &tuning);
}

int api_write_mp_result(void *handle, struct mp_station *mp)
{
	int error;

	api_set_ctrl_mode(handle, mode_suspend, false, true);
	error = _api_write_mp_result(handle, mp);
	api_set_ctrl_mode(handle, mode_normal, false, true);

	return error;
}

static void mp_result(const char *item, u8 data)
{
	if (data == 1)
		TP_INFO(NULL, PFMT_C8 " Result: PASS\n", item);
	else if (data == 2)
		TP_INFO(NULL, PFMT_C8 " Result: NG\n", item);
	else
		TP_INFO(NULL, PFMT_C8 " Result: N/A\n", item);
}

void api_decode_mp_result(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	struct mp_station *mp = &dev->mp;
	int i, j;
	struct mp_station_old *_mp;
	char module_name[32];
	char bar_code[256];

	TP_DBG_ARR(NULL, "[MpResult Raw]:", TYPE_U8, 1002, (u8 *)mp);

	TP_MSG(NULL, "mp result ver: 0x%08x\n", mp->info.mp_result_ver);

	/* For Old Mp Result Format */
	if (mp->info.mp_result_ver == 0xFFFFFFFF ||
	    mp->info.mp_result_ver < 0x01000000) {
		_mp = (struct mp_station_old *)mp;

		for (i = 0; i < 9; i++) {
			TP_INFO(NULL, "***Station%d***\n", i + 1);

			if (!_mp->station[i].week ||
			    _mp->station[i].week == 0xFF) {
				TP_INFO(NULL, "No Test\n");
				continue;
			}

			TP_INFO(NULL, "Week of year : " PFMT_U8 "\n",
				_mp->station[i].week);
			TP_INFO(NULL, "Year : 20%02u\n",
				_mp->station[i].year);
			TP_INFO_ARR(NULL, "Firmware Version : ",
				   TYPE_U8, 8, _mp->station[i].fw_ver);

			_memset(module_name, 0, sizeof(module_name));
			_memcpy(module_name, _mp->station[i].module,
				sizeof(_mp->station[i].module));
			module_name[sizeof(_mp->station[i].module) + 1] = '\0';
			for (j = 0; j < (int)sizeof(_mp->station[i].module);
			     j++) {
				if ((u8)module_name[j] == 0xFF) {
					module_name[j] = '\0';
					break;
				}
			}
			TP_INFO(NULL, "Module Name : [" PFMT_C8 "]\n",
				module_name);

			mp_result("Short Test", _mp->station[i].short_test);
			mp_result("Open Test", _mp->station[i].open_test);
			mp_result("Self Cap Test", _mp->station[i].self_test);
			mp_result("Uniformity Test",
				_mp->station[i].uniform_test);
			mp_result("DAC Test", _mp->station[i].dac_test);
			mp_result("Key Raw Test", _mp->station[i].key_test);;
			mp_result("Painting Test", _mp->station[i].paint_test);
			mp_result("MicroOpen Test", _mp->station[i].mopen_test);
			mp_result("GPIO Test", _mp->station[i].gpio_test);
			mp_result("Final", _mp->station[i].final_result);

			_memset(bar_code, 0, sizeof(bar_code));
			_memcpy(bar_code, _mp->station[i].bar_code,
				sizeof(_mp->station[i].bar_code));
			bar_code[sizeof(_mp->station[i].bar_code) + 1] = '\0';
			for (j = 0; j < (int)sizeof(_mp->station[i].bar_code);
			     j++) {
				if ((u8)bar_code[j] == 0xFF) {
					bar_code[j] = '\0';
					break;
				}
			}

			TP_INFO(NULL, "Bar Code : " PFMT_C8 "\n", bar_code);
			TP_INFO(NULL, "Customer ID : 0x%04x\n",
				_mp->station[i].custom_id);
			TP_INFO(NULL, "FWID : 0x%04x\n",
				_mp->station[i].fwid);
		}

		return;
	}

	TP_INFO(NULL, "[MP Result]\n");
	TP_INFO(NULL, "***Customer Info***\n");

	TP_INFO(NULL, "MPResult Version:%02X.%02X.%02X.%02X\n",
		(mp->info.mp_result_ver >> 24) & 0xFF,
		(mp->info.mp_result_ver >> 16) & 0xFF,
		(mp->info.mp_result_ver >> 8) & 0xFF,
		mp->info.mp_result_ver & 0xFF);
	TP_INFO(NULL, "Customer ID : 0x%04X\n", mp->info.customer_id);
	TP_INFO(NULL, "FW ID : 0x%04X\n", mp->info.fwid);

	for (i = 0; i < (int)ARRAY_SIZE(mp->station); i++) {
		TP_INFO(NULL, "***Station%d***\n", i + 1);

		if (!mp->station[i].week || mp->station[i].week == 0xFF) {
			TP_INFO(NULL, "No Test\n");
			continue;
		}

		TP_INFO(NULL, "Week of year : %d\n", mp->station[i].week);
		TP_INFO(NULL, "Year : 20%02d\n", mp->station[i].year);
		TP_INFO(NULL, "Firmware Version : 0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X\n",
			mp->station[i].fw_ver[7], mp->station[i].fw_ver[6],
			mp->station[i].fw_ver[5], mp->station[i].fw_ver[4],
			mp->station[i].fw_ver[3], mp->station[i].fw_ver[2],
			mp->station[i].fw_ver[1], mp->station[i].fw_ver[0]);

		_memset(module_name, 0, sizeof(module_name));
		_memcpy(module_name, mp->station[i].module,
			sizeof(mp->station[i].module));
		module_name[sizeof(mp->station[i].module) + 1] = '\0';
		for (j = 0; j < (int)sizeof(mp->station[i].module); j++) {
			if ((u8)module_name[j] == 0xFF) {
				module_name[j] = '\0';
				break;
			}
		}
		TP_INFO(NULL, "Module Name : [" PFMT_C8 "]\n", module_name);

		mp_result("Short Test", mp->station[i].short_test);
		mp_result("Open Test", mp->station[i].open_test);
		mp_result("Self Cap Test", mp->station[i].self_test);
		mp_result("Uniformity Test", mp->station[i].uniform_test);
		mp_result("DAC Test", mp->station[i].dac_test);
		mp_result("Key Raw Test", mp->station[i].key_test);;
		mp_result("Painting Test", mp->station[i].paint_test);
		mp_result("MicroOpen Test", mp->station[i].mopen_test);
		mp_result("GPIO Test", mp->station[i].gpio_test);
		mp_result("Final", mp->station[i].final_result);

		TP_INFO(NULL, "Tool Version : " PFMT_U8 "." PFMT_U8 "." PFMT_U8 "." PFMT_U8 "." PFMT_U8 "." PFMT_U8 "." PFMT_U8 "." PFMT_U8 "\n",
			mp->station[i].tool_ver[7], mp->station[i].tool_ver[6],
			mp->station[i].tool_ver[5], mp->station[i].tool_ver[4],
			mp->station[i].tool_ver[3], mp->station[i].tool_ver[2],
			mp->station[i].tool_ver[1], mp->station[i].tool_ver[0]);

		_memset(bar_code, 0, sizeof(bar_code));
		_memcpy(bar_code, mp->station[i].bar_code,
			sizeof(mp->station[i].bar_code));
		bar_code[sizeof(mp->station[i].bar_code) + 1] = '\0';
		TP_INFO(NULL, "Bar Code : " PFMT_C8 "\n", bar_code);
		TP_INFO(NULL, "Customer ID : 0x%04x\n",
			mp->station[i].custom_id);
		TP_INFO(NULL, "FWID : 0x%04x\n", mp->station[i].fwid);
	}
}

int api_read_tuning(void *handle, u8 *buf, int rlen)
{
	int error;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	struct tuning_para_settings tuning;
	int got, need;

	if (dev->ic[0].mode == BL_MODE)
		return -EINVAL;

	error = api_set_ctrl_mode(dev, mode_suspend, false, true);
	if (error < 0)
		return error;

	for (got = 0, need = rlen; need > 0; got += 1024, need -= 1024) {
		tuning.len = MIN(need, 1024);
		tuning.buf = buf + got;

		tuning.func = 0x0; tuning.ctrl = 0x2; tuning.type = 0x0;
		error = api_protocol_set_cmd(dev, TUNING_PARA_V6, &tuning);
		if (error < 0)
			return error;

		tuning.func = 0x1; tuning.ctrl = 0x2; tuning.type = 0x0;
		error = api_protocol_set_cmd(dev, TUNING_PARA_V6, &tuning);
		if (error < 0)
			return error;
	}

	TP_DBG_ARR(dev->id, "[tuning]:", TYPE_U8, rlen, buf);

	return api_set_ctrl_mode(dev, mode_normal, false, true);
}

int api_write_data_m2v(void *handle, int wlen)
{
	return api_protocol_set_cmd(handle, WRITE_DATA_M2V, &wlen);
}

int api_to_bl_mode_m2v(void *handle, bool to_bl)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	int cnt = 0, retry = 15;
	const u8 target_mode = (to_bl) ? BL_MODE : AP_MODE;
	u8 mode;

	if (dev->_interface != interface_usb)
		return -EINVAL;

	do {
		dev->cb.delay_ms(100);//Reed Add : 20230927

		if (api_access_slave(dev, 0x80, CMD_GET_MCU_MOD, &mode) < 0)
			continue;

		if (mode == target_mode)
			goto success_change_mode;

		dev->cb.delay_ms(300);//Reed Add : 20230927

		if (to_bl && api_access_slave(dev, 0x80, CMD_SET_BL_MODE,
					      NULL) < 0)
			continue;
		else if (!to_bl && api_access_slave(dev, 0x80, CMD_SET_AP_MODE,
						    NULL) < 0)
			continue;

		do {
			dev->cb.delay_ms(100);//Reed Add : 20230927
			if (!api_access_slave(dev, 0x80, CMD_GET_MCU_MOD, &mode) &&
			    mode == target_mode)
				goto success_change_mode;
			dev->cb.delay_ms(5000);
		} while (!dev->setting.no_retry && cnt++ < retry);
		break;
	} while (!!dev->setting.no_retry && cnt++ < retry);

	TP_ERR(dev->id, "M2V current mode: 0x" PFMT_X8 ", change to " PFMT_C8 " mode failed\n",
		mode, (to_bl) ? "BL" : "AP");
	return -EFAULT;

success_change_mode:
	TP_MSG(dev->id, "M2V current mode: 0x" PFMT_X8 " " PFMT_C8 " mode\n",
		mode, (to_bl) ? "BL" : "AP");

	return 0;
}

int api_get_ic_crc(void *handle, u8 final_fw_mode)
{
	int error;
	u8 i;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	error = api_set_ctrl_mode(dev, mode_suspend, false, true);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_AP_CRC, NULL);
	if (error < 0)
		goto err_set_normal;

	if (dev->ic[0].mode != AP_MODE)
		return 0;

	switch (dev->protocol.flag) {
	case PTL_V3:
		error = api_protocol_set_cmd(dev, GET_DF_CRC, NULL);
		if (error < 0)
			goto err_set_normal;
		break;
	case PTL_V6:
		for (i = 1; i < dev->tp_info.block_num; i++) {
			dev->ic[0].crc[i] = api_get_block_crc_by_num(dev,
				CRC_CALCULATE, i);
		}
		break;
	default:
		error = -EINVAL; break;
	}

err_set_normal:
	api_set_ctrl_mode(dev, final_fw_mode, false, true);

	return error;
}

void api_print_ts_info(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 i;

	TP_INFO(dev->id, "[Protocol Version]\n");
	TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X\n",
		(dev->protocol.ver >> 16) & 0xff,
		(dev->protocol.ver >> 8) & 0xff,
		dev->protocol.ver & 0xff);

	TP_INFO(dev->id, "[Firmware Version]\n");
	TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X\n",
		dev->fw_ver[0], dev->fw_ver[1],
		dev->fw_ver[2], dev->fw_ver[3]);
	TP_INFO(dev->id, "[Customer Version]\n");
	TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X\n",
		dev->fw_ver[4], dev->fw_ver[5],
		dev->fw_ver[6], dev->fw_ver[7]);

	TP_INFO(dev->id, "[Kernel Version]\n");
	if (dev->protocol.flag == PTL_V6 &&
	    support_mcu_info(dev)) {
		TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X.0x%02X (" PFMT_C8 ")\n",
			dev->mcu_info.ic_name[0], dev->mcu_info.ic_name[1],
			dev->mcu_info.ic_name[2], dev->mcu_info.ic_name[3],
			dev->mcu_info.ic_name[4], dev->mcu_info.ic_name);
	} else {
		TP_INFO(dev->id, "0x%c%c.0x%c%c\n",
			dev->mcu_info.ic_name[2], dev->mcu_info.ic_name[3],
			dev->mcu_info.ic_name[0], dev->mcu_info.ic_name[1]);
	}

	TP_INFO(dev->id, "[Current Mode]\n");
	TP_INFO(dev->id, "Master : " PFMT_C8 "\n", dev->ic[0].mode_str);

	if (dev->ic[0].mode != AP_MODE)
		return;

	for (i = 1; i < dev->tp_info.ic_num; i++)
		TP_INFO(dev->id, "Slave : " PFMT_C8 "\n", dev->ic[i].mode_str);

	TP_INFO(dev->id, "[Module Name]\n");
	TP_INFO(dev->id, PFMT_C8 "\n", dev->mcu_info.module_name);

	TP_INFO(dev->id, "[Core Version]\n");
	TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X\n",
		dev->core_ver[0], dev->core_ver[1],
		dev->core_ver[2], dev->core_ver[3]);

	if (is_231x(dev)) {
		switch (dev->core_ver[1]) {
		case 0x00:
			TP_INFO(dev->id, "(Dual Interface)\n"); break;
		case 0x01:
			TP_INFO(dev->id, "(USB Interface)\n"); break;
		case 0x02:
			TP_INFO(dev->id, "(I2C Interface)\n"); break;
		}
	}

	if (dev->protocol.flag == PTL_V6) {
		TP_INFO(dev->id, "[Tuning Version]\n");
		TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X\n",
			dev->tuning_ver[0], dev->tuning_ver[1],
			dev->tuning_ver[2], dev->tuning_ver[3]);
	}

	TP_INFO(dev->id, "[Panel Information]\n");
	TP_INFO(dev->id, "X resolution : " PFMT_U16 "\n", dev->tp_info.x_resolution);
	TP_INFO(dev->id, "Y resolution : " PFMT_U16 "\n", dev->tp_info.y_resolution);
	TP_INFO(dev->id, "AA X Channel : " PFMT_U16 "\n", dev->tp_info.x_ch);
	TP_INFO(dev->id, "AA Y Channel : " PFMT_U16 "\n", dev->tp_info.y_ch);
	TP_INFO(dev->id, "Support " PFMT_U8 " Fingers\n", dev->tp_info.max_fingers);
	TP_INFO(dev->id, "Support " PFMT_U8 " Touch Keys\n", dev->tp_info.key_num);

	if (dev->tp_info.key_num) {
		switch (dev->key.info.mode) {
		case key_disable:
			TP_INFO(dev->id, "Key Mode : NO_Key\n");
			break;
		case key_hw:
			TP_INFO(dev->id, "Key Mode : HW_Key_1\n");
			break;
		case key_hsw:
			TP_INFO(dev->id, "Key Mode : HW_Key_2\n");
			break;
		case key_vitual:
			TP_INFO(dev->id, "Key Mode : Virtual_Key\n");
			break;
		case key_fw_disable:
			TP_INFO(dev->id, "Key Mode : FW_disable\n");
			break;
		default:
			TP_INFO(dev->id, "Key Mode : Unknown(0x" PFMT_X8 ")\n",
				dev->key.info.mode);
			break;
		}
	}

	if (dev->protocol.flag == PTL_V6) {
		TP_INFO(dev->id, "Support Pen Type : " PFMT_C8 "\n",
			dev->pen_mode);
		TP_INFO(dev->id, "Chip Counts : " PFMT_U8 "\n", dev->tp_info.ic_num);
		TP_INFO(dev->id, "Report Format : " PFMT_U8 "\n", dev->tp_info.format);
		TP_INFO(dev->id, "Block Number : " PFMT_U8 "\n", dev->tp_info.block_num);
	}

	if (dev->protocol.flag == PTL_V6) {
		TP_INFO(dev->id, "[FW CRC]\n");
		TP_INFO(dev->id, "Master : 0x%04X\n", dev->ic[0].crc[0]);
		for (i = 1; i < dev->tp_info.ic_num; i++)
			TP_INFO(dev->id, "Slave : 0x%04X\n", dev->ic[i].crc[0]);
	} else {
		TP_INFO(dev->id, "[Check Code]\n");
		TP_INFO(dev->id, "AP : 0x%08X\n", dev->ic[0].crc[0]);
		TP_INFO(dev->id, "DATA : 0x%08X\n", dev->ic[0].crc[1]);
	}
}

void api_read_then_print_m2v_info(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;
	u8 m2v_mode;
	u32 m2v_checksum;
	u8 m2v_fw_ver[8];

	if (dev->ic[0].mode != AP_MODE)
		return;

	api_set_ctrl_mode(dev, mode_suspend, false, true);
	api_access_slave(dev, 0x80, CMD_GET_FW_VER, m2v_fw_ver);
	api_access_slave(dev, 0x80, CMD_GET_MCU_MOD, &m2v_mode);
	api_access_slave(dev, 0x80, CMD_GET_AP_CRC, &m2v_checksum);
	api_set_ctrl_mode(dev, mode_normal, false, true);

	TP_INFO(dev->id, "[M2V Firmware Version]\n");
	TP_INFO(dev->id, "0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X.0x%02X\n",
		m2v_fw_ver[0], m2v_fw_ver[1], m2v_fw_ver[2], m2v_fw_ver[3],
		m2v_fw_ver[4], m2v_fw_ver[5], m2v_fw_ver[6], m2v_fw_ver[7]);

	TP_INFO(dev->id, "[M2V Current Mode]\n");
	TP_INFO(dev->id, "Mode : " PFMT_C8 " Mode\n",
		(m2v_mode == AP_MODE) ? "AP" : "BL");

	TP_INFO(dev->id, "[M2V FW CheckSum]\n");
	TP_INFO(dev->id, "FW CheckSum : 0x%08X\n", m2v_checksum);
}

int api_update_ts_info(void *handle)
{
	int error;
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	/* set protocol default V6 initially for comms. afterwards */
	dev->protocol.flag = PTL_V6;
	dev->tp_info.ic_num = 1;

	error = api_set_ctrl_mode(dev, mode_suspend, false, true);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_PTL_VER, NULL);
	if (error < 0)
		goto err_set_normal;

	/*
	 * previous set suspend mode is in V6 format.
	 * set suspend mode again if device protocol is checked as V3.
	 */
	if (dev->protocol.flag == PTL_V3) {
		error = api_set_ctrl_mode(dev, mode_suspend, false, true);
		if (error < 0)
			goto err_set_normal;
	}

	error = api_protocol_set_cmd(dev, GET_MCU_MOD, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_MCU_VER, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_FW_VER, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_AP_CRC, NULL);
	if (error < 0)
		goto err_set_normal;

	if (dev->protocol.flag == PTL_V6) {
		error = api_protocol_set_cmd(dev, GET_PRODUCT_INFO, NULL);
		if (error < 0)
			goto err_set_normal;
		error = api_protocol_set_cmd(dev, GET_FWID, NULL);
		if (error < 0)
			goto err_set_normal;
		error = api_protocol_set_cmd(dev, GET_SENSOR_ID, NULL);
		if (error < 0)
			goto err_set_normal;
		error = api_protocol_set_cmd(dev, GET_HID_INFO, NULL);
		if (error < 0)
			goto err_set_normal;
	}

	/* BL mode should perform FW upgrade afterward */
	if (dev->ic[0].mode != AP_MODE)
		return 0;

	/* V3 need to get DF CRC */
	if (dev->protocol.flag == PTL_V3) {
		error = api_protocol_set_cmd(dev, GET_DF_CRC, NULL);
		if (error < 0)
			goto err_set_normal;
	}

	if (dev->protocol.flag == PTL_V6) {
		error = api_protocol_set_cmd(dev, GET_TUNING_VER, NULL);
		if (error < 0)
			goto err_set_normal;
	}

	error = api_protocol_set_cmd(dev, GET_CORE_VER, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_SCRN_RES, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_protocol_set_cmd(dev, GET_TP_INFO, NULL);
	if (error < 0)
		goto err_set_normal;
	error = api_get_func_mode(dev);
	if (error < 0)
		goto err_set_normal;

	if (dev->tp_info.ic_num > 1) {
		error = api_protocol_set_cmd(dev, GET_AP_CRC,
					     &dev->tp_info.ic_num);
		if (error < 0)
			goto err_set_normal;
		error = api_protocol_set_cmd(dev, GET_MCU_MOD,
					     &dev->tp_info.ic_num);
		if (error < 0)
			goto err_set_normal;
	}

err_set_normal:
	api_set_ctrl_mode(dev, mode_normal, false, true);

	return error;
}

void __ilitek_get_info(void *handle, struct ilitek_common_info *info)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!info || !dev)
		return;

	_memcpy(info, &dev->quirks, sizeof(struct ilitek_common_info));
}

void ilitek_dev_set_quirks(void *handle, u32 quirks)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!handle)
		return;

	dev->quirks = quirks;
}

void ilitek_dev_set_sys_info(void *handle, struct ilitek_sys_info *sys)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (!handle)
		return;

	_memcpy(&dev->sys, sys, sizeof(struct ilitek_sys_info));
}

void ilitek_dev_setting(void *handle, struct ilitek_ts_settings *setting)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

#define X(_enum, _id, _size, _cnt)	\
	touch_fmts[_id].size = _size;	\
	touch_fmts[_id].max_cnt = _cnt;

	ILITEK_TOUCH_REPORT_FORMAT;
#undef X

	if (!handle)
		return;

	_memcpy(&dev->setting, setting, sizeof(struct ilitek_ts_settings));

	if (dev->setting.default_format_enabled) {
		dev->fmt.touch_size = touch_fmts[touch_fmt_0x0].size;
		dev->fmt.touch_max_cnt = touch_fmts[touch_fmt_0x0].max_cnt;
	}

	TP_MSG(dev->id, "no-retry: %d, no-INT-ack: %d\n",
		dev->setting.no_retry, dev->setting.no_INT_ack);
}

void ilitek_dev_bind_callback(void *handle, struct ilitek_ts_callback *callback)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	if (callback) {
		_memcpy(&dev->cb, callback, sizeof(struct ilitek_ts_callback));
		if (dev->cb.msg)
			g_msg = dev->cb.msg;
	}
}

void *ilitek_dev_init(u8 _interface, const char *id,
		      bool need_update_ts_info,
		      struct ilitek_ts_callback *callback, void *_private)
{
	struct ilitek_ts_device *dev;

	dev = (struct ilitek_ts_device *)MALLOC(sizeof(*dev));
	if (!dev)
		return NULL;

	TP_MSG(NULL, "commonflow code version: 0x%x\n",
		COMMONFLOW_CODE_VERSION);

	TP_DBG(NULL, "sizeof(ilitek_ts_device): %u\n",
		(unsigned int)sizeof(struct ilitek_ts_device));

	/* initial all member to 0/ false/ NULL */
	_memset(dev, 0, sizeof(*dev));

	_strcpy(dev->id, id, sizeof(dev->id));
	ilitek_dev_bind_callback(dev, callback);

	dev->_interface = _interface;
	dev->_private = _private;

	/* set protocol default V6 initially for comms. afterwards */
	dev->protocol.flag = PTL_V6;
	dev->tp_info.ic_num = 1;

	dev->fw_mode = mode_unknown;

	if (need_update_ts_info && api_update_ts_info(dev) < 0) {
		ilitek_dev_exit(dev);
		return NULL;
	}

	return dev;
}

void ilitek_dev_exit(void *handle)
{
	struct ilitek_ts_device *dev = (struct ilitek_ts_device *)handle;

	/*
	 * LIBUSB would kill /dev/hidraw* and make system stop handling
	 * device's usb. sw reset is required to re-enum usb then /dev/hidraw*
	 * would be created and system would start to handle touch event.
	 */
	if (dev->quirks & QUIRK_LIBUSB || dev->setting.sw_reset_at_last)
		api_protocol_set_cmd(dev, SET_SW_RST, NULL);

	if (dev)
		FREE(dev);
}
