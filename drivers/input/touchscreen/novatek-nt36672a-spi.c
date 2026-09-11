// SPDX-License-Identifier: GPL-2.0
/*
 * Novatek NT36672A SPI Touchscreen Driver
 *
 * Copyright (c) 2026
 *
 * Hardware:
 *   Novatek NT36672A TDDI controller connected over SPI.
 *   Firmware is loaded dynamically into controller SRAM via request_firmware().
 *   Panel lifecycle synchronization uses the DRM panel follower API.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#define NVT_MAX_TOUCHES			10
#define NVT_POINT_DATA_LEN		65
#define NVT_TRANSFER_LEN		1024
#define NVT_DEFAULT_FW_NAME		"novatek/nt36672a_tianma.bin"

/* NT36672A Memory Map & Registers */
#define EVENT_BUF_ADDR			0x21c00
#define EVENT_MAP_HOST_CMD		0x50
#define EVENT_MAP_RESET_COMPLETE	0x60
#define EVENT_MAP_FWINFO		0x78

#define BOOT_RDY_ADDR			0x3f10d
#define BLD_CRC_EN_ADDR			0x3f30e
#define ILM_DES_ADDR			0x3f128
#define ILM_LENGTH_ADDR			0x3f118
#define G_ILM_CHECKSUM_ADDR		0x3f100
#define DLM_DES_ADDR			0x3f12c
#define DLM_LENGTH_ADDR			0x3f130
#define G_DLM_CHECKSUM_ADDR		0x3f104
#define SPI_RD_FAST_ADDR		0x03f310

struct nvt_ts_bin_map {
	u32 bin_addr;
	u32 sram_addr;
	u32 size;
	u32 crc;
};

struct novatek_ts_data {
	struct spi_device *spi;
	struct input_dev *input_dev;
	struct touchscreen_properties prop;
	/* Protects SPI bus access, firmware loading and controller state */
	struct mutex lock;
	struct drm_panel_follower panel_follower;
	struct workqueue_struct *fw_wq;
	struct delayed_work fw_reload_work;
	const char *fw_name;

	u16 abs_x_max;
	u16 abs_y_max;
	u8 fw_ver;
	u8 fw_reset_state;
	bool fw_running;
	bool slot_active[NVT_MAX_TOUCHES];
};

static inline u32 byte_to_word(const u8 *b)
{
	return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

static int nvt_spi_write(struct spi_device *spi, const u8 *buf, size_t len)
{
	struct spi_transfer t = {0};
	struct spi_message m;
	u8 *xbuf;
	int ret;

	xbuf = kmemdup(buf, len, GFP_KERNEL);
	if (!xbuf)
		return -ENOMEM;

	xbuf[0] |= 0x80; /* SPI_WRITE_MASK */

	t.tx_buf = xbuf;
	t.len = len;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(spi, &m);
	kfree(xbuf);

	return ret;
}

static int nvt_spi_read(struct spi_device *spi, u8 *buf, size_t len)
{
	struct spi_transfer t = {0};
	struct spi_message m;
	u8 *xbuf, *rbuf;
	int ret;

	xbuf = kzalloc(len + 1, GFP_KERNEL);
	rbuf = kzalloc(len + 1, GFP_KERNEL);
	if (!xbuf || !rbuf) {
		kfree(xbuf);
		kfree(rbuf);
		return -ENOMEM;
	}

	xbuf[0] = buf[0] & 0x7f; /* SPI_READ_MASK */

	t.tx_buf = xbuf;
	t.rx_buf = rbuf;
	t.len = len + 1; /* 1 dummy byte */

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	ret = spi_sync(spi, &m);
	if (!ret)
		memcpy(buf + 1, rbuf + 2, len - 1);

	kfree(xbuf);
	kfree(rbuf);

	return ret;
}

static int nvt_set_page(struct spi_device *spi, u32 addr)
{
	u8 buf[3];

	buf[0] = 0xff; /* page command */
	buf[1] = (addr >> 15) & 0xff;
	buf[2] = (addr >> 7) & 0xff;

	return nvt_spi_write(spi, buf, sizeof(buf));
}

static int nvt_write_addr(struct spi_device *spi, u32 addr, u8 data)
{
	u8 buf[2];
	int ret;

	ret = nvt_set_page(spi, addr);
	if (ret)
		return ret;

	buf[0] = addr & 0x7f;
	buf[1] = data;

	return nvt_spi_write(spi, buf, sizeof(buf));
}

static void nvt_bootloader_reset(struct spi_device *spi)
{
	/*
	 * Configure SPI Fast Read to 0x00 so bootloader SRAM writes succeed.
	 * Do NOT write 0x69 to SWRST_N8_ADDR because NT36672A is a TDDI chip:
	 * writing 0x69 resets the entire IC (display timing generator, DCS registers, PWM)
	 * and causes the physical LCD to go completely black.
	 * The controller is already reset into ROM bootloader mode by hardware panel reset.
	 */
	nvt_write_addr(spi, SPI_RD_FAST_ADDR, 0x00);
}

static void nvt_set_bld_crc_bank(struct spi_device *spi, u32 des_addr, u32 sram_addr,
				 u32 len_addr, u32 size, u32 g_chk_addr, u32 crc)
{
	u8 buf[5];

	/* Destination address */
	nvt_set_page(spi, des_addr);
	buf[0] = des_addr & 0x7f;
	buf[1] = sram_addr & 0xff;
	buf[2] = (sram_addr >> 8) & 0xff;
	buf[3] = (sram_addr >> 16) & 0xff;
	nvt_spi_write(spi, buf, 4);

	/* Length (for NT36672A hw_crc=1, 3 bytes) */
	buf[0] = len_addr & 0x7f;
	buf[1] = size & 0xff;
	buf[2] = (size >> 8) & 0xff;
	nvt_spi_write(spi, buf, 3);

	/* Golden Checksum (4 bytes) */
	buf[0] = g_chk_addr & 0x7f;
	buf[1] = crc & 0xff;
	buf[2] = (crc >> 8) & 0xff;
	buf[3] = (crc >> 16) & 0xff;
	buf[4] = (crc >> 24) & 0xff;
	nvt_spi_write(spi, buf, 5);
}

static int nvt_write_sram(struct spi_device *spi, const u8 *fwdata,
			  u32 sram_addr, u32 size, u32 bin_addr)
{
	u32 count, i, len;
	int ret = 0;
	u8 *fwbuf;

	fwbuf = kmalloc(NVT_TRANSFER_LEN + 2, GFP_KERNEL);
	if (!fwbuf)
		return -ENOMEM;

	count = (size + NVT_TRANSFER_LEN - 1) / NVT_TRANSFER_LEN;

	for (i = 0; i < count; i++) {
		len = (size < NVT_TRANSFER_LEN) ? size : NVT_TRANSFER_LEN;

		ret = nvt_set_page(spi, sram_addr);
		if (ret)
			goto out;

		fwbuf[0] = sram_addr & 0x7f;
		memcpy(fwbuf + 1, &fwdata[bin_addr], len);
		ret = nvt_spi_write(spi, fwbuf, len + 1);
		if (ret)
			goto out;

		sram_addr += NVT_TRANSFER_LEN;
		bin_addr += NVT_TRANSFER_LEN;
		size -= len;
	}

out:
	kfree(fwbuf);
	return ret;
}

static int nvt_parse_and_download(struct novatek_ts_data *ts, const u8 *fwdata, size_t fwsize)
{
	struct spi_device *spi = ts->spi;
	struct nvt_ts_bin_map bin_map[16];
	u8 info_sec_num = 0;
	u8 ovly_sec_num = 0;
	u32 partition, list;
	u32 header_end, pos;
	u8 buf[18];
	int ret, retry;

	if (fwsize < 0x40)
		return -EINVAL;

	header_end = byte_to_word(&fwdata[0]);
	pos = 0x30;
	while (pos < header_end && info_sec_num < 14) {
		info_sec_num++;
		pos += 0x10;
	}

	ovly_sec_num = (fwdata[0x28] & 0x10) ? (fwdata[0x28] & 0x0f) : 0;
	partition = 2 + ovly_sec_num + info_sec_num;
	if (partition > 16)
		partition = 16;

	for (list = 0; list < partition; list++) {
		if (list < 2) {
			bin_map[list].bin_addr = byte_to_word(&fwdata[0 + list * 12]);
			bin_map[list].sram_addr = byte_to_word(&fwdata[4 + list * 12]);
			bin_map[list].size = byte_to_word(&fwdata[8 + list * 12]);
			bin_map[list].crc = byte_to_word(&fwdata[0x18 + list * 4]);
		} else {
			pos = 0x30 + (0x10 * (list - 2));
			bin_map[list].sram_addr = byte_to_word(&fwdata[pos]);
			bin_map[list].size = byte_to_word(&fwdata[pos + 4]);
			bin_map[list].bin_addr = byte_to_word(&fwdata[pos + 8]);
			bin_map[list].crc = byte_to_word(&fwdata[pos + 12]);
		}
	}

	nvt_bootloader_reset(spi);

	for (list = 0; list < partition; list++) {
		u32 size = bin_map[list].size;

		if (!size)
			continue;
		size += 1;

		ret = nvt_write_sram(spi, fwdata, bin_map[list].sram_addr, size,
				     bin_map[list].bin_addr);
		if (ret) {
			dev_err(&spi->dev, "SRAM write partition %u failed: %d\n", list, ret);
			return ret;
		}
	}

	nvt_set_bld_crc_bank(spi, ILM_DES_ADDR, bin_map[0].sram_addr,
			     ILM_LENGTH_ADDR, bin_map[0].size,
			     G_ILM_CHECKSUM_ADDR, bin_map[0].crc);
	nvt_set_bld_crc_bank(spi, DLM_DES_ADDR, bin_map[1].sram_addr,
			     DLM_LENGTH_ADDR, bin_map[1].size,
			     G_DLM_CHECKSUM_ADDR, bin_map[1].crc);

	/* Enable BLD CRC (bit 7 of 0x3F30E) */
	nvt_set_page(spi, BLD_CRC_EN_ADDR);
	buf[0] = BLD_CRC_EN_ADDR & 0x7f;
	buf[1] = 0xff;
	nvt_spi_read(spi, buf, 2);
	buf[0] = BLD_CRC_EN_ADDR & 0x7f;
	buf[1] = buf[1] | 0x80;
	nvt_spi_write(spi, buf, 2);

	/* Clear reset status and enable firmware CRC */
	nvt_set_page(spi, EVENT_BUF_ADDR);
	buf[0] = EVENT_MAP_RESET_COMPLETE & 0x7f;
	buf[1] = 0x00;
	nvt_spi_write(spi, buf, 2);

	buf[0] = EVENT_MAP_HOST_CMD & 0x7f;
	buf[1] = 0xae;
	nvt_spi_write(spi, buf, 2);

	/* Boot Ready */
	nvt_write_addr(spi, BOOT_RDY_ADDR, 1);
	msleep(10);

	/* Verify Firmware Reset State (>= 0xA0) */
	ts->fw_running = false;
	nvt_set_page(spi, EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE);
	for (retry = 0; retry < 20; retry++) {
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		ret = nvt_spi_read(spi, buf, 6);
		if (ret == 0 && buf[1] >= 0xa0 && buf[1] <= 0xaf) {
			ts->fw_reset_state = buf[1];
			ts->fw_running = true;
			break;
		}
		msleep(10);
	}

	/* Wait for baseline calibration */
	for (retry = 0; retry < 50; retry++) {
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		ret = nvt_spi_read(spi, buf, 6);
		if (ret == 0 && buf[1] >= 0xa1 && buf[1] <= 0xaf) {
			ts->fw_reset_state = buf[1];
			break;
		}
		msleep(10);
	}

	if (!ts->fw_running) {
		dev_err(&spi->dev, "firmware boot timeout: state=0x%02x\n", buf[1]);
		return -EIO;
	}

	/* Read FW Information */
	nvt_set_page(spi, EVENT_BUF_ADDR | EVENT_MAP_FWINFO);
	buf[0] = EVENT_MAP_FWINFO;
	ret = nvt_spi_read(spi, buf, 17);
	if (ret == 0) {
		ts->fw_ver = buf[1];
		ts->abs_x_max = (u16)((buf[5] << 8) | buf[6]);
		ts->abs_y_max = (u16)((buf[7] << 8) | buf[8]);
	}

	if (!ts->abs_x_max || ts->abs_x_max > 4096)
		ts->abs_x_max = 1080;
	if (!ts->abs_y_max || ts->abs_y_max > 4096)
		ts->abs_y_max = 2340;

	nvt_set_page(spi, EVENT_BUF_ADDR);

	return 0;
}

static int novatek_ts_load_firmware(struct novatek_ts_data *ts)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, ts->fw_name, &ts->spi->dev);
	if (ret) {
		dev_err(&ts->spi->dev, "failed to request firmware '%s': %d\n",
			ts->fw_name, ret);
		return ret;
	}

	ret = nvt_parse_and_download(ts, fw->data, fw->size);
	release_firmware(fw);

	return ret;
}

static void novatek_ts_fw_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct novatek_ts_data *ts = container_of(dwork, struct novatek_ts_data, fw_reload_work);
	u8 rst_chk[6] = {EVENT_MAP_RESET_COMPLETE, 0};
	int ret;

	mutex_lock(&ts->lock);

	nvt_set_page(ts->spi, EVENT_BUF_ADDR);
	ret = nvt_spi_read(ts->spi, rst_chk, 2);
	if (ret == 0 && rst_chk[1] >= 0xa0 && rst_chk[1] <= 0xaf) {
		/* Controller is already running valid firmware */
		mutex_unlock(&ts->lock);
		return;
	}

	ret = novatek_ts_load_firmware(ts);
	if (ret)
		dev_err(&ts->spi->dev, "firmware load failed: %d\n", ret);

	nvt_set_page(ts->spi, EVENT_BUF_ADDR);
	mutex_unlock(&ts->lock);
}

static int novatek_ts_panel_prepared(struct drm_panel_follower *follower)
{
	struct novatek_ts_data *ts = container_of(follower, struct novatek_ts_data, panel_follower);

	if (ts->fw_wq) {
		cancel_delayed_work_sync(&ts->fw_reload_work);
		queue_delayed_work(ts->fw_wq, &ts->fw_reload_work, msecs_to_jiffies(1500));
	}
	return 0;
}

static int novatek_ts_panel_unpreparing(struct drm_panel_follower *follower)
{
	struct novatek_ts_data *ts = container_of(follower, struct novatek_ts_data, panel_follower);

	if (ts->fw_wq)
		cancel_delayed_work_sync(&ts->fw_reload_work);
	return 0;
}

static const struct drm_panel_follower_funcs novatek_ts_panel_follower_funcs = {
	.panel_prepared = novatek_ts_panel_prepared,
	.panel_unpreparing = novatek_ts_panel_unpreparing,
};

static irqreturn_t novatek_ts_threaded_irq(int irq, void *dev_id)
{
	struct novatek_ts_data *ts = dev_id;
	u8 point_data[NVT_POINT_DATA_LEN + 2] = {0};
	bool slot_pressed[NVT_MAX_TOUCHES] = {false};
	int finger_cnt = 0;
	int ret, i;

	mutex_lock(&ts->lock);

	nvt_set_page(ts->spi, EVENT_BUF_ADDR);

	point_data[0] = 0x00;
	ret = nvt_spi_read(ts->spi, point_data, NVT_POINT_DATA_LEN + 1);
	if (ret < 0) {
		dev_err_ratelimited(&ts->spi->dev, "failed to read touch points: %d\n", ret);
		mutex_unlock(&ts->lock);
		return IRQ_HANDLED;
	}

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		int pos = 1 + 6 * i;
		u8 raw0 = point_data[pos];
		u8 input_id = raw0 >> 3;
		u8 status = raw0 & 0x07;

		if (input_id < 1 || input_id > NVT_MAX_TOUCHES)
			continue;

		if (status == 0x01 || status == 0x02) { /* 1: down, 2: move */
			u32 x = ((u32)point_data[pos + 1] << 4) | ((u32)point_data[pos + 3] >> 4);
			u32 y = ((u32)point_data[pos + 2] << 4) | ((u32)point_data[pos + 3] & 0x0f);
			u32 w = point_data[pos + 4] ? point_data[pos + 4] : 1;
			u32 p = point_data[pos + 5] ? point_data[pos + 5] : 1;

			if (x <= ts->abs_x_max && y <= ts->abs_y_max) {
				int slot = input_id - 1;

				slot_pressed[slot] = true;
				finger_cnt++;

				input_mt_slot(ts->input_dev, slot);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE, p);
			}
		}
	}

	for (i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (!slot_pressed[i] && ts->slot_active[i]) {
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
			ts->slot_active[i] = false;
		} else if (slot_pressed[i]) {
			ts->slot_active[i] = true;
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, finger_cnt > 0);
	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);

	mutex_unlock(&ts->lock);

	return IRQ_HANDLED;
}

static int novatek_ts_init_input(struct novatek_ts_data *ts)
{
	struct spi_device *spi = ts->spi;
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(&spi->dev);
	if (!input)
		return -ENOMEM;

	input->name = "Novatek NT36672A Touchscreen";
	input->id.bustype = BUS_SPI;
	input->dev.parent = &spi->dev;

	__set_bit(EV_SYN, input->evbit);
	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_ABS, input->evbit);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);

	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	ret = input_mt_init_slots(input, NVT_MAX_TOUCHES,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;

	ret = input_register_device(input);
	if (ret)
		return ret;

	ts->input_dev = input;

	return 0;
}

static int novatek_ts_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct novatek_ts_data *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	mutex_init(&ts->lock);

	ts->fw_wq = create_singlethread_workqueue("novatek_ts_fw");
	if (!ts->fw_wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&ts->fw_reload_work, novatek_ts_fw_work);

	ret = device_property_read_string(dev, "firmware-name", &ts->fw_name);
	if (ret)
		ts->fw_name = NVT_DEFAULT_FW_NAME;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz)
		spi->max_speed_hz = 4000000;

	ret = spi_setup(spi);
	if (ret)
		goto err_destroy_wq;

	spi_set_drvdata(spi, ts);

	ts->abs_x_max = 1080;
	ts->abs_y_max = 2340;

	ret = novatek_ts_load_firmware(ts);
	if (ret) {
		dev_err(dev, "failed to download firmware during probe: %d\n", ret);
		goto err_destroy_wq;
	}

	dev_info(dev, "Novatek NT36672A initialized (fw_ver=0x%02x, reset_state=0x%02x)\n",
		 ts->fw_ver, ts->fw_reset_state);

	ret = novatek_ts_init_input(ts);
	if (ret)
		goto err_destroy_wq;

	if (spi->irq <= 0) {
		ret = dev_err_probe(dev, -EINVAL, "no IRQ specified\n");
		goto err_destroy_wq;
	}

	ret = devm_request_threaded_irq(dev, spi->irq, NULL,
					novatek_ts_threaded_irq,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"novatek-nt36672a-ts", ts);
	if (ret) {
		dev_err_probe(dev, ret, "failed to request threaded IRQ\n");
		goto err_destroy_wq;
	}

	if (drm_is_panel_follower(dev)) {
		ts->panel_follower.funcs = &novatek_ts_panel_follower_funcs;
		ret = devm_drm_panel_add_follower(dev, &ts->panel_follower);
		if (ret) {
			dev_err_probe(dev, ret, "failed to add panel follower\n");
			goto err_destroy_wq;
		}
	}

	return 0;

err_destroy_wq:
	cancel_delayed_work_sync(&ts->fw_reload_work);
	destroy_workqueue(ts->fw_wq);
	return ret;
}

static void novatek_ts_remove(struct spi_device *spi)
{
	struct novatek_ts_data *ts = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&ts->fw_reload_work);
	destroy_workqueue(ts->fw_wq);
}

static const struct of_device_id novatek_ts_of_match[] = {
	{ .compatible = "novatek,nt36672a-spi" },
	{ .compatible = "novatek,nt36672a-ts" },
	{ }
};
MODULE_DEVICE_TABLE(of, novatek_ts_of_match);

static const struct spi_device_id novatek_ts_spi_id[] = {
	{ "nt36672a-ts" },
	{ }
};
MODULE_DEVICE_TABLE(spi, novatek_ts_spi_id);

static struct spi_driver novatek_ts_driver = {
	.driver = {
		.name = "novatek-nt36672a-spi",
		.of_match_table = novatek_ts_of_match,
	},
	.id_table = novatek_ts_spi_id,
	.probe = novatek_ts_probe,
	.remove = novatek_ts_remove,
};
module_spi_driver(novatek_ts_driver);

MODULE_AUTHOR("Antigravity Pair Programmer");
MODULE_DESCRIPTION("Novatek NT36672A SPI Touchscreen Driver");
MODULE_LICENSE("GPL");
