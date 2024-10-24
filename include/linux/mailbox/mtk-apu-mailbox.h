/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 MediaTek Inc.
 *
 */

#ifndef __MTK_APU_MAILBOX_H__
#define __MTK_APU_MAILBOX_H__

#define MSG_MBOX_SLOTS	(8)

struct mtk_apu_mailbox_msg {
	int send_cnt;
	u32 data[MSG_MBOX_SLOTS];
};

int mtk_apu_mbox_write(u32 val, u32 offset);
int mtk_apu_mbox_read(u32 offset, u32 *val);

#endif /* __MTK_APU_MAILBOX_H__ */
