// SPDX-License-Identifier: GPL-2.0
/*  Himax hx83102j Driver Code for Common IC to simulate HID
 *
 *  Copyright (C) 2023 Himax Corporation.
 */

#include "hid-himax-83102j.h"

static void hx83102j_pin_reset(struct himax_ts_data *ts);
static void himax_ts_work(struct himax_ts_data *ts);
static int himax_resume(struct device *dev);
static int himax_suspend(struct device *dev);
static int himax_chip_init(struct himax_ts_data *ts);
static bool hx83102j_sense_off(struct himax_ts_data *ts, bool check_en);

static int himax_spi_read(struct himax_ts_data *ts, u8 *cmd,
			  u8 cmd_len, u8 *buf, u32 len)
{
	struct spi_message m;
	int result = 0;
	int retry;
	int error;
	struct spi_transfer	t = {
		.len = cmd_len + len,
	};

	t.tx_buf = ts->xfer_data;
	t.rx_buf = ts->xfer_data;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	for (retry = 0; retry < HIMAX_BUS_RETRY_TIMES; retry++) {
		error = spi_sync(ts->spi, &m);
		if (!unlikely(error))
			break;
	}

	if (retry == HIMAX_BUS_RETRY_TIMES) {
		dev_err(ts->dev, "SPI read error retry over %d", HIMAX_BUS_RETRY_TIMES);
		result = -EIO;
		goto err_retry_over;
	} else {
		memcpy(buf, ts->xfer_data + cmd_len, len);
	}

err_retry_over:
	return result;
}

static int himax_spi_write(struct himax_ts_data *ts, u8 *buf,
			   u32 length)
{
	int status;
	struct spi_message	m;
	struct spi_transfer	t = {
			.tx_buf		= buf,
			.len		= length,
	};

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	status = spi_sync(ts->spi, &m);

	if (status == 0) {
		status = m.status;
		if (status == 0)
			status = m.actual_length;
	}

	return status;
}

static int himax_bus_read(struct himax_ts_data *ts, u8 cmd,
		   u8 *buf, u32 len)
{
	int result = -1;
	u8 hw_addr = 0x00;

	if (len > HIMAX_BUS_R_DLEN) {
		dev_err(ts->dev, "len[%d] is over %d", len, HIMAX_BUS_R_DLEN);
		return result;
	}

	mutex_lock(&ts->rw_lock);

	hw_addr = 0xF3;

	memset(ts->xfer_data, 0, HIMAX_BUS_R_HLEN + len);
	ts->xfer_data[0] = hw_addr;
	ts->xfer_data[1] = cmd;
	ts->xfer_data[2] = 0x00;
	result = himax_spi_read(ts, ts->xfer_data, HIMAX_BUS_R_HLEN, buf, len);

	mutex_unlock(&ts->rw_lock);

	return result;
}

static int himax_bus_write(struct himax_ts_data *ts, u8 cmd,
		    u8 *addr, u8 *data, u32 len)
{
	int result = -1;
	u8 offset = 0;
	u32 tmp_len = len;
	u8 hw_addr = 0x00;

	if (len > HIMAX_BUS_W_DLEN) {
		dev_err(ts->dev, "len[%d] is over %d", len, HIMAX_BUS_W_DLEN);
		return -EFAULT;
	}

	mutex_lock(&ts->rw_lock);

	hw_addr = 0xF2;

	ts->xfer_data[0] = hw_addr;
	ts->xfer_data[1] = cmd;
	offset = HIMAX_BUS_W_HLEN;

	if (addr) {
		memcpy(ts->xfer_data + offset, addr, 4);
		offset += 4;
		tmp_len -= 4;
	}

	if (data)
		memcpy(ts->xfer_data + offset, data, tmp_len);

	result = himax_spi_write(ts, ts->xfer_data, len + HIMAX_BUS_W_HLEN);

	mutex_unlock(&ts->rw_lock);

	return (result == len + HIMAX_BUS_W_HLEN) ? 0 : -EIO;
}

static void himax_int_enable(struct himax_ts_data *ts, int enable)
{
	unsigned long irqflags = 0;
	int irqnum = ts->himax_irq;

	spin_lock_irqsave(&ts->irq_lock, irqflags);
	if (enable == 1 && atomic_read(&ts->irq_state) == 0) {
		atomic_set(&ts->irq_state, 1);
		enable_irq(irqnum);
		ts->irq_enabled = 1;
	} else if (enable == 0 && atomic_read(&ts->irq_state) == 1) {
		atomic_set(&ts->irq_state, 0);
		disable_irq_nosync(irqnum);
		ts->irq_enabled = 0;
	}

	spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

static void himax_ts_isr_func(struct himax_ts_data *ts)
{
	himax_ts_work(ts);
}

static irqreturn_t himax_ts_thread(int irq, void *ptr)
{
	himax_ts_isr_func((struct himax_ts_data *)ptr);

	return IRQ_HANDLED;
}

static int himax_int_register_trigger(struct himax_ts_data *ts)
{
	int ret = 0;

	if (ts->ic_data->HX_INT_IS_EDGE) {
		ret = devm_request_threaded_irq(ts->dev, ts->himax_irq, NULL,
					  himax_ts_thread, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					  ts->dev->driver->name, ts);
	} else {
		ret = devm_request_threaded_irq(ts->dev, ts->himax_irq, NULL,
					  himax_ts_thread, IRQF_TRIGGER_LOW | IRQF_ONESHOT,
					  ts->dev->driver->name, ts);
	}

	return ret;
}

static int himax_ts_register_interrupt(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->irq_enabled = 0;

	if (ts->himax_irq) {

		ret = himax_int_register_trigger(ts);

		if (ret == 0) {
			ts->irq_enabled = 1;
			atomic_set(&ts->irq_state, 1);
		} else {

			dev_err(ts->dev, "request_irq failed");
		}
	}

	return ret;
}

static void himax_mcu_burst_enable(struct himax_ts_data *ts,
				   u8 auto_add_4_byte)
{
	u8 tmp_data[HIMAX_REG_DATA_LEN];
	int ret;

	tmp_data[0] = HIMAX_IC_CMD_CONTI;

	ret = himax_bus_write(ts, HIMAX_IC_ADR_CONTI, NULL, tmp_data, 1);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		return;
	}

	tmp_data[0] = (HIMAX_IC_CMD_INCR4 | auto_add_4_byte);

	ret = himax_bus_write(ts, HIMAX_IC_ADR_INCR4, NULL, tmp_data, 1);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		return;
	}
}
static int himax_mcu_register_read(struct himax_ts_data *ts, u32 addr,
				   u8 *buf, u32 len)
{
	int ret = 0;
	union {
		u8 byte[4];
		u32 word;
	} target_addr = { .word = cpu_to_le32(addr) };
	u8 direction_switch = HIMAX_IC_CMD_AHB_ACCESS_DIRECTION_READ;

	mutex_lock(&ts->reg_lock);

	if (addr == HIMAX_FLASH_ADDR_SPI200_DATA)
		himax_mcu_burst_enable(ts, 0);
	else if (len > HIMAX_REG_DATA_LEN)
		himax_mcu_burst_enable(ts, 1);
	else
		himax_mcu_burst_enable(ts, 0);

	ret = himax_bus_write(ts, HIMAX_IC_ADR_AHB_ADDR_BYTE_0, target_addr.byte, NULL, 4);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		goto read_end;
	}

	ret = himax_bus_write(ts, HIMAX_IC_ADR_AHB_ACCESS_DIRECTION, NULL,
			      &direction_switch, 1);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		goto read_end;
	}

	ret = himax_bus_read(ts, HIMAX_IC_ADR_AHB_RDATA_BYTE_0, buf, len);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		goto read_end;
	}

read_end:
	mutex_unlock(&ts->reg_lock);

	return ret;
}

static int himax_mcu_register_write(struct himax_ts_data *ts, u32 addr,
				    u8 *val, u32 len)
{
	int ret = 0;
	const u32 max_trans_sz = 4 * 1024;
	int i = 0;
	union {
		u8 byte[4];
		u16 half[2];
		u32 word;
	} target_addr;
	u32 temp_len = 0;

	mutex_lock(&ts->reg_lock);
	if (addr == HIMAX_FLASH_ADDR_SPI200_DATA)
		himax_mcu_burst_enable(ts, 0);
	else if (len > HIMAX_REG_DATA_LEN)
		himax_mcu_burst_enable(ts, 1);
	else
		himax_mcu_burst_enable(ts, 0);

	if (len > max_trans_sz) {
		for (i = 0; i < len; i += max_trans_sz) {
			if ((len - i) > max_trans_sz)
				temp_len = max_trans_sz;
			else
				temp_len = len % max_trans_sz;

			target_addr.word = cpu_to_le32(addr + i);
			ret = himax_bus_write(ts, HIMAX_IC_ADR_AHB_ADDR_BYTE_0,
					      target_addr.byte, val + i, temp_len + HIMAX_REG_ADDR_LEN);
			if (ret < 0) {
				dev_err(ts->dev, "xfer fail!");
				goto write_end;
			}
		}
	} else {
		target_addr.word = cpu_to_le32(addr);
		ret = himax_bus_write(ts, HIMAX_IC_ADR_AHB_ADDR_BYTE_0, target_addr.byte, val,
				      len + HIMAX_REG_ADDR_LEN);
		if (ret < 0) {
			dev_err(ts->dev, "xfer fail!");
			goto write_end;
		}
	}
write_end:
	mutex_unlock(&ts->reg_lock);

	return ret;
}


static void himax_ap_notify_fw_sus(struct himax_ts_data *ts, int suspend)
{
	int retry = 0;
	int read_sts = 0;
	union {
		u8 byte[4];
		u16 half[2];
		u32 word;
	} rdata, data;

	if (suspend)
		data.word = HIMAX_FW_DATA_AP_NOTIFY_FW_SUS_EN;
	else
		data.word = HIMAX_FW_DATA_AP_NOTIFY_FW_SUS_DIS;

	data.word = cpu_to_le32(data.word);
	do {
		himax_mcu_register_write(ts, HIMAX_FW_ADDR_AP_NOTIFY_FW_SUS, data.byte,
			4);
		usleep_range(1000, 1001);
		read_sts = himax_mcu_register_read(ts, HIMAX_FW_ADDR_AP_NOTIFY_FW_SUS, rdata.byte,
			4);
	} while ((retry++ < 10) && (read_sts != 0) &&
		(rdata.word != data.word));
}

static void himax_resume_proc(struct himax_ts_data *ts, bool suspended)
{
		himax_ap_notify_fw_sus(ts, 0);
}

static void himax_mcu_ic_reset(struct himax_ts_data *ts, u8 loadconfig,
			       u8 int_off)
{

	if (ts->gpiod_rst) {
		if (int_off)
			himax_int_enable(ts, 0);

		hx83102j_pin_reset(ts);

		if (int_off)
			himax_int_enable(ts, 1);
	}
}
static void himax_mcu_touch_information(struct himax_ts_data *ts)
{
	if (ts->ic_data->HX_RX_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_RX_NUM = 48;

	if (ts->ic_data->HX_TX_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_TX_NUM = 32;

	if (ts->ic_data->HX_BT_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_BT_NUM = 0;

	if (ts->ic_data->HX_MAX_PT == 0xFFFFFFFF)
		ts->ic_data->HX_MAX_PT = 10;

	if (ts->ic_data->HX_INT_IS_EDGE == 0xFF)
		ts->ic_data->HX_INT_IS_EDGE = false;

	if (ts->ic_data->HX_STYLUS_FUNC == 0xFF)
		ts->ic_data->HX_STYLUS_FUNC = 1;

	if (ts->ic_data->HX_STYLUS_ID_V2 == 0xFF)
		ts->ic_data->HX_STYLUS_ID_V2 = 0;

	if (ts->ic_data->HX_STYLUS_RATIO == 0xFF)
		ts->ic_data->HX_STYLUS_RATIO = 1;

}
static bool hx83102j_chip_detect(struct himax_ts_data *ts)
{
	union {
		u8 byte[4];
		u16 half[2];
		u32 word;
	} data;
	bool ret_data = false;
	int ret = 0;
	int i = 0;
	bool check_flash;

	hx83102j_pin_reset(ts);
	ret = himax_bus_read(ts, 0x13, data.byte, 1);
	if (ret < 0) {
		dev_err(ts->dev, "bus access fail!");
		return false;
	}

	check_flash = false;

	if (hx83102j_sense_off(ts, check_flash) == false) {
		ret_data = false;
		dev_err(ts->dev, "hx83102_sense_off Fail!");
		return ret_data;
	}

	for (i = 0; i < 5; i++) {
		ret = himax_mcu_register_read(ts, HIMAX_HX83102J_ICID_ADDR, data.byte, 4);
		if (ret != 0) {
			ret_data = false;
			dev_err(ts->dev, "read ic id Fail");
			return ret_data;
		}

		if ((data.word & 0xFFFFFF00) == HIMAX_HX83102J_ICID_DATA) {
			strscpy(ts->chip_name,
				HIMAX_HX83102J_ID, 30);
			ts->ic_data->icid = data.word;
			ret_data = true;
			break;
		}
		dev_err(ts->dev, "Read driver IC ID = %X,%X,%X",
		  data.byte[3], data.byte[2], data.byte[1]);
		ret_data = false;
		dev_err(ts->dev, "Read driver ID register Fail!");
		dev_err(ts->dev, "Could NOT find Himax Chipset");
		dev_err(ts->dev, "Please check:\n1.VCCD,VCCA,VSP,VSN");
		dev_err(ts->dev, "2. LCM_RST,TP_RST");
		dev_err(ts->dev, "3. Power On Sequence");
	}

	return ret_data;
}
static bool hx83102j_sense_off(struct himax_ts_data *ts, bool check_en)
{
	u32 cnt = 0;
	union {
		u8 byte[4];
		u16 half[2];
		u32 word;
	} data;
	int ret = 0;

	do {
		data.word = cpu_to_le32(HIMAX_FW_DATA_FW_STOP);
		if (cnt == 0 ||
		    (data.byte[0] != 0xA5 &&
		    data.byte[0] != 0x00 &&
		    data.byte[0] != 0x87))
			himax_mcu_register_write(ts, HIMAX_FW_ADDR_CTRL_FW,
				data.byte, 4);
		usleep_range(10000, 10001);

		himax_mcu_register_read(ts, HIMAX_IC_ADR_CS_CENTRAL_STATE,
			data.byte, 4);

		if (data.byte[0] != 0x05)
			break;

		himax_mcu_register_read(ts, HIMAX_FW_ADDR_CTRL_FW,
			data.byte, 4);
	} while (data.byte[0] != 0x87 && ++cnt < 35 && check_en);

	cnt = 0;

	do {
		/**
		 * set Enter safe mode : 0x31 ==> 0x9527
		 */
		data.half[0] = cpu_to_le16(HIMAX_HX83102J_SAFE_MODE_PASSWORD);
		ret = himax_bus_write(ts, 0x31, NULL, data.byte, 2);
		if (ret < 0) {
			dev_err(ts->dev, "bus access fail!");
			return false;
		}

		/**
		 *Check enter_save_mode
		 */
		himax_mcu_register_read(ts, HIMAX_IC_ADR_CS_CENTRAL_STATE, data.byte, 4);

		if (data.byte[0] == 0x0C) {
			/**
			 *Reset TCON
			 */
			data.word = 0;
			himax_mcu_register_write(ts, HIMAX_HX83102J_IC_ADR_TCON_RST, data.byte, 4);
			usleep_range(1000, 1001);
			return true;
		}
		usleep_range(5000, 5001);
		hx83102j_pin_reset(ts);
	} while (cnt++ < 5);

	return false;
}

static bool hx83102j_read_event_stack(struct himax_ts_data *ts,
				      u8 *buf, u32 length)
{
	int ret = 0;

	ret = himax_bus_read(ts, HIMAX_FW_ADDR_EVENT_ADDR, buf, length);

	return (ret == 0) ? true : false;
}

static void hx83102j_pin_reset(struct himax_ts_data *ts)
{
	if (ts->gpiod_rst) {
		gpiod_set_value(ts->gpiod_rst, 1);
		usleep_range(100 * 100, 101 * 100);
		gpiod_set_value(ts->gpiod_rst, 0);
		usleep_range(200 * 100, 201 * 100);
	}
}

static int himax_touch_get(struct himax_ts_data *ts, u8 *buf, int ts_path)
{
	u32 read_size = 0;
	int ts_status = 0;

	switch (ts_path) {
	case HIMAX_REPORT_COORD:
		read_size = ts->touch_all_size;
		break;
	default:
		break;
	}

	if (read_size == 0) {
		dev_err(ts->dev, "Read size fault!");
		ts_status = HIMAX_TS_GET_DATA_FAIL;
	} else {
		if (!hx83102j_read_event_stack(ts, buf, read_size)) {
			dev_err(ts->dev, "can't read data from chip!");
			ts_status = HIMAX_TS_GET_DATA_FAIL;
		}
	}

	return ts_status;
}

static int himax_hid_parse(struct hid_device *hid)
{
	struct himax_ts_data *ts = NULL;
	int ret;

	if (!hid) {
		dev_err(ts->dev, "hid is NULL");
		return -EINVAL;
	}

	ts = hid->driver_data;
	if (!ts) {
		dev_err(ts->dev, "hid->driver_data is NULL");
		return -EINVAL;
	}

	ret = hid_parse_report(hid, ts->hid_rd_data.rd_data,
			       ts->hid_rd_data.rd_length);
	if (ret) {
		dev_err(ts->dev, "failed parse report");
		return	ret;
	}
	return 0;
}

static int himax_hid_start(struct hid_device *hid)
{
	return 0;
}

static void himax_hid_stop(struct hid_device *hid)
{
}

static int himax_hid_open(struct hid_device *hid)
{
	return 0;
}

static void himax_hid_close(struct hid_device *hid)
{
}

static int himax_hid_get_raw_report(const struct hid_device *hid, unsigned char reportnum,
				 __u8 *buf, size_t len, unsigned char report_type)
{
	struct himax_ts_data *ts = NULL;
	int ret = 0;

	ts = hid->driver_data;
	if (!ts) {
		dev_err(ts->dev, "hid->driver_data is NULL");
		return -EINVAL;
	}


	switch (reportnum) {
	case ID_CONTACT_COUNT:
		if (!ts->ic_data) {
			dev_err(ts->dev, "ts->ic_data is NULL");
			return -EINVAL;
		}
		buf[1] = ts->ic_data->HX_MAX_PT;
		ret = len;
		break;
	default:
		ret = -EINVAL;
	};
	return ret;
}

static int himax_raw_request(struct hid_device *hid, unsigned char reportnum,
			  __u8 *buf, size_t len, unsigned char rtype, int reqtype)
{
	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		return himax_hid_get_raw_report(hid, reportnum, buf, len, rtype);
	default:
		return -EIO;
	}

	return -EINVAL;
}

static struct hid_ll_driver himax_hid_ll_driver = {
	.parse = himax_hid_parse,
	.start = himax_hid_start,
	.stop = himax_hid_stop,
	.open = himax_hid_open,
	.close = himax_hid_close,
	.raw_request = himax_raw_request
};

static int himax_hid_report(const struct himax_ts_data *ts, u8 *data, s32 len)
{
	int ret = 0;

	if (ts->hid)
		ret = hid_input_report(ts->hid, HID_INPUT_REPORT, data, len, 1);

	return ret;
}
static int himax_hid_probe(struct himax_ts_data *ts)
{
	int ret;
	struct hid_device *hid = NULL;

	if (!ts) {
		dev_err(ts->dev, "ts is NULL");
		return -EINVAL;
	}
	hid = ts->hid;
	if (hid) {
		hid_destroy_device(hid);
		hid = NULL;
	}

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		ret = PTR_ERR(hid);
		return ret;
	}

	hid->driver_data = ts;
	hid->ll_driver = &himax_hid_ll_driver;
	hid->bus = BUS_SPI;
	hid->dev.parent = &ts->spi->dev;

	hid->version = ts->hid_desc.bcd_version;
	hid->vendor = ts->hid_desc.vendor_id;
	hid->product = ts->hid_desc.product_id;
	snprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", "hid-hxtp",
		 hid->vendor, hid->product);

	ret = hid_add_device(hid);
	if (ret) {
		dev_err(ts->dev, "failed add hid device");
		goto err_hid_data;
	}
	ts->hid = hid;
	mutex_unlock(&ts->hid_ioctl_lock);
	return 0;

err_hid_data:
	hid_destroy_device(hid);
	return ret;
}

static void himax_hid_remove(struct himax_ts_data *ts)
{
	mutex_lock(&ts->hid_ioctl_lock);
	if (ts && ts->hid)
		hid_destroy_device(ts->hid);
	else
		goto out;
	ts->hid = NULL;
out:
	mutex_unlock(&ts->hid_ioctl_lock);
}


static int himax_ts_operation(struct himax_ts_data *ts,
			      int ts_path)
{
	int ts_status = HIMAX_TS_NORMAL_END;
	int ret = 0;
	u32 offset = 0;

	memset(ts->xfer_buff,
	       0x00,
		ts->touch_all_size * sizeof(u8));
	ts_status = himax_touch_get(ts, ts->xfer_buff, ts_path);
	if (ts_status == HIMAX_TS_GET_DATA_FAIL)
		goto end_function;
	if (ts->hid_probe) {
		offset += ts->hid_desc.max_input_length;
		if (ts->ic_data->HX_STYLUS_FUNC) {
			ret += himax_hid_report(ts,
				ts->xfer_buff + offset + HIMAX_HID_REPORT_HDR_SZ,
				ts->hid_desc.max_input_length - HIMAX_HID_REPORT_HDR_SZ);
			offset += ts->hid_desc.max_input_length;
		}
	}

	if (ret != 0)
		ts_status = HIMAX_TS_GET_DATA_FAIL;

end_function:
	return ts_status;
}
static void himax_ts_work(struct himax_ts_data *ts)
{
	int ts_status = HIMAX_TS_NORMAL_END;
	int ts_path = 0;


	ts_path = HIMAX_REPORT_COORD;
	ts_status = himax_ts_operation(ts, ts_path);
	if (ts_status == HIMAX_TS_GET_DATA_FAIL)
		himax_mcu_ic_reset(ts, false, true);

}

static int himax_hid_rd_init(struct himax_ts_data *ts)
{
	int ret = 0;
	u32 rd_sz = 0;

	rd_sz = ts->hid_desc.report_desc_length;
	if (ts->flash_ver_info.addr_hid_rd_desc != 0) {
		if (ts->hid_rd_data.rd_data &&
		    rd_sz != ts->hid_rd_data.rd_length) {
			kfree(ts->hid_rd_data.rd_data);
			ts->hid_rd_data.rd_data = NULL;
		}

		if (!ts->hid_rd_data.rd_data)
			ts->hid_rd_data.rd_data = kzalloc(rd_sz, GFP_KERNEL);

		if (ts->hid_rd_data.rd_data) {
		} else {
			dev_err(ts->dev, "hid rd data alloc fail");
			ret = -ENOMEM;
		}
	}

	return ret;
}

static void himax_hid_register(struct himax_ts_data *ts)
{
	if (ts->hid_probe) {
		hid_destroy_device(ts->hid);
		ts->hid = NULL;
		ts->hid_probe = false;
	}

	if (himax_hid_probe(ts) != 0) {
		dev_err(ts->dev, "hid probe fail");
		ts->hid_probe = false;
	} else {
		ts->hid_probe = true;
	}
}

static int himax_hid_report_data_init(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->touch_info_size = ts->hid_desc.max_input_length;
	if (ts->ic_data->HX_STYLUS_FUNC)
		ts->touch_info_size += ts->hid_desc.max_input_length;

	ts->touch_all_size = ts->touch_info_size;
	return ret;
}

static void himax_hid_update(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_hid_update.work);

	himax_int_enable(ts, false);
	if (himax_hid_rd_init(ts) == 0) {
		himax_hid_register(ts);
		if (ts->hid_probe)
			himax_hid_report_data_init(ts);
	}
	himax_int_enable(ts, true);
}

static int himax_chip_suspend(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->suspended = true;
	himax_int_enable(ts, false);
	if (ts->gpiod_rst)
		gpiod_set_value(ts->gpiod_rst, 1);
	himax_hid_remove(ts);
	return ret;
}

static int himax_chip_resume(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->suspended = false;
	if (ts->gpiod_rst)
		gpiod_set_value(ts->gpiod_rst, 0);
	himax_resume_proc(ts, ts->suspended);
		himax_hid_probe(ts);
		himax_int_enable(ts, true);
	return ret;
}

static int himax_suspend(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	if (!ts->initialized) {
		dev_err(ts->dev, "init not ready, skip!");
		return -ECANCELED;
	}
	himax_chip_suspend(ts);
	return 0;
}

static void himax_shutdown(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	if (!ts->initialized) {
		dev_err(ts->dev, "init not ready, skip!");
		return;
	}

	himax_int_enable(ts, false);
	himax_hid_remove(ts);
}

static int himax_resume(struct device *dev)
{
	int ret = 0;
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	if (!ts->initialized) {
		if (himax_chip_init(ts))
			return -ECANCELED;
	}
	ret = himax_chip_resume(ts);
	if (ret < 0)
		dev_err(ts->dev, "resume failed!");
	return ret;
}
static const struct dev_pm_ops himax_hid_pm = {
	.suspend = himax_suspend,
	.resume = himax_resume,
	.restore = himax_resume,
};

#if defined(CONFIG_OF)
static const struct of_device_id himax_table[] = {
	{ .compatible = "himax,hx83102j" },
	{},
};
MODULE_DEVICE_TABLE(of, himax_table);
#endif

static int himax_chip_init(struct himax_ts_data *ts)
{
	himax_mcu_touch_information(ts);
	spin_lock_init(&ts->irq_lock);
	if (himax_ts_register_interrupt(ts)) {
		dev_err(ts->dev, "register interrupt failed");
		return -EIO;
	}
	himax_int_enable(ts, false);
	INIT_DELAYED_WORK(&ts->work_hid_update, himax_hid_update);
	ts->suspended = false;
	ts->initialized = true;
	return 0;

}
static bool himax_platform_init(struct himax_ts_data *ts,
				struct himax_platform_data *local_pdata)
{
	struct himax_platform_data *pdata;

	ts->xfer_buff = devm_kzalloc(ts->dev, HIMAX_FULL_STACK_SIZE, GFP_KERNEL);
	if (!ts->xfer_buff)
		return false;

	pdata = devm_kzalloc(ts->dev, sizeof(struct himax_platform_data), GFP_KERNEL);
	if (!pdata)
		return false;


	ts->ic_data = devm_kzalloc(ts->dev, sizeof(struct himax_ic_data), GFP_KERNEL);
	if (!ts->ic_data)
		return false;

	memset(ts->ic_data, 0xFF, sizeof(struct himax_ic_data));
	memcpy(pdata, local_pdata, sizeof(struct himax_platform_data));
	ts->pdata = pdata;
	pdata->ts = ts;
	ts->gpiod_rst = pdata->gpiod_rst;
	if (pdata->gpiod_rst)
		gpiod_set_value(pdata->gpiod_rst, 1);
	if (pdata->gpiod_rst)
		gpiod_set_value(pdata->gpiod_rst, 0);

	return true;
}

static int himax_spi_drv_probe(struct spi_device *spi)
{
	struct himax_ts_data *ts = NULL;
	int ret = 0;
	bool bret = false;
	static struct himax_platform_data pdata = {0};

	ts = devm_kzalloc(&spi->dev, sizeof(struct himax_ts_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = &spi->dev;
	if (spi->master->flags & SPI_MASTER_HALF_DUPLEX) {
		dev_err(ts->dev, "Full duplex not supported by host");
		return -EIO;
	}
	pdata.ts = ts;
	ts->dev = &spi->dev;
	if (!spi->irq) {
		dev_dbg(ts->dev, "no IRQ?\n");
		return -EINVAL;
	}
	ts->himax_irq = spi->irq;
	pdata.gpiod_rst = devm_gpiod_get(ts->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pdata.gpiod_rst)) {
		dev_err(ts->dev, "gpio-rst value is not valid");
		return -EIO;
	}


	ts->xfer_data = devm_kzalloc(ts->dev, HIMAX_BUS_RW_MAX_LEN, GFP_KERNEL);
	if (!ts->xfer_data)
		return -ENOMEM;

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	spi->chip_select = 0;

	ts->spi = spi;
	mutex_init(&ts->rw_lock);
	mutex_init(&ts->reg_lock);
	mutex_init(&ts->hid_ioctl_lock);
	dev_set_drvdata(&spi->dev, ts);
	spi_set_drvdata(spi, ts);

	ts->probe_finish = false;
	ts->initialized = false;
	ts->ic_boot_done = false;
	bret = himax_platform_init(ts, &pdata);
	if (!bret) {
		dev_err(ts->dev, "platform init failed");
		return -ENODEV;
	}

	bret = hx83102j_chip_detect(ts);
	if (!bret) {
		dev_err(ts->dev, "IC detect failed");
		return -ENODEV;
	}

	ret = himax_chip_init(ts);
	if (ret < 0)
		return ret;
	ts->probe_finish = true;
	return ret;

}


static void himax_spi_drv_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	if (ts->probe_finish) {
		if (ts->ic_boot_done) {
			himax_int_enable(ts, false);

			if (ts->hid_probe) {
				himax_hid_remove(ts);
				ts->hid_probe = false;
			}

			kfree(ts->hid_rd_data.rd_data);
			ts->hid_rd_data.rd_data = NULL;

			ts->ic_boot_done = false;
		}
	}
	spi_set_drvdata(spi, NULL);

}
static struct spi_driver himax_hid_over_spi_driver = {
	.driver = {
		.name =		"hx83102j",
		.owner =	THIS_MODULE,
		.pm	= &himax_hid_pm,
#if defined(CONFIG_OF)
		.of_match_table = of_match_ptr(himax_table),
#endif
	},
	.probe =	himax_spi_drv_probe,
	.remove =	himax_spi_drv_remove,
	.shutdown =	himax_shutdown,
};
static void himax_spi_drv_exit(void)
{
	spi_unregister_driver(&himax_hid_over_spi_driver);
}

static int himax_spi_drv_init(void)
{
	int ret;

	ret = spi_register_driver(&himax_hid_over_spi_driver);
	return ret;
}

static int __init himax_ic_init(void)
{
	int ret = 0;

	ret = himax_spi_drv_init();
	return ret;
}

static void __exit himax_ic_exit(void)
{
	himax_spi_drv_exit();
}

#if !defined(CONFIG_HID_HIMAX)
module_init(himax_ic_init);
#else
late_initcall(himax_ic_init);
#endif
module_exit(himax_ic_exit);

MODULE_DESCRIPTION("Himax SPI driver for HID simulator for " HIMAX_HX83102J_ID);
MODULE_AUTHOR("Himax, Inc.");
MODULE_LICENSE("GPL");
