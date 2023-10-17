/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __HX_PLAT_H__
#define __HX_PLAT_H__

#include "hx_core.h"
#include <linux/notifier.h>
#include <linux/power_supply.h>

#if defined(CONFIG_FB)
int fb_notifier_callback(struct notifier_block *self,
			 unsigned long event, void *data);
void himax_fb_register(struct work_struct *work);
#endif

void himax_pwr_register(struct work_struct *work);

int himax_gpio_power_config(struct himax_ts_data *ts,
			    struct himax_platform_data *pdata);
void himax_gpio_power_deconfig(struct himax_platform_data *pdata);
int himax_bus_read(struct himax_ts_data *ts, u8 cmd, u8 *buf,
		   u32 len);
int himax_bus_write(struct himax_ts_data *ts, u8 cmd, u8 *addr,
		    u8 *data, u32 len);
void himax_int_enable(struct himax_ts_data *ts, int enable);
int himax_int_register_trigger(struct himax_ts_data *ts);
int himax_int_en_set(void);
int himax_ts_register_interrupt(struct himax_ts_data *ts);
int himax_ts_unregister_interrupt(struct himax_ts_data *ts);

#endif
