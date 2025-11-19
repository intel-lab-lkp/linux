/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __linux_video_edid_h__
#define __linux_video_edid_h__

#include <uapi/video/edid.h>

#if defined(CONFIG_FIRMWARE_EDID)
const u8 *get_edid_info(void) __attribute_const__;
#endif

#endif /* __linux_video_edid_h__ */
