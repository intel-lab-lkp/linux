// SPDX-License-Identifier: GPL-2.0

#include <linux/rtc.h>

int rust_helper_devm_rtc_register_device(struct rtc_device *rtc)
{
	return __devm_rtc_register_device(THIS_MODULE, rtc);
}

