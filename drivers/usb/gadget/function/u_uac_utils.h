/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * u_uac_utils.h -- Utilities for UAC1/2 function driver
 *
 * Copyright (C) 2024
 * Author: Chris Wulff <crwulff@gmail.com>
 */

#ifndef __U_UAC_UTILS_H
#define __U_UAC_UTILS_H

#define uac_kstrtou8			kstrtou8
#define uac_kstrtos16			kstrtos16
#define uac_kstrtou32			kstrtou32
#define uac_kstrtobool(s, base, res)	kstrtobool((s), (res))

#define u8_FMT "%u\n"
#define u32_FMT "%u\n"
#define s16_FMT "%hd\n"
#define bool_FMT "%u\n"

#define UAC_ATTRIBUTE(prefix, to_struct, var, lock, refcnt, type, name) \
static ssize_t prefix##_##name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	to_struct;							\
	int result;							\
									\
	mutex_lock(&lock);						\
	result = sprintf(page, type##_FMT, var->name);			\
	mutex_unlock(&lock);						\
									\
	return result;							\
}									\
									\
static ssize_t prefix##_##name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	to_struct;							\
	int ret;							\
	type num;							\
									\
	mutex_lock(&lock);						\
	if (refcnt) {							\
		ret = -EBUSY;						\
		goto end;						\
	}								\
									\
	ret = uac_kstrto##type(page, 0, &num);				\
	if (ret)							\
		goto end;						\
									\
	var->name = num;						\
	ret = len;							\
									\
end:									\
	mutex_unlock(&lock);						\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(prefix##_, name)

#define UAC_RATE_ATTRIBUTE(prefix, to_struct, var, lock, refcnt, name)	\
static ssize_t prefix##_##name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	to_struct;							\
	int result = 0;							\
	int i;								\
									\
	mutex_lock(&lock);						\
	page[0] = '\0';							\
	for (i = 0; i < UAC_MAX_RATES; i++) {				\
		if (var->name##s[i] == 0)				\
			break;						\
		result += sprintf(page + strlen(page), "%u,",		\
				var->name##s[i]);			\
	}								\
	if (strlen(page) > 0)						\
		page[strlen(page) - 1] = '\n';				\
	mutex_unlock(&lock);						\
									\
	return result;							\
}									\
									\
static ssize_t prefix##_##name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	to_struct;							\
	char *split_page = NULL;					\
	int ret = -EINVAL;						\
	char *token;							\
	u32 num;							\
	int i;								\
									\
	mutex_lock(&lock);						\
	if (refcnt) {							\
		ret = -EBUSY;						\
		goto end;						\
	}								\
									\
	i = 0;								\
	memset(var->name##s, 0x00, sizeof(var->name##s));		\
	split_page = kstrdup(page, GFP_KERNEL);				\
	while ((token = strsep(&split_page, ",")) != NULL) {		\
		ret = kstrtou32(token, 0, &num);			\
		if (ret)						\
			goto end;					\
									\
		var->name##s[i++] = num;				\
		ret = len;						\
	};								\
									\
end:									\
	kfree(split_page);						\
	mutex_unlock(&lock);						\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(prefix##_, name)

#define UAC_ATTRIBUTE_STRING(prefix, to_struct, var, lock, refcnt, name) \
static ssize_t prefix##_##name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	to_struct;							\
	int result;							\
									\
	mutex_lock(&lock);						\
	result = scnprintf(page, sizeof(var->name), "%s", var->name);	\
	mutex_unlock(&lock);						\
									\
	return result;							\
}									\
									\
static ssize_t prefix##_##name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	to_struct;							\
	int ret = 0;							\
									\
	mutex_lock(&lock);						\
	if (refcnt) {							\
		ret = -EBUSY;						\
		goto end;						\
	}								\
									\
	if (len && page[len - 1] == '\n')				\
		len--;							\
									\
	ret = scnprintf(var->name, min(sizeof(var->name), len + 1),	\
			"%s", page);					\
									\
end:									\
	mutex_unlock(&lock);						\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(prefix##_, name)

#define UAC_ATTRIBUTE_SYNC(prefix, to_struct, var, lock, refcnt, name)	\
static ssize_t prefix##_##name##_show(struct config_item *item,		\
				      char *page)			\
{									\
	to_struct;							\
	int result;							\
	char *str;							\
									\
	mutex_lock(&lock);						\
	switch (var->name) {						\
	case USB_ENDPOINT_SYNC_ASYNC:					\
		str = "async";						\
		break;							\
	case USB_ENDPOINT_SYNC_ADAPTIVE:				\
		str = "adaptive";					\
		break;							\
	default:							\
		str = "unknown";					\
		break;							\
	}								\
	result = sprintf(page, "%s\n", str);				\
	mutex_unlock(&lock);						\
									\
	return result;							\
}									\
									\
static ssize_t prefix##_##name##_store(struct config_item *item,	\
				       const char *page, size_t len)	\
{									\
	to_struct;							\
	int ret = 0;							\
									\
	mutex_lock(&lock);						\
	if (refcnt) {							\
		ret = -EBUSY;						\
		goto end;						\
	}								\
									\
	if (!strncmp(page, "async", 5))					\
		var->name = USB_ENDPOINT_SYNC_ASYNC;			\
	else if (!strncmp(page, "adaptive", 8))				\
		var->name = USB_ENDPOINT_SYNC_ADAPTIVE;			\
	else {								\
		ret = -EINVAL;						\
		goto end;						\
	}								\
									\
	ret = len;							\
									\
end:									\
	mutex_unlock(&lock);						\
	return ret;							\
}									\
									\
CONFIGFS_ATTR(prefix##_, name)

/*
 * Functions for EP interval and max packet size
 */

static const char *const speed_names[] = {
	[USB_SPEED_UNKNOWN] = "UNKNOWN",
	[USB_SPEED_LOW] = "LS",
	[USB_SPEED_FULL] = "FS",
	[USB_SPEED_HIGH] = "HS",
	[USB_SPEED_WIRELESS] = "W",
	[USB_SPEED_SUPER] = "SS",
	[USB_SPEED_SUPER_PLUS] = "SS+",
};

static int get_max_srate(const int *srates)
{
	int i, max_srate = 0;

	for (i = 0; i < UAC_MAX_RATES; i++) {
		if (srates[i] == 0)
			break;
		if (srates[i] > max_srate)
			max_srate = srates[i];
	}
	return max_srate;
}

static int get_max_bw_for_bint(u8 bint, unsigned int factor, int chmask,
			       int srate, int ssize, int sync, int fb_max)
{
	u16 max_size_bw;

	if (sync == USB_ENDPOINT_SYNC_ASYNC) {
		// playback is always async, capture only when configured
		// Win10 requires max packet size + 1 frame
		srate = srate * (1000 + fb_max) / 1000;
		// updated srate is always bigger, therefore DIV_ROUND_UP always yields +1
		max_size_bw = num_channels(chmask) * ssize *
			(DIV_ROUND_UP(srate, factor / (1 << (bint - 1))));
	} else {
		// adding 1 frame provision for Win10
		max_size_bw = num_channels(chmask) * ssize *
			(DIV_ROUND_UP(srate, factor / (1 << (bint - 1))) + 1);
	}
	return max_size_bw;
}

static int uac_set_ep_max_packet_size_bint(struct device *dev,
	struct usb_endpoint_descriptor *ep_desc,
	enum usb_device_speed speed, bool is_playback, int hs_bint,
	int chmask, int srate, int ssize, int sync, int fb_max)
{
	u16 max_size_bw, max_size_ep;
	u8 bint;
	char *dir;

	switch (speed) {
	case USB_SPEED_FULL:
		max_size_ep = 1023;
		// fixed
		bint = ep_desc->bInterval;
		max_size_bw = get_max_bw_for_bint(bint, 1000, chmask, srate,
						  ssize, sync, fb_max);
		break;

	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
		max_size_ep = 1024;
		if (hs_bint > 0) {
			/* fixed bint */
			bint = hs_bint;
			max_size_bw = get_max_bw_for_bint(bint, 8000, chmask, srate,
							  ssize, sync, fb_max);
		} else {
			/* checking bInterval from 4 to 1 whether the required bandwidth fits */
			for (bint = 4; bint > 0; --bint) {
				max_size_bw = get_max_bw_for_bint(
					bint, 8000, chmask, srate,
					ssize, sync, fb_max);
				if (max_size_bw <= max_size_ep)
					break;
			}
		}
		break;

	default:
		return -EINVAL;
	}

	if (is_playback)
		dir = "Playback";
	else
		dir = "Capture";

	if (max_size_bw <= max_size_ep)
		dev_dbg(dev,
			"%s %s: Would use wMaxPacketSize %d and bInterval %d\n",
			speed_names[speed], dir, max_size_bw, bint);
	else {
		dev_warn(dev,
			"%s %s: Req. wMaxPacketSize %d at bInterval %d > max ISOC %d, may drop data!\n",
			speed_names[speed], dir, max_size_bw, bint, max_size_ep);
		max_size_bw = max_size_ep;
	}

	ep_desc->wMaxPacketSize = cpu_to_le16(max_size_bw);
	ep_desc->bInterval = bint;

	return 0;
}

#endif	/* __U_UAC_UTILS_H */
