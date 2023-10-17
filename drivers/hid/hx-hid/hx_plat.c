// SPDX-License-Identifier: GPL-2.0
/*  Himax Driver Code for Common IC to simulate HID
 *
 *  Copyright (C) 2023 Himax Corporation.
 *
 *  This software is licensed under the terms of the GNU General Public
 *  License version 2,  as published by the Free Software Foundation,  and
 *  may be copied,  distributed,  and modified under those terms.
 *
 *  This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "hx_core.h"
#include "hx_plat.h"

void himax_rst_gpio_set(int pinnum, u8 value)
{
	gpio_direction_output(pinnum, value);
}

int himax_gpio_power_config(struct himax_ts_data *ts,
			    struct himax_platform_data *pdata)
{
	int error = 0;

	if (gpio_is_valid(pdata->gpio_reset)) {
		error = gpio_request(pdata->gpio_reset, "himax-reset");

		if (error < 0) {
			E("request reset pin failed");
			goto err_gpio_reset_req;
		}

		error = gpio_direction_output(pdata->gpio_reset, 0);

		if (error) {
			E("unable to set direction for gpio [%d]",
			  pdata->gpio_reset);
			goto err_gpio_reset_dir;
		}
	}

	if (pdata->vccd_supply) {
		error = regulator_enable(pdata->vccd_supply);
		if (error) {
			E("unable to enable vccd supply");
			goto err_vccd_supply_enable;
		}
	}

	if (pdata->vcca_supply) {
		error = regulator_enable(pdata->vcca_supply);
		if (error) {
			E("unable to enable vcca supply");
			goto err_vcca_supply_enable;
		}
	}

	if (gpio_is_valid(pdata->gpio_irq)) {
		/* configure touchscreen irq gpio */
		error = gpio_request(pdata->gpio_irq, "himax_gpio_irq");

		if (error) {
			E("unable to request gpio [%d]", pdata->gpio_irq);
			goto err_gpio_irq_req;
		}

		error = gpio_direction_input(pdata->gpio_irq);
		if (error) {
			E("unable to set direction for gpio [%d]",
			  pdata->gpio_irq);
			goto err_gpio_irq_set_input;
		}

		ts->hx_irq = gpio_to_irq(pdata->gpio_irq);
	} else if (pdata->of_irq) {
		ts->hx_irq = pdata->of_irq;
	} else {
		E("irq not provided");
		goto err_gpio_irq_req;
	}

	usleep_range(2000, 2001);

	if (gpio_is_valid(pdata->gpio_reset)) {
		error = gpio_direction_output(pdata->gpio_reset, 1);

		if (error) {
			E("unable to set direction for gpio [%d]",
			  pdata->gpio_reset);
			goto err_gpio_reset_set_high;
		}
	}

	return error;

err_gpio_reset_set_high:
err_gpio_irq_set_input:
	if (gpio_is_valid(pdata->gpio_irq))
		gpio_free(pdata->gpio_irq);
err_gpio_irq_req:
	if (pdata->vcca_supply) {
		regulator_disable(pdata->vcca_supply);
		regulator_put(pdata->vcca_supply);
		pdata->vcca_supply = NULL;
	}
err_vcca_supply_enable:
	if (pdata->vccd_supply) {
		regulator_disable(pdata->vccd_supply);
		regulator_put(pdata->vccd_supply);
		pdata->vccd_supply = NULL;
	}
err_vccd_supply_enable:
err_gpio_reset_dir:
	if (gpio_is_valid(pdata->gpio_reset))
		gpio_free(pdata->gpio_reset);
err_gpio_reset_req:

	return error;
}

void himax_gpio_power_deconfig(struct himax_platform_data *pdata)
{
	if (gpio_is_valid(pdata->gpio_irq)) {
		I("free gpio_irq = %d", pdata->gpio_irq);
		gpio_free(pdata->gpio_irq);
	}

	if (gpio_is_valid(pdata->gpio_reset)) {
		I("free gpio_reset = %d", pdata->gpio_reset);
		gpio_free(pdata->gpio_reset);
	}

	if (pdata->vcca_supply) {
		regulator_disable(pdata->vcca_supply);
		regulator_put(pdata->vcca_supply);
		pdata->vcca_supply = NULL;
	}
	if (pdata->vccd_supply) {
		regulator_disable(pdata->vccd_supply);
		regulator_put(pdata->vccd_supply);
		pdata->vccd_supply = NULL;
	}
}

static int himax_spi_read(struct himax_ts_data *ts, u8 *cmd,
			  u8 cmd_len, u8 *buf, u32 len)
{
	struct spi_message m;
	int result = NO_ERR;
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
		if (unlikely(error))
			E("SPI read error: %d", error);
		else
			break;
	}

	if (retry == HIMAX_BUS_RETRY_TIMES) {
		E("SPI read error retry over %d", HIMAX_BUS_RETRY_TIMES);
		result = -EIO;
		goto END;
	} else {
		memcpy(buf, ts->xfer_data + cmd_len, len);
	}

END:
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

int himax_bus_read(struct himax_ts_data *ts, u8 cmd,
		   u8 *buf, u32 len)
{
	int result = -1;
	u8 hw_addr = 0x00;

	if (len > BUS_R_DLEN) {
		E("len[%d] is over %d", len, BUS_R_DLEN);
		return result;
	}

	mutex_lock(&ts->rw_lock);

	if (ts->select_slave_reg) {
		hw_addr = ts->slave_read_reg;
		I("now addr=0x%02X!", hw_addr);
	} else {
		hw_addr = 0xF3;
	}

	memset(ts->xfer_data, 0, BUS_R_HLEN + len);
	ts->xfer_data[0] = hw_addr;
	ts->xfer_data[1] = cmd;
	ts->xfer_data[2] = 0x00;
	result = himax_spi_read(ts, ts->xfer_data, BUS_R_HLEN, buf, len);

	mutex_unlock(&ts->rw_lock);

	return result;
}

int himax_bus_write(struct himax_ts_data *ts, u8 cmd,
		    u8 *addr, u8 *data, u32 len)
{
	int result = -1;
	u8 offset = 0;
	u32 tmp_len = len;
	u8 hw_addr = 0x00;

	if (len > BUS_W_DLEN) {
		E("len[%d] is over %d", len, BUS_W_DLEN);
		return -EFAULT;
	}

	mutex_lock(&ts->rw_lock);

	if (ts->select_slave_reg) {
		hw_addr = ts->slave_write_reg;
		I("now addr=0x%02X!", hw_addr);
	} else {
		hw_addr = 0xF2;
	}

	ts->xfer_data[0] = hw_addr;
	ts->xfer_data[1] = cmd;
	offset = BUS_W_HLEN;

	if (addr) {
		memcpy(ts->xfer_data + offset, addr, 4);
		offset += 4;
		tmp_len -= 4;
	}

	if (data)
		memcpy(ts->xfer_data + offset, data, tmp_len);

	result = himax_spi_write(ts, ts->xfer_data, len + BUS_W_HLEN);

	mutex_unlock(&ts->rw_lock);

	return result;
}

void himax_int_enable(struct himax_ts_data *ts, int enable)
{
	unsigned long irqflags = 0;
	int irqnum = ts->hx_irq;

	spin_lock_irqsave(&ts->irq_lock, irqflags);
	D("Entering! irqnum = %d", irqnum);
	if (enable == 1 && atomic_read(&ts->irq_state) == 0) {
		atomic_set(&ts->irq_state, 1);
		enable_irq(irqnum);
		ts->irq_enabled = 1;
	} else if (enable == 0 && atomic_read(&ts->irq_state) == 1) {
		atomic_set(&ts->irq_state, 0);
		disable_irq_nosync(irqnum);
		ts->irq_enabled = 0;
	}

	I("interrupt enable = %d", enable);
	spin_unlock_irqrestore(&ts->irq_lock, irqflags);
}

static void himax_ts_isr_func(struct himax_ts_data *ts)
{
	himax_ts_work(ts);
}

irqreturn_t himax_ts_thread(int irq, void *ptr)
{
	himax_ts_isr_func((struct himax_ts_data *)ptr);

	return IRQ_HANDLED;
}

static void himax_ts_work_func(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work,
		struct himax_ts_data, work);

	himax_ts_work(ts);
}

int himax_int_register_trigger(struct himax_ts_data *ts)
{
	int ret = 0;

	if (ts->ic_data->HX_INT_IS_EDGE) {
		I("edge triiger falling");
		ret = request_threaded_irq(ts->hx_irq, NULL, himax_ts_thread,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			himax_dev_name, ts);
	} else {
		I("level trigger low");
		ret = request_threaded_irq(ts->hx_irq, NULL, himax_ts_thread,
					   IRQF_TRIGGER_LOW | IRQF_ONESHOT, himax_dev_name, ts);
	}

	return ret;
}

int himax_ts_register_interrupt(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->irq_enabled = 0;

	/* Work functon */
	if (ts->hx_irq) {/*INT mode*/
		ts->use_irq = 1;
		ret = himax_int_register_trigger(ts);

		if (ret == 0) {
			ts->irq_enabled = 1;
			atomic_set(&ts->irq_state, 1);
			I("irq enabled at number: %d",
			  ts->hx_irq);
		} else {
			ts->use_irq = 0;
			E("request_irq failed");
		}
	} else {
		I("ts->hx_irq is empty, use polling mode.");
	}

	/*if use polling mode need to disable HX_ESD_RECOVERY function*/
	if (!ts->use_irq) {
		ts->himax_wq = create_singlethread_workqueue("himax_touch");
		INIT_WORK(&ts->work, himax_ts_work_func);
		hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ts->timer.function = himax_ts_timer_func;
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		I("polling mode enabled");
	}

	return ret;
}

int himax_ts_unregister_interrupt(struct himax_ts_data *ts)
{
	int ret = 0;

	I("entered.");

	/* Work functon */
	if (ts->hx_irq && ts->use_irq) {/*INT mode*/
		free_irq(ts->hx_irq, ts);
		I("irq disabled at qpio: %d",
		  ts->hx_irq);
	}

	/*if use polling mode need to disable HX_ESD_RECOVERY function*/
	if (!ts->use_irq) {
		hrtimer_cancel(&ts->timer);
		cancel_work_sync(&ts->work);
		if (ts->himax_wq)
			destroy_workqueue(ts->himax_wq);
		I("polling mode destroyed");
	}

	return ret;
}

#if defined(CONFIG_FB)
int fb_notifier_callback(struct notifier_block *self,
			 unsigned long event, void *data)
{
	const struct fb_event *evdata = data;
	int *blank;
	struct himax_ts_data *ts =
	    container_of(self, struct himax_ts_data, fb_notif);

	I("entered");

	if (!ts) {
		E("ts is NULL");
		return -ECANCELED;
	}

	if (!ts->ic_boot_done) {
		E("IC is booting");
		return -ECANCELED;
	}

	if (evdata && evdata->data &&
	    event == FB_EVENT_BLANK &&
		ts->dev) {
		blank = evdata->data;

		switch (*blank) {
		case FB_BLANK_UNBLANK:
			himax_resume(ts->dev);
			break;

		case FB_BLANK_POWERDOWN:
		case FB_BLANK_HSYNC_SUSPEND:
		case FB_BLANK_VSYNC_SUSPEND:
		case FB_BLANK_NORMAL:
			himax_suspend(ts->dev);
			break;
		}
	}

	return 0;
}

void himax_fb_register(struct work_struct *work)
{
	int ret = 0;
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_att.work);

	ts->fb_notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);

	if (ret)
		E("Unable to register fb_notifier: %d", ret);
}
#endif

void hx_check_power_status(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_pwr.work);

	ts->latest_power_status = power_supply_is_system_supplied();

	I("Update ts->latest_power_status = %X", ts->latest_power_status);

	himax_cable_detect_func(ts, true);
}

int pwr_notifier_callback(struct notifier_block *self,
			  unsigned long event, void *data)
{
	struct himax_ts_data *ts = container_of(self, struct himax_ts_data,
		power_notif);
	I("entered. event = %lX", event);

	cancel_delayed_work_sync(&ts->work_pwr);
	queue_delayed_work(ts->himax_pwr_wq, &ts->work_pwr,
			   msecs_to_jiffies(1100));

	return 0;
}

void himax_pwr_register(struct work_struct *work)
{
	int ret = 0;
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_pwr.work);

	ts->power_notif.notifier_call = pwr_notifier_callback;
	ret = power_supply_reg_notifier(&ts->power_notif);
	if (ret) {
		E("Unable to register power_notif: %d", ret);
	} else {
		INIT_DELAYED_WORK(&ts->work_pwr, hx_check_power_status);
		queue_delayed_work(ts->himax_pwr_wq, &ts->work_pwr,
				   msecs_to_jiffies(3000));
	}
}
