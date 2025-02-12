/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2025 Loongson Technology Corporation Limited */

#ifndef __LOONGSON_SE_H__
#define __LOONGSON_SE_H__

#define SE_DATA_S			0x0
#define SE_DATA_L			0x20
#define SE_S2LINT_STAT			0x88
#define SE_S2LINT_EN			0x8c
#define SE_S2LINT_SET			0x90
#define SE_S2LINT_CL			0x94
#define SE_L2SINT_STAT			0x98
#define SE_L2SINT_EN			0x9c
#define SE_L2SINT_SET			0xa0
#define SE_L2SINT_CL			0xa4

/* INT bit definition */
#define SE_INT_SETUP			BIT(0)
#define SE_INT_TPM			BIT(5)

#define SE_CMD_START			0x0
#define SE_CMD_STOP			0x1
#define SE_CMD_GETVER			0x2
#define SE_CMD_SETBUF			0x3
#define SE_CMD_SETMSG			0x4

#define SE_CMD_RNG			0x100
#define SE_CMD_SM2_SIGN			0x200
#define SE_CMD_SM2_VSIGN		0x201
#define SE_CMD_SM3_DIGEST		0x300
#define SE_CMD_SM3_UPDATE		0x301
#define SE_CMD_SM3_FINISH		0x302
#define SE_CMD_SM4_ECB_ENCRY		0x400
#define SE_CMD_SM4_ECB_DECRY		0x401
#define SE_CMD_SM4_CBC_ENCRY		0x402
#define SE_CMD_SM4_CBC_DECRY		0x403
#define SE_CMD_SM4_CTR			0x404
#define SE_CMD_TPM			0x500
#define SE_CMD_ZUC_INIT_READ		0x600
#define SE_CMD_ZUC_READ			0x601
#define SE_CMD_SDF			0x700

#define SE_CH_MAX			32
#define SE_CH_RNG			1
#define SE_CH_SM2			2
#define SE_CH_SM3			3
#define SE_CH_SM4			4
#define SE_CH_TPM			5
#define SE_CH_ZUC			6
#define SE_CH_SDF			7

struct lsse_ch {
	struct loongson_se *se;
	void *priv;
	u32 version;
	u32 id;
	u32 int_bit;

	void *smsg;
	void *rmsg;
	int msg_size;

	void *data_buffer;
	int data_size;
	u32 off;

	void (*complete)(struct lsse_ch *se_ch);
};

struct lsse_ch *se_init_ch(struct device *dev, int id, int data_size, int msg_size,
			   void *priv, void (*complete)(struct lsse_ch *se_ch));
int se_send_ch_requeset(struct lsse_ch *ch);

#endif
