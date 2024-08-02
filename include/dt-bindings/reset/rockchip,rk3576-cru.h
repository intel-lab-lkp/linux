/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2023 Rockchip Electronics Co. Ltd.
 * Copyright (c) 2024 Collabora Ltd.
 *
 * Author: Elaine Zhang <zhangqing@rock-chips.com>
 * Author: Detlev Casanova <detlev.casanova@collabora.com>
 */

#ifndef _DT_BINDINGS_RESET_ROCKCHIP_RK3576_H
#define _DT_BINDINGS_RESET_ROCKCHIP_RK3576_H

#define SRST_A_TOP_BIU			1
#define SRST_P_TOP_BIU			2
#define SRST_A_TOP_MID_BIU		3
#define SRST_A_SECURE_HIGH_BIU		4
#define SRST_H_TOP_BIU			5

#define SRST_H_VO0VOP_CHANNEL_BIU	6
#define SRST_A_VO0VOP_CHANNEL_BIU	7

#define SRST_BISRINTF			8

#define SRST_H_AUDIO_BIU		9
#define SRST_H_ASRC_2CH_0		10
#define SRST_H_ASRC_2CH_1		11
#define SRST_H_ASRC_4CH_0		12
#define SRST_H_ASRC_4CH_1		13
#define SRST_ASRC_2CH_0			14
#define SRST_ASRC_2CH_1			15
#define SRST_ASRC_4CH_0			16
#define SRST_ASRC_4CH_1			17
#define SRST_M_SAI0_8CH			18
#define SRST_H_SAI0_8CH			19
#define SRST_H_SPDIF_RX0		20
#define SRST_M_SPDIF_RX0		21

#define SRST_H_SPDIF_RX1		22
#define SRST_M_SPDIF_RX1		23
#define SRST_M_SAI1_8CH			24
#define SRST_H_SAI1_8CH			25
#define SRST_M_SAI2_2CH			26
#define SRST_H_SAI2_2CH			27
#define SRST_M_SAI3_2CH			28
#define SRST_H_SAI3_2CH			29

#define SRST_M_SAI4_2CH			30
#define SRST_H_SAI4_2CH			31
#define SRST_H_ACDCDIG_DSM		32
#define SRST_M_ACDCDIG_DSM		33
#define SRST_PDM1			34
#define SRST_H_PDM1			35
#define SRST_M_PDM1			36
#define SRST_H_SPDIF_TX0		37
#define SRST_M_SPDIF_TX0		38
#define SRST_H_SPDIF_TX1		39
#define SRST_M_SPDIF_TX1		40

#define SRST_A_BUS_BIU			41
#define SRST_P_BUS_BIU			42
#define SRST_P_CRU			43
#define SRST_H_CAN0			44
#define SRST_CAN0			45
#define SRST_H_CAN1			46
#define SRST_CAN1			47
#define SRST_P_INTMUX2BUS		48
#define SRST_P_VCCIO_IOC		49
#define SRST_H_BUS_BIU			50
#define SRST_KEY_SHIFT			51

#define SRST_P_I2C1			52
#define SRST_P_I2C2			53
#define SRST_P_I2C3			54
#define SRST_P_I2C4			55
#define SRST_P_I2C5			56
#define SRST_P_I2C6			57
#define SRST_P_I2C7			58
#define SRST_P_I2C8			59
#define SRST_P_I2C9			60
#define SRST_P_WDT_BUSMCU		61
#define SRST_T_WDT_BUSMCU		62
#define SRST_A_GIC			63
#define SRST_I2C1			64
#define SRST_I2C2			65
#define SRST_I2C3			66
#define SRST_I2C4			67

#define SRST_I2C5			68
#define SRST_I2C6			69
#define SRST_I2C7			70
#define SRST_I2C8			71
#define SRST_I2C9			72
#define SRST_P_SARADC			73
#define SRST_SARADC			74
#define SRST_P_TSADC			75
#define SRST_TSADC			76
#define SRST_P_UART0			77
#define SRST_P_UART2			78
#define SRST_P_UART3			79
#define SRST_P_UART4			80
#define SRST_P_UART5			81
#define SRST_P_UART6			82

#define SRST_P_UART7			83
#define SRST_P_UART8			84
#define SRST_P_UART9			85
#define SRST_P_UART10			86
#define SRST_P_UART11			87
#define SRST_S_UART0			88
#define SRST_S_UART2			89
#define SRST_S_UART3			90
#define SRST_S_UART4			91
#define SRST_S_UART5			92

#define SRST_S_UART6			93
#define SRST_S_UART7			94
#define SRST_S_UART8			95
#define SRST_S_UART9			96
#define SRST_S_UART10			97
#define SRST_S_UART11			98
#define SRST_P_SPI0			99
#define SRST_P_SPI1			100
#define SRST_P_SPI2			101

#define SRST_P_SPI3			102
#define SRST_P_SPI4			103
#define SRST_SPI0			104
#define SRST_SPI1			105
#define SRST_SPI2			106
#define SRST_SPI3			107
#define SRST_SPI4			108
#define SRST_P_WDT0			109
#define SRST_T_WDT0			110
#define SRST_P_SYS_GRF			111
#define SRST_P_PWM1			112
#define SRST_PWM1			113

#define SRST_P_BUSTIMER0		114
#define SRST_P_BUSTIMER1		115
#define SRST_TIMER0			116
#define SRST_TIMER1			117
#define SRST_TIMER2			118
#define SRST_TIMER3			119
#define SRST_TIMER4			120
#define SRST_TIMER5			121
#define SRST_P_BUSIOC			122
#define SRST_P_MAILBOX0			123
#define SRST_P_GPIO1			124

#define SRST_GPIO1			125
#define SRST_P_GPIO2			126
#define SRST_GPIO2			127
#define SRST_P_GPIO3			128
#define SRST_GPIO3			129
#define SRST_P_GPIO4			130
#define SRST_GPIO4			131
#define SRST_A_DECOM			132
#define SRST_P_DECOM			133
#define SRST_D_DECOM			134
#define SRST_TIMER6			135
#define SRST_TIMER7			136
#define SRST_TIMER8			137
#define SRST_TIMER9			138
#define SRST_TIMER10			139

#define SRST_TIMER11			140
#define SRST_A_DMAC0			141
#define SRST_A_DMAC1			142
#define SRST_A_DMAC2			143
#define SRST_A_SPINLOCK			144
#define SRST_REF_PVTPLL_BUS		145
#define SRST_H_I3C0			146
#define SRST_H_I3C1			147
#define SRST_H_BUS_CM0_BIU		148
#define SRST_F_BUS_CM0_CORE		149
#define SRST_T_BUS_CM0_JTAG		150

#define SRST_P_INTMUX2PMU		151
#define SRST_P_INTMUX2DDR		152
#define SRST_P_PVTPLL_BUS		153
#define SRST_P_PWM2			154
#define SRST_PWM2			155
#define SRST_FREQ_PWM1			156
#define SRST_COUNTER_PWM1		157
#define SRST_I3C0			158
#define SRST_I3C1			159

#define SRST_P_DDR_MON_CH0		160
#define SRST_P_DDR_BIU			161
#define SRST_P_DDR_UPCTL_CH0		162
#define SRST_TM_DDR_MON_CH0		163
#define SRST_A_DDR_BIU			164
#define SRST_DFI_CH0			165
#define SRST_DDR_MON_CH0		166
#define SRST_P_DDR_HWLP_CH0		167
#define SRST_P_DDR_MON_CH1		168
#define SRST_P_DDR_HWLP_CH1		169

#define SRST_P_DDR_UPCTL_CH1		170
#define SRST_TM_DDR_MON_CH1		171
#define SRST_DFI_CH1			172
#define SRST_A_DDR01_MSCH0		173
#define SRST_A_DDR01_MSCH1		174
#define SRST_DDR_MON_CH1		175
#define SRST_DDR_SCRAMBLE_CH0		176
#define SRST_DDR_SCRAMBLE_CH1		177
#define SRST_P_AHB2APB			178
#define SRST_H_AHB2APB			179
#define SRST_H_DDR_BIU			180
#define SRST_F_DDR_CM0_CORE		181

#define SRST_P_DDR01_MSCH0		182
#define SRST_P_DDR01_MSCH1		183
#define SRST_DDR_TIMER0			184
#define SRST_DDR_TIMER1			185
#define SRST_T_WDT_DDR			186
#define SRST_P_WDT			187
#define SRST_P_TIMER			188
#define SRST_T_DDR_CM0_JTAG		189
#define SRST_P_DDR_GRF			190

#define SRST_DDR_UPCTL_CH0		191
#define SRST_A_DDR_UPCTL_0_CH0		192
#define SRST_A_DDR_UPCTL_1_CH0		193
#define SRST_A_DDR_UPCTL_2_CH0		194
#define SRST_A_DDR_UPCTL_3_CH0		195
#define SRST_A_DDR_UPCTL_4_CH0		196

#define SRST_DDR_UPCTL_CH1		197
#define SRST_A_DDR_UPCTL_0_CH1		198
#define SRST_A_DDR_UPCTL_1_CH1		199
#define SRST_A_DDR_UPCTL_2_CH1		200
#define SRST_A_DDR_UPCTL_3_CH1		201
#define SRST_A_DDR_UPCTL_4_CH1		202

#define SRST_REF_PVTPLL_DDR		203
#define SRST_P_PVTPLL_DDR		204

#define SRST_A_RKNN0			205
#define SRST_A_RKNN0_BIU		206
#define SRST_L_RKNN0_BIU		207

#define SRST_A_RKNN1			208
#define SRST_A_RKNN1_BIU		209
#define SRST_L_RKNN1_BIU		210

#define SRST_NPU_DAP			211
#define SRST_L_NPUSUBSYS_BIU		212
#define SRST_P_NPUTOP_BIU		213
#define SRST_P_NPU_TIMER		214
#define SRST_NPUTIMER0			215
#define SRST_NPUTIMER1			216
#define SRST_P_NPU_WDT			217
#define SRST_T_NPU_WDT			218

#define SRST_A_RKNN_CBUF		219
#define SRST_A_RVCORE0			220
#define SRST_P_NPU_GRF			221
#define SRST_P_PVTPLL_NPU		222
#define SRST_NPU_PVTPLL			223
#define SRST_H_NPU_CM0_BIU		224
#define SRST_F_NPU_CM0_CORE		225
#define SRST_T_NPU_CM0_JTAG		226
#define SRST_A_RKNNTOP_BIU		227
#define SRST_H_RKNN_CBUF		228
#define SRST_H_RKNNTOP_BIU		229

#define SRST_H_NVM_BIU			230
#define SRST_A_NVM_BIU			231
#define SRST_S_FSPI			232
#define SRST_H_FSPI			233
#define SRST_C_EMMC			234
#define SRST_H_EMMC			235
#define SRST_A_EMMC			236
#define SRST_B_EMMC			237
#define SRST_T_EMMC			238

#define SRST_P_GRF			239
#define SRST_P_PHP_BIU			240
#define SRST_A_PHP_BIU			241
#define SRST_P_PCIE0			242
#define SRST_PCIE0_POWER_UP		243

#define SRST_A_USB3OTG1			244
#define SRST_A_MMU0			245
#define SRST_A_SLV_MMU0			246
#define SRST_A_MMU1			247

#define SRST_A_SLV_MMU1			248
#define SRST_P_PCIE1			249
#define SRST_PCIE1_POWER_UP		250

#define SRST_RXOOB0			251
#define SRST_RXOOB1			252
#define SRST_PMALIVE0			253
#define SRST_PMALIVE1			254
#define SRST_A_SATA0			255
#define SRST_A_SATA1			256
#define SRST_ASIC1			257
#define SRST_ASIC0			258

#define SRST_P_CSIDPHY1			259
#define SRST_SCAN_CSIDPHY1		260

#define SRST_P_SDGMAC_GRF		261
#define SRST_P_SDGMAC_BIU		262
#define SRST_A_SDGMAC_BIU		263
#define SRST_H_SDGMAC_BIU		264
#define SRST_A_GMAC0			265
#define SRST_A_GMAC1			266
#define SRST_P_GMAC0			267
#define SRST_P_GMAC1			268
#define SRST_H_SDIO			269

#define SRST_H_SDMMC0			270
#define SRST_S_FSPI1			271
#define SRST_H_FSPI1			272
#define SRST_A_DSMC_BIU			273
#define SRST_A_DSMC			274
#define SRST_P_DSMC			275
#define SRST_H_HSGPIO			276
#define SRST_HSGPIO			277
#define SRST_A_HSGPIO			278

#define SRST_H_RKVDEC			279
#define SRST_H_RKVDEC_BIU		280
#define SRST_A_RKVDEC_BIU		281
#define SRST_RKVDEC_HEVC_CA		282
#define SRST_RKVDEC_CORE		283

#define SRST_A_USB_BIU			284
#define SRST_P_USBUFS_BIU		285
#define SRST_A_USB3OTG0			286
#define SRST_A_UFS_BIU			287
#define SRST_A_MMU2			288
#define SRST_A_SLV_MMU2			289
#define SRST_A_UFS_SYS			290

#define SRST_A_UFS			291
#define SRST_P_USBUFS_GRF		292
#define SRST_P_UFS_GRF			293

#define SRST_H_VPU_BIU			294
#define SRST_A_JPEG_BIU			295
#define SRST_A_RGA_BIU			296
#define SRST_A_VDPP_BIU			297
#define SRST_A_EBC_BIU			298
#define SRST_H_RGA2E_0			299
#define SRST_A_RGA2E_0			300
#define SRST_CORE_RGA2E_0		301

#define SRST_A_JPEG			302
#define SRST_H_JPEG			303
#define SRST_H_VDPP			304
#define SRST_A_VDPP			305
#define SRST_CORE_VDPP			306
#define SRST_H_RGA2E_1			307
#define SRST_A_RGA2E_1			308
#define SRST_CORE_RGA2E_1		309
#define SRST_H_EBC			310
#define SRST_A_EBC			311
#define SRST_D_EBC			312

#define SRST_H_VEPU0_BIU		313
#define SRST_A_VEPU0_BIU		314
#define SRST_H_VEPU0			315
#define SRST_A_VEPU0			316
#define SRST_VEPU0_CORE			317

#define SRST_A_VI_BIU			318
#define SRST_H_VI_BIU			319
#define SRST_P_VI_BIU			320
#define SRST_D_VICAP			321
#define SRST_A_VICAP			322
#define SRST_H_VICAP			323
#define SRST_ISP0			324
#define SRST_ISP0_VICAP			325

#define SRST_CORE_VPSS			326
#define SRST_P_CSI_HOST_0		327
#define SRST_P_CSI_HOST_1		328
#define SRST_P_CSI_HOST_2		329
#define SRST_P_CSI_HOST_3		330
#define SRST_P_CSI_HOST_4		331

#define SRST_CIFIN			332
#define SRST_VICAP_I0CLK		333
#define SRST_VICAP_I1CLK		334
#define SRST_VICAP_I2CLK		335
#define SRST_VICAP_I3CLK		336
#define SRST_VICAP_I4CLK		337

#define SRST_A_VOP_BIU			338
#define SRST_A_VOP2_BIU			339
#define SRST_H_VOP_BIU			340
#define SRST_P_VOP_BIU			341
#define SRST_H_VOP			342
#define SRST_A_VOP			343
#define SRST_D_VP0			344

#define SRST_D_VP1			345
#define SRST_D_VP2			346
#define SRST_P_VOP2_BIU			347
#define SRST_P_VOPGRF			348

#define SRST_H_VO0_BIU			349
#define SRST_P_VO0_BIU			350
#define SRST_A_HDCP0_BIU		351
#define SRST_P_VO0_GRF			352
#define SRST_A_HDCP0			353
#define SRST_H_HDCP0			354
#define SRST_HDCP0			355

#define SRST_P_DSIHOST0			356
#define SRST_DSIHOST0			357
#define SRST_P_HDMITX0			358
#define SRST_HDMITX0_REF		359
#define SRST_P_EDP0			360
#define SRST_EDP0_24M			361

#define SRST_M_SAI5_8CH			362
#define SRST_H_SAI5_8CH			363
#define SRST_M_SAI6_8CH			364
#define SRST_H_SAI6_8CH			365
#define SRST_H_SPDIF_TX2		366
#define SRST_M_SPDIF_TX2		367
#define SRST_H_SPDIF_RX2		368
#define SRST_M_SPDIF_RX2		369

#define SRST_H_SAI8_8CH			370
#define SRST_M_SAI8_8CH			371

#define SRST_H_VO1_BIU			372
#define SRST_P_VO1_BIU			373
#define SRST_M_SAI7_8CH			374
#define SRST_H_SAI7_8CH			375
#define SRST_H_SPDIF_TX3		376
#define SRST_H_SPDIF_TX4		377
#define SRST_H_SPDIF_TX5		378
#define SRST_M_SPDIF_TX3		379

#define SRST_DP0			380
#define SRST_P_VO1_GRF			381
#define SRST_A_HDCP1_BIU		382
#define SRST_A_HDCP1			383
#define SRST_H_HDCP1			384
#define SRST_HDCP1			385
#define SRST_H_SAI9_8CH			386
#define SRST_M_SAI9_8CH			387
#define SRST_M_SPDIF_TX4		388
#define SRST_M_SPDIF_TX5		389

#define SRST_GPU			390
#define SRST_A_S_GPU_BIU		391
#define SRST_A_M0_GPU_BIU		392
#define SRST_P_GPU_BIU			393
#define SRST_P_GPU_GRF			394
#define SRST_GPU_PVTPLL			395
#define SRST_P_PVTPLL_GPU		396

#define SRST_A_CENTER_BIU		397
#define SRST_A_DMA2DDR			398
#define SRST_A_DDR_SHAREMEM		399
#define SRST_A_DDR_SHAREMEM_BIU		400
#define SRST_H_CENTER_BIU		401
#define SRST_P_CENTER_GRF		402
#define SRST_P_DMA2DDR			403
#define SRST_P_SHAREMEM			404
#define SRST_P_CENTER_BIU		405

#define SRST_LINKSYM_HDMITXPHY0		406

#define SRST_DP0_PIXELCLK		407
#define SRST_PHY_DP0_TX			408
#define SRST_DP1_PIXELCLK		409
#define SRST_DP2_PIXELCLK		410

#define SRST_H_VEPU1_BIU		411
#define SRST_A_VEPU1_BIU		412
#define SRST_H_VEPU1			413
#define SRST_A_VEPU1			414
#define SRST_VEPU1_CORE			415

#endif
