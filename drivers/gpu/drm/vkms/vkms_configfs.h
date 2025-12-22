/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _VKMS_CONFIGFS_H_
#define _VKMS_CONFIGFS_H_

#include <linux/types.h>

int vkms_configfs_register(void);
void vkms_configfs_unregister(void);

#if IS_ENABLED(CONFIG_KUNIT)
int vkms_configfs_parse_next_format(const char *page, const char *end_page, char **out);
#endif

#endif /* _VKMS_CONFIGFS_H_ */
