/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 *
 */

#ifndef __MTK_APU_MAILBOX_H__
#define __MTK_APU_MAILBOX_H__

struct mtk_apu_mailbox_msg {
	u8 data_cnt;
	u32 *data;
};

#endif /* __MTK_APU_MAILBOX_H__ */
