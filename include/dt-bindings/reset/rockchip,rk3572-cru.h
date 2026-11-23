/* SPDX-License-Identifier: (GPL-2.0-only OR MIT) */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 * Author: Elaine Zhang <zhangqing@rock-chips.com>
 */

#ifndef _DT_BINDINGS_RESET_ROCKCHIP_RK3572_H
#define _DT_BINDINGS_RESET_ROCKCHIP_RK3572_H

/* CRU-->SOFTRST_CON01 */
#define SRST_A_TOP_BIU                           0
#define SRST_P_TOP_BIU                           1
#define SRST_A_TOP_MID_BIU                       2
#define SRST_H_TOP_BIU                           3

/* CRU-->SOFTRST_CON02 */
#define SRST_H_TOP_FW                            4

/* CRU-->SOFTRST_CON06 */
#define SRST_TEST_JUDGE_24M_TOP                  5
#define SRST_TEST_JUDGE_TOP                      6

/* CRU-->SOFTRST_CON07 */
#define SRST_H_AUDIO_BIU                         7
#define SRST_H_ASRC_2CH_0                        8
#define SRST_H_ASRC_2CH_1                        9
#define SRST_ASRC_2CH_0                          10
#define SRST_ASRC_2CH_1                          11
#define SRST_M_SAI0                              12
#define SRST_H_SAI0                              13
#define SRST_H_SPDIF_RX0                         14
#define SRST_M_SPDIF_RX0                         15

/* CRU-->SOFTRST_CON08 */
#define SRST_M_SAI1                              16
#define SRST_H_SAI1                              17
#define SRST_M_SAI2                              18
#define SRST_H_SAI2                              19
#define SRST_M_SAI3                              20
#define SRST_H_SAI3                              21
#define SRST_H_ACDCDIG_DSM                       22

/* CRU-->SOFTRST_CON09 */
#define SRST_M_ACDCDIG_DSM                       23
#define SRST_P_AUDIO_BIU                         24
#define SRST_P_AUDIO_GRF                         25
#define SRST_H_SPDIF_TX0                         26
#define SRST_M_SPDIF_TX0                         27
#define SRST_H_SPDIF_TX1                         28
#define SRST_M_SPDIF_TX1                         29
#define SRST_TEST_JUDGE_24M_AUDIO                30
#define SRST_TEST_JUDGE_AUDIO                    31

/* CRU-->SOFTRST_CON11 */
#define SRST_A_BUS_BIU                           32
#define SRST_A_BUS_FW                            33
#define SRST_P_BUS_BIU                           34
#define SRST_P_CRU                               35
#define SRST_H_CAN0                              36
#define SRST_CAN0                                37
#define SRST_H_CAN1                              38
#define SRST_CAN1                                39
#define SRST_H_CAN2                              40
#define SRST_CAN2                                41
#define SRST_H_CAN3                              42
#define SRST_CAN3                                43

/* CRU-->SOFTRST_CON12 */
#define SRST_A_DECOM_BIU                         44
#define SRST_H_BUS_BIU                           45
#define SRST_KEY_SHIFT                           46
#define SRST_P_I2C1                              47
#define SRST_P_I2C2                              48
#define SRST_P_I2C3                              49
#define SRST_P_I2C4                              50
#define SRST_P_I2C5                              51
#define SRST_P_I2C6                              52
#define SRST_P_I2C7                              53
#define SRST_P_I2C8                              54
#define SRST_P_I2C9                              55
#define SRST_P_WDT_BUSMCU                        56
#define SRST_T_WDT_BUSMCU                        57

/* CRU-->SOFTRST_CON13 */
#define SRST_A_GIC                               58
#define SRST_I2C1                                59
#define SRST_I2C2                                60
#define SRST_I2C3                                61
#define SRST_I2C4                                62
#define SRST_I2C5                                63
#define SRST_I2C6                                64
#define SRST_I2C7                                65
#define SRST_I2C8                                66
#define SRST_I2C9                                67
#define SRST_P_SARADC                            68

/* CRU-->SOFTRST_CON14 */
#define SRST_SARADC                              69
#define SRST_SARADC_32K                          70
#define SRST_P_TSADC                             71
#define SRST_TSADC                               72
#define SRST_P_UART0                             73
#define SRST_P_UART2                             74
#define SRST_P_UART3                             75
#define SRST_P_UART4                             76
#define SRST_P_UART5                             77
#define SRST_P_UART6                             78
#define SRST_P_UART7                             79
#define SRST_P_UART8                             80
#define SRST_P_UART9                             81
#define SRST_P_UART10                            82
#define SRST_P_UART11                            83
#define SRST_S_UART0                             84

/* CRU-->SOFTRST_CON15 */
#define SRST_S_UART2                             85
#define SRST_S_UART3                             86
#define SRST_S_UART4                             87
#define SRST_S_UART5                             88
#define SRST_S_UART6                             89
#define SRST_S_UART7                             90

/* CRU-->SOFTRST_CON16 */
#define SRST_S_UART8                             91
#define SRST_S_UART9                             92
#define SRST_S_UART10                            93
#define SRST_S_UART11                            94
#define SRST_P_SPI0                              95
#define SRST_P_SPI1                              96
#define SRST_P_SPI2                              97
#define SRST_P_SPI3                              98
#define SRST_P_SPI4                              99
#define SRST_SPI0                                100
#define SRST_SPI1                                101
#define SRST_SPI2                                102
#define SRST_SPI3                                103

/* CRU-->SOFTRST_CON17 */
#define SRST_SPI4                                104
#define SRST_P_WDT0                              105
#define SRST_T_WDT0                              106
#define SRST_P_SYS_GRF                           107
#define SRST_P_PWM1                              108
#define SRST_PWM1                                109
#define SRST_P_PWM2                              110
#define SRST_PWM2                                111

/* CRU-->SOFTRST_CON18 */
#define SRST_P_BUSTIMER0                         112
#define SRST_P_BUSTIMER1                         113
#define SRST_TIMER0                              114
#define SRST_TIMER1                              115
#define SRST_TIMER2                              116
#define SRST_TIMER3                              117
#define SRST_TIMER4                              118
#define SRST_TIMER5                              119
#define SRST_P_MAILBOX0                          120

/* CRU-->SOFTRST_CON19 */
#define SRST_P_GPIO1                             121
#define SRST_DB_GPIO1                            122
#define SRST_P_GPIO2                             123
#define SRST_DB_GPIO2                            124
#define SRST_P_GPIO3                             125
#define SRST_DB_GPIO3                            126
#define SRST_P_GPIO4                             127
#define SRST_DB_GPIO4                            128
#define SRST_A_DECOM                             129
#define SRST_P_DECOM                             130
#define SRST_D_DECOM                             131
#define SRST_TIMER6                              132
#define SRST_TIMER7                              133
#define SRST_TIMER8                              134
#define SRST_TIMER9                              135

/* CRU-->SOFTRST_CON20 */
#define SRST_TIMER10                             136
#define SRST_TIMER11                             137
#define SRST_A_RKDMA0                            138
#define SRST_P_RKDMA0                            139
#define SRST_A_RKDMA1                            140
#define SRST_P_RKDMA1                            141
#define SRST_A_RKDMA2                            142
#define SRST_P_RKDMA2                            143
#define SRST_A_RKDMA3                            144
#define SRST_P_RKDMA3                            145
#define SRST_P_SPINLOCK                          146
#define SRST_REF_PVTPLL_BUS                      147
#define SRST_I3C                                 148
#define SRST_H_I3C                               149

/* CRU-->SOFTRST_CON21 */
#define SRST_BUSMCU_BIU                          150
#define SRST_CORE_N320                           151
#define SRST_PO_N320                             152
#define SRST_MCU_AXI_INTEGRITY                   153
#define SRST_P_INTMUX2PMU                        154
#define SRST_P_INTMUX2DDR                        155
#define SRST_P_PVTPLL_BUS                        156
#define SRST_HDMITXHPD                           157

/* CRU-->SOFTRST_CON22 */
#define SRST_P_APB2ASB_HDMITX                    158
#define SRST_S_APB2ASB_HDMITX                    159
#define SRST_P_APB2ASB_MIPICSI                   160
#define SRST_S_APB2ASB_MIPICSI                   161
#define SRST_P_APB2ASB_VCCIO0_3                  162
#define SRST_S_APB2ASB_VCCIO0_3                  163
#define SRST_P_APB2ASB_VCCIO1_2_4                164
#define SRST_S_APB2ASB_VCCIO1_2_4                165
#define SRST_P_APB2ASB_VCCIO5_6                  166
#define SRST_S_APB2ASB_VCCIO5_6                  167
#define SRST_TEST_JUDGE_24M_BUS                  168
#define SRST_TEST_JUDGE_BUS                      169

/* CRU-->SOFTRST_CON23 */
#define SRST_P_DDR_MON                           170
#define SRST_P_DDR_BIU                           171
#define SRST_P_DDR_UPCTL                         172
#define SRST_TM_DDR_MON                          173
#define SRST_A_DDR_BIU                           174
#define SRST_P_DDR_BIU_FW                        175
#define SRST_DDR_MON                             176
#define SRST_P_DDR_HWLP                          177

/* CRU-->SOFTRST_CON24 */
#define SRST_A_DDR_MSCH_BIU                      178
#define SRST_A_DDR_MSCH_FW                       179
#define SRST_DDR_SCRAMBLE                        180
#define SRST_L_DDRSCH_BIU                        181

/* CRU-->SOFTRST_CON25 */
#define SRST_DDR_TIMER0                          182
#define SRST_DDR_TIMER1                          183
#define SRST_T_DDR_WDT                           184
#define SRST_P_WDT                               185
#define SRST_P_TIMER                             186
#define SRST_P_DDR_GRF                           187

/* CRU-->SOFTRST_CON26 */
#define SRST_DDR_UPCTL                           188
#define SRST_A_DDR_UPCTL_0                       189
#define SRST_A_DDR_UPCTL_1                       190
#define SRST_A_DDR_UPCTL_2                       191
#define SRST_A_DDR_UPCTL_3                       192
#define SRST_A_DDR_UPCTL_4                       193
#define SRST_A_DDR_UPCTL_5                       194

/* CRU-->SOFTRST_CON27 */
#define SRST_DDRMCU_BIU                          195
#define SRST_CORE_N320_DDR                       196
#define SRST_PO_N320_DDR                         197
#define SRST_TEST_JUDGE_24M_DDR                  198
#define SRST_TEST_JUDGE_DDR                      199

/* CRU-->SOFTRST_CON31 */
#define SRST_A_RKNN                              200
#define SRST_A_RKNN_RV                           201
#define SRST_A_RKNN_BIU                          202
#define SRST_A_RKNN_FW                           203
#define SRST_P_RKNN_BIU                          204
#define SRST_P_PVTPLL_NPU                        205
#define SRST_NPU_PVTPLL                          206
#define SRST_24M_RKNN                            207
#define SRST_NPUTIMER0                           208
#define SRST_NPUTIMER1                           209
#define SRST_P_NPU_TIMER                         210

/* CRU-->SOFTRST_CON32 */
#define SRST_P_NPU_WDT                           211
#define SRST_T_NPU_WDT                           212
#define SRST_P_RKNN_GRF                          213
#define SRST_TEST_JUDGE_24M_NPU                  214
#define SRST_TEST_JUDGE_NPU                      215

/* CRU-->SOFTRST_CON33 */
#define SRST_P_PHP0_GRF                          216
#define SRST_P_PHP0_BIU                          217
#define SRST_A_PHP0_BIU                          218
#define SRST_A_PHP0_FW                           219
#define SRST_P_PCIE1                             220
#define SRST_PCIE1_POWER_UP                      221
#define SRST_TEST_JUDGE_24M_PHP0                 222
#define SRST_TEST_JUDGE_PHP0                     223

/* CRU-->SOFTRST_CON34 */
#define SRST_P_PCIE0                             224
#define SRST_PCIE0_POWER_UP                      225
#define SRST_A_USB3OTG1                          226
#define SRST_A_GMAC0                             227
#define SRST_A_USB3OTG0                          228

/* CRU-->SOFTRST_CON35 */
#define SRST_A_UFS_BIU                           229
#define SRST_A_UFS_SYS                           230
#define SRST_A_UFS                               231
#define SRST_P_UFS_GRF                           232

/* CRU-->SOFTRST_CON37 */
#define SRST_RXOOB0                              233
#define SRST_PMALIVE0                            234
#define SRST_A_SATA0                             235
#define SRST_RXOOB1                              236
#define SRST_PMALIVE1                            237
#define SRST_A_SATA1                             238
#define SRST_XPCS_PWR_ON                         239
#define SRST_ASIC0                               240
#define SRST_ASIC1                               241

/* CRU-->SOFTRST_CON42 */
#define SRST_P_NVM0_GRF                          242
#define SRST_P_NVM0_BIU                          243
#define SRST_A_NVM0_BIU                          244
#define SRST_H_NVM0_BIU                          245
#define SRST_C_EMMC                              246
#define SRST_H_EMMC                              247
#define SRST_A_EMMC                              248
#define SRST_B_EMMC                              249
#define SRST_T_EMMC                              250
#define SRST_A_GMAC1                             251

/* CRU-->SOFTRST_CON43 */
#define SRST_S_FSPI0                             252
#define SRST_H_FSPI0                             253
#define SRST_H_SDMMC1                            254
#define SRST_TEST_JUDGE_24M_NVM0                 255
#define SRST_TEST_JUDGE_NVM0                     256

/* CRU-->SOFTRST_CON44 */
#define SRST_A_DSMC_BIU                          257
#define SRST_A_DSMC                              258
#define SRST_H_DSMC_BIU                          259
#define SRST_H_DSMC                              260
#define SRST_P_DSMC_BIU                          261
#define SRST_P_DSMC                              262
#define SRST_TEST_JUDGE_24M_DSMC                 263
#define SRST_TEST_JUDGE_DSMC                     264
#define SRST_P_DSMC_GRF                          265

/* CRU-->SOFTRST_CON45 */
#define SRST_P_NVM1_GRF                          266
#define SRST_P_NVM1_BIU                          267
#define SRST_H_NVM1_BIU                          268
#define SRST_H_SDMMC0                            269
#define SRST_S_FSPI1                             270
#define SRST_H_FSPI1                             271

/* CRU-->SOFTRST_CON46 */
#define SRST_TEST_JUDGE_24M_NVM1                 272
#define SRST_TEST_JUDGE_NVM1                     273

/* CRU-->SOFTRST_CON50 */
#define SRST_H_RKVDEC                            274
#define SRST_H_RKVDEC_BIU                        275
#define SRST_A_RKVDEC_BIU                        276
#define SRST_RKVDEC_HEVC_CA                      277
#define SRST_RKVDEC_CORE                         278
#define SRST_TEST_JUDGE_24M_VDEC                 279
#define SRST_TEST_JUDGE_VDEC                     280
#define SRST_P_VDEC_BIU                          281
#define SRST_P_VDEC_GRF                          282

/* CRU-->SOFTRST_CON52 */
#define SRST_P_APB2ASB_MIPI_HDMI_SUBVO           283
#define SRST_S_APB2ASB_MIPI_HDMI_SUBVO           284
#define SRST_P_APB2ASB_VCCIO0_3_SUBVO            285
#define SRST_S_APB2ASB_VCCIO0_3_SUBVO            286
#define SRST_P_HDPTXPHY                          287
#define SRST_P_HDPTXPHY_GRF                      288
#define SRST_P_MIPI_DPHY0                        289

/* CRU-->SOFTRST_CON53 */
#define SRST_HDPTX_INIT                          290
#define SRST_HDPTX_CMN                           291
#define SRST_HDPTX_LANE                          292
#define SRST_P_IOC_SUBVO                         293
#define SRST_GMAC1_SUBVO_ASYNC                   294
#define SRST_TEST_JUDGE_24M_SUBSYSVO             295
#define SRST_TEST_JUDGE_SUBSYSVO                 296

/* CRU-->SOFTRST_CON54 */
#define SRST_P_APB2ASB_VCCIO1_2_4_SUBVI          297
#define SRST_S_APB2ASB_VCCIO1_2_4_SUBVI          298
#define SRST_P_APB2ASB_CSIPHY_SUBVI              299
#define SRST_S_APB2ASB_CSIPHY_SUBVI              300
#define SRST_P_CSIPHY0                           301
#define SRST_P_CSIPHY0_GRF                       302
#define SRST_P_CSIPHY1                           303
#define SRST_P_CSIPHY1_GRF                       304
#define SRST_P_IOC_SUBVI                         305
#define SRST_GMAC0_SUBVI_ASYNC                   306
#define SRST_SCAN_CSIPHY0                        307
#define SRST_SCAN_CSIPHY1                        308
#define SRST_TEST_JUDGE_24M_SUBSYSVI             309
#define SRST_TEST_JUDGE_SUBSYSVI                 310

/* CRU-->SOFTRST_CON56 */
#define SRST_P_APB2ASB_VCCIO5_6_SUBDSMC          311
#define SRST_S_APB2ASB_VCCIO5_6_SUBDSMC          312
#define SRST_P_VCCIO5_6                          313
#define SRST_GMAC1_ASYNC                         314

/* CRU-->SOFTRST_CON57 */
#define SRST_P_IMG_BIU                           315
#define SRST_P_IMG_GRF                           316
#define SRST_H_IMG_BIU                           317
#define SRST_A_JPEG_BIU                          318
#define SRST_TEST_JUDGE_24M_IMG                  319
#define SRST_TEST_JUDGE_IMG                      320
#define SRST_A_RGA_BIU                           321
#define SRST_A_VDPP_BIU                          322
#define SRST_A_EBC_BIU                           323

/* CRU-->SOFTRST_CON58 */
#define SRST_H_RGA2E_0                           324
#define SRST_A_RGA2E_0                           325
#define SRST_CORE_RGA2E_0                        326
#define SRST_A_JPEG                              327
#define SRST_H_JPEG                              328
#define SRST_H_VDPP                              329
#define SRST_A_VDPP                              330
#define SRST_CORE_VDPP                           331
#define SRST_H_EBC                               332
#define SRST_A_EBC                               333
#define SRST_D_EBC                               334

/* CRU-->SOFTRST_CON60 */
#define SRST_H_RKVENC_BIU                        335
#define SRST_A_RKVENC_BIU                        336
#define SRST_H_RKVENC                            337
#define SRST_A_RKVENC                            338
#define SRST_RKVENC_CORE                         339
#define SRST_TEST_JUDGE_24M_VENC                 340
#define SRST_TEST_JUDGE_VENC                     341

/* CRU-->SOFTRST_CON63 */
#define SRST_A_VI_BIU                            342
#define SRST_H_VI_BIU                            343
#define SRST_P_VI_BIU                            344
#define SRST_D_VICAP                             345
#define SRST_A_VICAP                             346
#define SRST_H_VICAP                             347
#define SRST_ISP                                 348
#define SRST_ISP_VICAP                           349

/* CRU-->SOFTRST_CON64 */
#define SRST_CORE_VPSS                           350
#define SRST_P_VI_GRF                            351
#define SRST_P_CSI_HOST_0                        352
#define SRST_P_CSI_HOST_1                        353
#define SRST_P_CSI_HOST_2                        354
#define SRST_P_CSI_HOST_3                        355
#define SRST_TEST_JUDGE_24M_VI                   356
#define SRST_TEST_JUDGE_VI                       357

/* CRU-->SOFTRST_CON72 */
#define SRST_CIFIN                               358

/* CRU-->SOFTRST_CON74 */
#define SRST_A_VOP_BIU                           359
#define SRST_A_VOP_FW                            360
#define SRST_H_VOP_BIU                           361
#define SRST_P_VOP_BIU                           362
#define SRST_H_VOP                               363
#define SRST_A_VOP                               364
#define SRST_D_VP0                               365

/* CRU-->SOFTRST_CON75 */
#define SRST_D_VP1                               366
#define SRST_P_VOPGRF                            367
#define SRST_TEST_JUDGE_24M_VOP                  368
#define SRST_TEST_JUDGE_VOP                      369

/* CRU-->SOFTRST_CON78 */
#define SRST_P_DSIHOST0                          370
#define SRST_H_VO_BIU                            371
#define SRST_P_VO_BIU                            372
#define SRST_A_HDCP0_BIU                         373
#define SRST_P_VO_GRF                            374

/* CRU-->SOFTRST_CON79 */
#define SRST_P_HDCP0_TRNG_NS                     375
#define SRST_A_HDCP0                             376
#define SRST_H_HDCP0                             377
#define SRST_HDCP0                               378
#define SRST_P_HDMITX0                           379
#define SRST_HDMITX0_EARC                        380

/* CRU-->SOFTRST_CON80 */
#define SRST_HDMITX0_REF                         381
#define SRST_P_EDP0                              382
#define SRST_EDP0_24M                            383
#define SRST_M_SAI4                              384
#define SRST_H_SAI4                              385
#define SRST_M_SAI5                              386
#define SRST_H_SAI5                              387
#define SRST_H_SPDIF_TX2                         388
#define SRST_M_SPDIF_TX2                         389
#define SRST_H_SPDIF_RX1                         390
#define SRST_M_SPDIF_RX1                         391

/* CRU-->SOFTRST_CON81 */
#define SRST_EBC_VOP_DCLK_VO                     392

/* CRU-->SOFTRST_CON82 */
#define SRST_TEST_JUDGE_24M_VO                   393
#define SRST_TEST_JUDGE_VO                       394

/* CRU-->SOFTRST_CON84 */
#define SRST_GPU                                 395
#define SRST_SYS_GPU                             396
#define SRST_P_GPU_BIU                           397
#define SRST_PO_GPU_JTAG                         398
#define SRST_P_GPU_GRF                           399
#define SRST_GPU_PVTPLL                          400
#define SRST_P_PVTPLL_GPU                        401
#define SRST_A_SLV_GPU_BIU                       402

/* CRU-->SOFTRST_CON85 */
#define SRST_A_GPU_BIU                           403
#define SRST_TEST_JUDGE_24M_GPU                  404
#define SRST_TEST_JUDGE_GPU                      405

/* CRU-->SOFTRST_CON86 */
#define SRST_A_CENTER_BIU                        406
#define SRST_A_CENTER_FW_DDR                     407
#define SRST_A_CENTER_FW_GPU                     408
#define SRST_A_DMA2DDR                           409
#define SRST_A_DDR_SHAREMEM                      410
#define SRST_A_DDR_SHAREMEM_BIU                  411
#define SRST_A_DDR_SHAREMEM_FW                   412
#define SRST_H_CENTER_BIU                        413
#define SRST_H_CENTER_FW                         414
#define SRST_P_CENTER_GRF                        415
#define SRST_P_DMA2DDR                           416

/* CRU-->SOFTRST_CON87 */
#define SRST_P_CENTER_BIU                        417
#define SRST_TEST_JUDGE_24M_CENTER               418
#define SRST_TEST_JUDGE_CENTER                   419
#define SRST_LINKSYM_HDMITXPHY0                  420

/* PHPPHY_CRU-->SOFTRST_CON00 */
#define SRST_P_PHPPHY_CRU                        421
#define SRST_P_PCIE2_COMBOPHY0                   422
#define SRST_P_PCIE2_COMBOPHY0_GRF               423
#define SRST_P_PCIE2_COMBOPHY1                   424
#define SRST_P_PCIE2_COMBOPHY1_GRF               425
#define SRST_P_USBPHY_GRF_0                      426
#define SRST_P_USBPHY_GRF_1                      427
#define SRST_MPHY_INIT                           428
#define SRST_P_MPHY_GRF                          429
#define SRST_P_VCCIO7_IOC                        430
#define SRST_USB2DEBUG                           431
#define SRST_P_PCIEPHY2                          432
#define SRST_P_PCIEPHY2_GRF                      433

/* PHPPHY_CRU-->SOFTRST_CON01 */
#define SRST_S_APB2ASB_USBMPHY_PHPPHY            434
#define SRST_P_APB2ASB_USBMPHY_PHPPHY            435
#define SRST_OTGPHY_0                            436
#define SRST_OTGPHY_1                            437
#define SRST_TEST_JUDGE_24M_PHPPHY               438
#define SRST_TEST_JUDGE_PHPPHY                   439

/* PHPPHY_CRU-->SOFTRST_CON02 */
#define SRST_PCIEPHY2                            440
#define SRST_PCIE0_PIPE_PHY                      441
#define SRST_PCIE1_PIPE_PHY                      442

/* SECURE_CRU-->SOFTRST_CON00 */
#define SRST_H_CRYPTO_NS                         443
#define SRST_H_TRNG_NS                           444
#define SRST_P_SECURE_NS_BIU                     445
#define SRST_A_SECURE_HIGH_BIU                   446
#define SRST_H_SECURE_NS_BIU                     447
#define SRST_P_OTPC_NS                           448
#define SRST_OTPC_NS                             449

/* PMU1_CRU-->SOFTRST_CON03 */
#define SRST_P_APB2ASB_PMU1                      450
#define SRST_S_APB2ASB_PMU1                      451
#define SRST_H_PMU1_BIU                          452
#define SRST_H_PMU1_FW                           453
#define SRST_P_PMU1_BIU                          454
#define SRST_H_PMU_CM0_BIU                       455
#define SRST_CORE_N100                           456
#define SRST_PO_N100                             457

/* PMU1_CRU-->SOFTRST_CON04 */
#define SRST_P_CRU_PMU1                          458
#define SRST_P_PMU1_GRF                          459
#define SRST_P_PMU1_IOC                          460
#define SRST_P_PMU1WDT                           461
#define SRST_T_PMU1WDT                           462
#define SRST_P_PMUTIMER                          463
#define SRST_PMUTIMER0                           464
#define SRST_PMUTIMER1                           465
#define SRST_P_PWM0                              466
#define SRST_PWM0                                467

/* PMU1_CRU-->SOFTRST_CON05 */
#define SRST_P_I2C0                              468
#define SRST_I2C0                                469
#define SRST_S_UART1                             470
#define SRST_P_UART1                             471
#define SRST_TEST_JUDGE_24M_PMU1                 472
#define SRST_TEST_JUDGE_PMU1                     473
#define SRST_PDM0                                474

/* PMU1_CRU-->SOFTRST_CON06 */
#define SRST_H_PDM0                              475
#define SRST_M_PDM0                              476
#define SRST_H_VAD                               477
#define SRST_P_RCOSC                             478
#define SRST_RCOSC                               479

/* PMU1_CRU-->SOFTRST_CON07 */
#define SRST_P_PMU0GRF                           480
#define SRST_P_PMU0IOC                           481
#define SRST_P_GPIO0                             482
#define SRST_DB_GPIO0                            483

/* DDR0_CRU-->SOFTRST_CON00 */
#define SRST_DDRPHY1X                            484

/* DDR0_CRU-->SOFTRST_CON01 */
#define SRST_P_DDRPHY_CRU                        485
#define SRST_P_DDRPHY                            486

/* BIGCORE_CRU-->SOFTRST_CON00 */
#define SRST_REF_PVTPLL_BIGCORE                  487
#define SRST_P_BIGCORE_BIU                       488
#define SRST_P_BIGCORE_GRF                       489
#define SRST_P_BIGCORE_CRU                       490
#define SRST_P_PVTPLL_BIGCORE                    491
#define SRST_NBIGCOREPORESET0                    492
#define SRST_NBIGCOREPORESET1                    493
#define SRST_NBIGCORESET0                        494
#define SRST_NBIGCORESET1                        495

/* BIGCORE_CRU-->SOFTRST_CON01 */
#define SRST_NL2RESET_BIGCORE                    496
#define SRST_A_ADB400_A73_ACE                    497
#define SRST_TEST_JUDGE_24M_BIGCORE              498
#define SRST_TEST_JUDGE_BIGCORE                  499
#define SRST_P_DBG_BIGCORE                       500

/* LITCORE0_CRU-->SOFTRST_CON00 */
#define SRST_REF_LITCORE0_PVTPLL                 501
#define SRST_NLITCOREPORESET0                    502
#define SRST_NLITCOREPORESET1                    503
#define SRST_NLITCOREPORESET2                    504
#define SRST_NLITCOREPORESET3                    505
#define SRST_NLITCORESET0                        506
#define SRST_NLITCORESET1                        507
#define SRST_NLITCORESET2                        508
#define SRST_NLITCORESET3                        509
#define SRST_NL2RESET_LITCORE0                   510
#define SRST_P_DBG_LITCORE0                      511
#define SRST_A_ADB400_A53_ACE_LITCORE0           512

/* LITCORE0_CRU-->SOFTRST_CON01 */
#define SRST_P_LITCORE0_GRF                      513
#define SRST_P_LITCORE0_BIU                      514
#define SRST_P_PVTPLL_LITCORE0                   515
#define SRST_P_LITCORE0_CRU                      516
#define SRST_TEST_JUDGE_24M_LITCORE0             517
#define SRST_TEST_JUDGE_LITCORE0                 518

/* LITCORE1_CRU-->SOFTRST_CON00 */
#define SRST_REF_LITCORE1_PVTPLL                 519
#define SRST_NLITCOREPORESET4                    520
#define SRST_NLITCOREPORESET5                    521
#define SRST_NLITCORESET4                        522
#define SRST_NLITCORESET5                        523
#define SRST_NL2RESET_LITCORE1                   524
#define SRST_P_DBG_LITCORE1                      525
#define SRST_A_ADB400_A53_ACE_LITCORE1           526
#define SRST_P_LITCORE1_GRF                      527
#define SRST_P_LITCORE1_BIU                      528

/* LITCORE1_CRU-->SOFTRST_CON01 */
#define SRST_P_PVTPLL_LITCORE1                   529
#define SRST_P_LITCORE1_CRU                      530
#define SRST_TEST_JUDGE_24M_LITCORE1             531
#define SRST_TEST_JUDGE_LITCORE1                 532

/* CCI_CRU-->SOFTRST_CON01 */
#define SRST_TEST_JUDGE_24M_CCI                  533
#define SRST_TEST_JUDGE_CCI                      534
#define SRST_A_CPE                               535
#define SRST_P_CCI_BIU                           536
#define SRST_P_CCI_FW                            537
#define SRST_A_CCI_BIU                           538
#define SRST_A_CCI_FW                            539

/* CCI_CRU-->SOFTRST_CON02 */
#define SRST_P_DBG_SYS                           540
#define SRST_P_CCI_GRF                           541
#define SRST_P_CCI_CRU                           542
#define SRST_A_CCI                               543
#define SRST_PO_JTAG                             544
#define SRST_P_DBG_M                             545
#define SRST_A_ADB400_CCI_A73                    546
#define SRST_A_ADB400_CCI_A53_0                  547
#define SRST_A_ADB400_CCI_A53_1                  548
#define SRST_P_PVTPLL_CCI                        549
#define SRST_P_CCI_SPINLOCK                      550
#define SRST_P_DBG_M_BIU                         551
#define SRST_REF_CCI_PVTPLL                      552

#endif
