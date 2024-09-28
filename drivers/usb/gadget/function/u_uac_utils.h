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

#endif	/* __U_UAC_UTILS_H */
