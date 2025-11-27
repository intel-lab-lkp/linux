/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2025 Amlogic, Inc. All rights reserved.
 */

#ifndef _DT_BINDINGS_AMLOGIC_S7_RESET_H
#define _DT_BINDINGS_AMLOGIC_S7_RESET_H

/* RESET0 */
#define RESET_USB_DDR0			0
#define RESET_USB_DDR1			1
#define RESET_USB_DDR2			2
#define RESET_USB_DDR3			3
#define RESET_USB1			4
#define RESET_USB0			5
#define RESET_USB1_COMB			6
#define RESET_USB0_COMB			7
#define RESET_USBPHY20			8
#define RESET_USBPHY21			9
/*					10-14	*/
#define RESET_HDMI20_AES		15
#define RESET_HDMITX_CAPB3		16
#define RESET_BRG_VCBUS_DEC		17
#define RESET_VCBUS			18
#define RESET_VID_PLL_DIV		19
#define RESET_VIDEO6			20
#define RESET_GE2D			21
#define RESET_HDMITXPHY			22
#define RESET_VID_LOCK			23
#define RESET_VENCL			24
#define RESET_VDAC			25
#define RESET_VENCP			26
#define RESET_VENCI			27
#define RESET_RDMA			28
#define RESET_HDMI_TX			29
#define RESET_VIU			30
#define RESET_VENC			31

/* RESET1 */
#define RESET_AUDIO			32
#define RESET_MALI_CAPB3		33
#define RESET_MALI			34
#define RESET_DDR_APB			35
#define RESET_DDR			36
#define RESET_DOS_CAPB3			37
#define RESET_DOS			38
/*					39-47	*/
#define RESET_ETH			48
/*					49-63	*/

/* RESET2 */
#define RESET_AM2AXI			64
#define RESET_IR_CTRL			65
/*					66	*/
#define RESET_TEMPSENSOR_PLL		67
/*					68-71	*/
#define RESET_SMART_CARD		72
#define RESET_SPICC0			73
/*					74-79	*/
#define RESET_MSR_CLK			80
/*					81	*/
#define RESET_SARADC			82
/*					83-87	*/
#define RESET_ACODEC			88
#define RESET_CEC			89
/*					90	*/
#define RESET_WATCHDOG			91
/*					92-95	*/

/* RESET3 */
/* 96 ~ 127 */

/* RESET4 */
/*					128-131	*/
#define RESET_PWM_A			128
#define RESET_PWM_B			129
#define RESET_PWM_C			130
#define RESET_PWM_D			131
#define RESET_PWM_E			132
#define RESET_PWM_F			133
#define RESET_PWM_G			134
#define RESET_PWM_H			135
#define RESET_PWM_I			136
#define RESET_PWM_J			137
#define RESET_UART_A			138
#define RESET_UART_B			139
/*					140-143	*/
#define RESET_I2C_S_A			144
#define RESET_I2C_M_A			145
#define RESET_I2C_M_B			146
#define RESET_I2C_M_C			147
#define RESET_I2C_M_D			148
#define RESET_I2C_M_E			149
/*					150-151	*/
#define RESET_SD_EMMC_A			152
#define RESET_SD_EMMC_B			153
#define RESET_SD_EMMC_C			154
/*					155-159	*/

/* RESET5 */
#define RESET_BRG_VDEC_PIPL0		160
#define RESET_BRG_HEVCF_PIPL0		161
/*					162-163	*/
#define RESET_BRG_GE2D_PIPL0		164
#define RESET_BRG_DMC_PIPL0		165
#define RESET_BRG_A53_PIPL0		166
#define RESET_BRG_MALI_PIPL0		167
/*					168	*/
#define RESET_BRG_MALI_PIPL1		169
/*					170-171	*/
#define RESET_BRG_HEVCF_PIPL1		172
#define RESET_BRG_HEVCB_PIPL1		173
/*					174-182	*/
#define RESET_BRG_NIC_EMMC		183
#define RESET_BRG_NIC_RAMA		184
#define RESET_BRG_NIC_SDIOB		185
#define RESET_BRG_NIC_SDIOA		186
#define RESET_BRG_NIC_VAPB		187
#define RESET_BRG_NIC_DSU		188
#define RESET_BRG_NIC_CLK81		189
#define RESET_BRG_NIC_MAIN		190
#define RESET_BRG_NIC_ALL		191

#endif
