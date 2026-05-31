/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#ifndef _DT_BINDINGS_CLK_MSM_GCC_8660_H
#define _DT_BINDINGS_CLK_MSM_GCC_8660_H

#define AFAB_CLK_SRC                        0       /* header-only - AFAB (app fabric) src */
#define AFAB_CORE_CLK                       1       /* header-only - AFAB core */
#define SCSS_A_CLK                          2       /* header-only - Scorpion subsys AFAB */
#define SCSS_H_CLK                          3       /* header-only - Scorpion subsys AHB */
#define SCSS_XO_SRC_CLK                     4       /* header-only - Scorpion XO src */
#define AFAB_EBI1_CH0_A_CLK                 5       /* header-only - AFAB->EBI1 ch0 AFAB */
#define AFAB_EBI1_CH1_A_CLK                 6       /* header-only - AFAB->EBI1 ch1 AFAB */
#define AFAB_AXI_S0_FCLK                    7       /* header-only - AFAB AXI slave0 fabric */
#define AFAB_AXI_S1_FCLK                    8       /* header-only - AFAB AXI slave1 fabric */
#define AFAB_AXI_S2_FCLK                    9       /* header-only - AFAB AXI slave2 fabric */
#define AFAB_AXI_S3_FCLK                    10      /* header-only - AFAB AXI slave3 fabric */
#define AFAB_AXI_S4_FCLK                    11      /* header-only - AFAB AXI slave4 fabric */
#define SFAB_CORE_CLK                       12      /* header-only - SFAB (system fabric) core */
#define SFAB_AXI_S0_FCLK                    13      /* header-only - SFAB AXI slave0 fabric */
#define SFAB_AXI_S1_FCLK                    14      /* header-only - SFAB AXI slave1 fabric */
#define SFAB_AXI_S2_FCLK                    15      /* header-only - SFAB AXI slave2 fabric */
#define SFAB_AXI_S3_FCLK                    16      /* header-only - SFAB AXI slave3 fabric */
#define SFAB_AXI_S4_FCLK                    17      /* header-only - SFAB AXI slave4 fabric */
#define SFAB_AHB_S0_FCLK                    18      /* header-only - SFAB AHB slave0 fabric */
#define SFAB_AHB_S1_FCLK                    19      /* header-only - SFAB AHB slave1 fabric */
#define SFAB_AHB_S2_FCLK                    20      /* header-only - SFAB AHB slave2 fabric */
#define SFAB_AHB_S3_FCLK                    21      /* header-only - SFAB AHB slave3 fabric */
#define SFAB_AHB_S4_FCLK                    22      /* header-only - SFAB AHB slave4 fabric */
#define SFAB_AHB_S5_FCLK                    23      /* header-only - SFAB AHB slave5 fabric */
#define SFAB_AHB_S6_FCLK                    24      /* header-only - SFAB AHB slave6 fabric */
#define SFAB_ADM0_M0_A_CLK                  25      /* header-only - SFAB->ADM0 master0 AFAB */
#define SFAB_ADM0_M1_A_CLK                  26      /* header-only - SFAB->ADM0 master1 AFAB */
#define SFAB_ADM0_M2_A_CLK                  27      /* header-only - SFAB->ADM0 master2 AFAB */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - ADM0 (DMA controller) */
#define ADM0_CLK				28
#define ADM0_PBUS_CLK                       29      /* drv+dt - ADM0 APB */
#define SFAB_ADM1_M0_A_CLK                  30      /* header-only - SFAB->ADM1 master0 AFAB */
#define SFAB_ADM1_M1_A_CLK                  31      /* header-only - SFAB->ADM1 master1 AFAB */
#define SFAB_ADM1_M2_A_CLK                  32      /* header-only - SFAB->ADM1 master2 AFAB */
#define MMFAB_ADM1_M3_A_CLK                 33      /* header-only - MMFAB->ADM1 master3 AFAB */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - ADM1 (DMA controller) */
#define ADM1_CLK				34
#define ADM1_PBUS_CLK                       35      /* drv+dt - ADM1 APB */
#define IMEM0_A_CLK                         36      /* header-only - IMEM0 AFAB */
#define MAHB0_CLK                           37      /* header-only - MAHB0 */
#define SFAB_LPASS_Q6_A_CLK                 38      /* header-only - SFAB LPASS Q6 AFAB */
#define SFAB_AFAB_M_A_CLK                   39      /* header-only - SFAB->AFAB master AFAB */
#define AFAB_SFAB_M0_A_CLK                  40      /* header-only - AFAB->SFAB master0 AFAB */
#define AFAB_SFAB_M1_A_CLK                  41      /* header-only - AFAB->SFAB master1 AFAB */
#define DFAB_CLK_SRC                        42      /* header-only - DFAB (display fabric) src */
#define DFAB_CLK                            43      /* legacy:HTC,Sam - DFAB clock */
#define DFAB_CORE_CLK                       44      /* header-only - DFAB core */
#define SFAB_DFAB_M_A_CLK                   45      /* header-only - SFAB->DFAB master AFAB */
#define DFAB_SFAB_M_A_CLK                   46      /* header-only - DFAB->SFAB master AFAB */
#define DFAB_SWAY0_H_CLK                    47      /* header-only - DFAB sway0 AHB */
#define DFAB_SWAY1_H_CLK                    48      /* header-only - DFAB sway1 AHB */
#define DFAB_ARB0_H_CLK                     49      /* header-only - DFAB arb0 AHB */
#define DFAB_ARB1_H_CLK                     50      /* header-only - DFAB arb1 AHB */
/* drv+dt - PPSS (peripheral periodic sample subsys) AHB */
#define PPSS_H_CLK				51
#define PPSS_PROC_CLK                       52      /* header-only - PPSS CPU */
#define PPSS_TIMER0_CLK                     53      /* header-only - PPSS timer0 */
#define PPSS_TIMER1_CLK                     54      /* header-only - PPSS timer1 */
#define PMEM_A_CLK                          55      /* header-only - PMEM AFAB */
#define DMA_BAM_H_CLK                       56      /* header-only - DMA BAM AHB */
#define SIC_H_CLK                           57      /* header-only - SIC AHB */
#define SPS_TIC_H_CLK                       58      /* header-only - SPS TIC AHB */
#define SLIMBUS_H_CLK                       59      /* header-only - SLIMbus AHB */
#define SLIMBUS_XO_SRC_CLK                  60      /* header-only - SLIMbus XO src */
#define CFPB_2X_CLK_SRC                     61      /* header-only - CFPB 2x src */
/* legacy:HTC,Sam,TP - CFPB (config peripheral bridge) */
#define CFPB_CLK				62
#define CFPB0_H_CLK                         63      /* header-only - CFPB0 AHB */
#define CFPB1_H_CLK                         64      /* header-only - CFPB1 AHB */
#define CFPB2_H_CLK                         65      /* header-only - CFPB2 AHB */
#define EBI2_2X_CLK                         66      /* drv+dt legacy:HTC,Sam - EBI2 (NAND) 2x */
#define EBI2_CLK                            67      /* drv+dt legacy:HTC,Sam - EBI2 (NAND) */
#define SFAB_CFPB_M_H_CLK                   68      /* header-only - SFAB->CFPB master AHB */
#define CFPB_MASTER_H_CLK                   69      /* header-only - CFPB master AHB */
#define SFAB_CFPB_S_HCLK                    70      /* header-only - SFAB->CFPB slave AHB */
#define CFPB_SPLITTER_H_CLK                 71      /* header-only - CFPB splitter AHB */
#define TSIF_H_CLK                          72      /* drv - TSIF AHB */
#define TSIF_INACTIVITY_TIMERS_CLK          73      /* header-only - TSIF inactivity timer */
#define TSIF_REF_SRC                        74      /* drv legacy:TP,Pre3 - TSIF ref src */
#define TSIF_REF_CLK                        75      /* drv legacy:HTC,Sam,TP,Pre3 - TSIF ref */
#define CE1_H_CLK                           76      /* header-only - CE1 (crypto engine 1) AHB */
#define CE2_H_CLK                           77      /* drv+dt - CE2 AHB */
#define SFPB_H_CLK_SRC                      78      /* header-only - SFPB AHB src */
/* header-only - SFPB (system peripheral bridge) AHB */
#define SFPB_H_CLK				79
#define SFAB_SFPB_M_H_CLK                   80      /* header-only - SFAB->SFPB master AHB */
#define SFAB_SFPB_S_H_CLK                   81      /* header-only - SFAB->SFPB slave AHB */
#define RPM_PROC_CLK                        82      /* header-only - RPM CPU */
#define RPM_BUS_H_CLK                       83      /* header-only - RPM bus AHB */
#define RPM_SLEEP_CLK                       84      /* header-only - RPM sleep */
#define RPM_TIMER_CLK                       85      /* header-only - RPM timer */
#define MODEM_AHB1_H_CLK                    86      /* drv - modem AHB1 bridge */
#define MODEM_AHB2_H_CLK                    87      /* drv - modem AHB2 bridge */
#define RPM_MSG_RAM_H_CLK                   88      /* drv+dt - RPM msg-ram AHB */
#define SC_H_CLK                            89      /* header-only - SC AHB */
#define SC_A_CLK                            90      /* header-only - SC AFAB */
#define PMIC_ARB0_H_CLK                     91      /* drv - PMIC SSBI arb0 AHB */
#define PMIC_ARB1_H_CLK                     92      /* drv - PMIC SSBI arb1 AHB */
#define PMIC_SSBI2_SRC                      93      /* legacy:TP,Pre3 - PMIC SSBI2 src */
#define PMIC_SSBI2_CLK                      94      /* drv legacy:HTC,Sam,TP,Pre3 - PMIC SSBI2 */
#define SDC1_H_CLK                          95      /* drv+dt - eMMC1 AHB */
#define SDC2_H_CLK                          96      /* drv+dt - eMMC2 AHB */
#define SDC3_H_CLK                          97      /* drv+dt - eMMC3 AHB */
#define SDC4_H_CLK                          98      /* drv+dt - eMMC4 AHB */
#define SDC5_H_CLK                          99      /* drv+dt - eMMC5 AHB */
#define SDC1_SRC                            100     /* drv legacy:TP,Pre3 - eMMC1 src */
#define SDC2_SRC                            101     /* drv legacy:TP,Pre3 - eMMC2 src */
#define SDC3_SRC                            102     /* drv legacy:TP,Pre3 - eMMC3 src */
#define SDC4_SRC                            103     /* drv legacy:TP,Pre3 - eMMC4 src */
#define SDC5_SRC                            104     /* drv legacy:TP,Pre3 - eMMC5 src */
#define SDC1_CLK                            105     /* drv+dt legacy:HTC,Sam,TP,Pre3 - eMMC1 data */
#define SDC2_CLK                            106     /* drv+dt legacy:HTC,Sam,TP,Pre3 - eMMC2 data */
#define SDC3_CLK                            107     /* drv+dt legacy:HTC,Sam,TP,Pre3 - eMMC3 data */
#define SDC4_CLK                            108     /* drv+dt legacy:HTC,Sam,TP,Pre3 - eMMC4 data */
#define SDC5_CLK                            109     /* drv+dt legacy:HTC,Sam,TP,Pre3 - eMMC5 data */
#define USB_HS1_H_CLK                       110     /* drv+dt - USB HS1 AHB */
#define USB_HS1_XCVR_SRC                    111     /* drv legacy:TP - USB HS1 xcvr src */
#define USB_HS1_XCVR_CLK                    112     /* drv+dt legacy:HTC,Sam,TP - USB HS1 xcvr */
#define USB_HS2_H_CLK                       113     /* header-only - USB HS2 AHB */
#define USB_HS2_XCVR_SRC                    114     /* header-only - USB HS2 xcvr src */
#define USB_HS2_XCVR_CLK                    115     /* header-only - USB HS2 xcvr */
#define USB_FS1_H_CLK                       116     /* drv - USB FS1 AHB */
#define USB_FS1_XCVR_FS_SRC                 117     /* drv - USB FS1 xcvr src */
#define USB_FS1_XCVR_FS_CLK                 118     /* drv - USB FS1 xcvr */
#define USB_FS1_SYSTEM_CLK                  119     /* drv - USB FS1 system */
#define USB_FS2_H_CLK                       120     /* drv - USB FS2 AHB */
#define USB_FS2_XCVR_FS_SRC                 121     /* drv - USB FS2 xcvr src */
#define USB_FS2_XCVR_FS_CLK                 122     /* drv - USB FS2 xcvr */
#define USB_FS2_SYSTEM_CLK                  123     /* drv - USB FS2 system */
#define GSBI_COMMON_SIM_SRC                 124     /* header-only - GSBI SIM common src */
#define GSBI1_H_CLK                         125     /* drv+dt - GSBI1 AHB */
#define GSBI2_H_CLK                         126     /* drv+dt - GSBI2 AHB */
#define GSBI3_H_CLK                         127     /* drv+dt - GSBI3 AHB */
#define GSBI4_H_CLK                         128     /* drv+dt - GSBI4 AHB */
#define GSBI5_H_CLK                         129     /* drv+dt - GSBI5 AHB */
#define GSBI6_H_CLK                         130     /* drv+dt - GSBI6 AHB */
#define GSBI7_H_CLK                         131     /* drv+dt - GSBI7 AHB */
#define GSBI8_H_CLK                         132     /* drv+dt - GSBI8 AHB */
#define GSBI9_H_CLK                         133     /* drv - GSBI9 AHB */
#define GSBI10_H_CLK                        134     /* drv+dt - GSBI10 AHB */
#define GSBI11_H_CLK                        135     /* drv - GSBI11 AHB */
#define GSBI12_H_CLK                        136     /* drv+dt - GSBI12 AHB */
#define GSBI1_UART_SRC                      137     /* drv legacy:TP,Pre3 - GSBI1 UART src */
#define GSBI1_UART_CLK                      138     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI1 UART */
#define GSBI2_UART_SRC                      139     /* drv legacy:TP,Pre3 - GSBI2 UART src */
#define GSBI2_UART_CLK                      140     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI2 UART */
#define GSBI3_UART_SRC                      141     /* drv legacy:TP,Pre3 - GSBI3 UART src */
#define GSBI3_UART_CLK                      142     /* drv legacy:HTC,Sam,TP,Pre3 - GSBI3 UART */
#define GSBI4_UART_SRC                      143     /* drv legacy:TP,Pre3 - GSBI4 UART src */
#define GSBI4_UART_CLK                      144     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI4 UART */
#define GSBI5_UART_SRC                      145     /* drv legacy:TP,Pre3 - GSBI5 UART src */
#define GSBI5_UART_CLK                      146     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI5 UART */
#define GSBI6_UART_SRC                      147     /* drv legacy:TP,Pre3 - GSBI6 UART src */
#define GSBI6_UART_CLK                      148     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI6 UART */
#define GSBI7_UART_SRC                      149     /* drv legacy:TP,Pre3 - GSBI7 UART src */
#define GSBI7_UART_CLK                      150     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI7 UART */
#define GSBI8_UART_SRC                      151     /* drv legacy:TP,Pre3 - GSBI8 UART src */
#define GSBI8_UART_CLK                      152     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI8 UART */
#define GSBI9_UART_SRC                      153     /* drv legacy:TP,Pre3 - GSBI9 UART src */
#define GSBI9_UART_CLK                      154     /* drv legacy:HTC,Sam,TP,Pre3 - GSBI9 UART */
#define GSBI10_UART_SRC                     155     /* drv legacy:TP,Pre3 - GSBI10 UART src */
#define GSBI10_UART_CLK                     156     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI10 UART */
#define GSBI11_UART_SRC                     157     /* drv legacy:TP,Pre3 - GSBI11 UART src */
#define GSBI11_UART_CLK                     158     /* drv legacy:HTC,Sam,TP,Pre3 - GSBI11 UART */
#define GSBI12_UART_SRC                     159     /* drv legacy:TP,Pre3 - GSBI12 UART src */
#define GSBI12_UART_CLK                     160     /* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI12 UART */
#define GSBI1_QUP_SRC                       161     /* drv legacy:TP,Pre3 - GSBI1 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI1 QUP (SPI/I2C) */
#define GSBI1_QUP_CLK				162
#define GSBI2_QUP_SRC                       163     /* drv legacy:TP,Pre3 - GSBI2 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI2 QUP (SPI/I2C) */
#define GSBI2_QUP_CLK				164
#define GSBI3_QUP_SRC                       165     /* drv legacy:TP,Pre3 - GSBI3 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI3 QUP (SPI/I2C) */
#define GSBI3_QUP_CLK				166
#define GSBI4_QUP_SRC                       167     /* drv legacy:TP,Pre3 - GSBI4 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI4 QUP (SPI/I2C) */
#define GSBI4_QUP_CLK				168
#define GSBI5_QUP_SRC                       169     /* drv legacy:TP,Pre3 - GSBI5 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI5 QUP (SPI/I2C) */
#define GSBI5_QUP_CLK				170
#define GSBI6_QUP_SRC                       171     /* drv legacy:TP,Pre3 - GSBI6 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI6 QUP (SPI/I2C) */
#define GSBI6_QUP_CLK				172
#define GSBI7_QUP_SRC                       173     /* drv legacy:TP,Pre3 - GSBI7 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI7 QUP (SPI/I2C) */
#define GSBI7_QUP_CLK				174
#define GSBI8_QUP_SRC                       175     /* drv legacy:TP,Pre3 - GSBI8 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI8 QUP (SPI/I2C) */
#define GSBI8_QUP_CLK				176
#define GSBI9_QUP_SRC                       177     /* drv legacy:TP,Pre3 - GSBI9 QUP src */
/* drv legacy:HTC,Sam,TP,Pre3 - GSBI9 QUP (SPI/I2C) */
#define GSBI9_QUP_CLK				178
#define GSBI10_QUP_SRC                      179     /* drv legacy:TP,Pre3 - GSBI10 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI10 QUP (SPI/I2C) */
#define GSBI10_QUP_CLK				180
#define GSBI11_QUP_SRC                      181     /* drv legacy:TP,Pre3 - GSBI11 QUP src */
/* drv legacy:HTC,Sam,TP,Pre3 - GSBI11 QUP (SPI/I2C) */
#define GSBI11_QUP_CLK				182
#define GSBI12_QUP_SRC                      183     /* drv legacy:TP,Pre3 - GSBI12 QUP src */
/* drv+dt legacy:HTC,Sam,TP,Pre3 - GSBI12 QUP (SPI/I2C) */
#define GSBI12_QUP_CLK				184
#define GSBI1_SIM_CLK                       185     /* legacy:Pre3 - GSBI1 SIM */
#define GSBI2_SIM_CLK                       186     /* legacy:Pre3 - GSBI2 SIM */
#define GSBI3_SIM_CLK                       187     /* legacy:Pre3 - GSBI3 SIM */
#define GSBI4_SIM_CLK                       188     /* legacy:Pre3 - GSBI4 SIM */
#define GSBI5_SIM_CLK                       189     /* legacy:Pre3 - GSBI5 SIM */
#define GSBI6_SIM_CLK                       190     /* legacy:Pre3 - GSBI6 SIM */
#define GSBI7_SIM_CLK                       191     /* legacy:Pre3 - GSBI7 SIM */
#define GSBI8_SIM_CLK                       192     /* legacy:Pre3 - GSBI8 SIM */
#define GSBI9_SIM_CLK                       193     /* legacy:Pre3 - GSBI9 SIM */
#define GSBI10_SIM_CLK                      194     /* legacy:Pre3 - GSBI10 SIM */
#define GSBI11_SIM_CLK                      195     /* legacy:Pre3 - GSBI11 SIM */
#define GSBI12_SIM_CLK                      196     /* legacy:Pre3 - GSBI12 SIM */
#define SPDM_CFG_H_CLK                      197     /* header-only - SPDM cfg AHB */
#define SPDM_MSTR_H_CLK                     198     /* header-only - SPDM master AHB */
#define SPDM_FF_CLK_SRC                     199     /* header-only - SPDM FF src */
#define SPDM_FF_CLK                         200     /* header-only - SPDM FF */
#define SEC_CTRL_CLK                        201     /* header-only - security ctrl */
#define SEC_CTRL_ACC_CLK_SRC                202     /* header-only - security ctrl accel src */
#define SEC_CTRL_ACC_CLK                    203     /* header-only - security ctrl accel */
#define TLMM_H_CLK                          204     /* header-only - TLMM (pinctrl) AHB */
#define TLMM_CLK                            205     /* header-only - TLMM (pinctrl) */
#define MARM_CLK_SRC                        206     /* header-only - modem ARM CPU src */
#define MARM_CLK                            207     /* header-only - modem ARM CPU */
#define MAHB1_SRC                           208     /* header-only - modem AHB1 src */
#define MAHB1_CLK                           209     /* header-only - modem AHB1 */
#define SFAB_MSS_S_H_CLK                    210     /* header-only - SFAB->modem subsys slave AHB */
#define MAHB2_SRC                           211     /* header-only - modem AHB2 src */
#define MAHB2_CLK                           212     /* header-only - modem AHB2 */
#define MSS_MODEM_CLK_SRC                   213     /* header-only - modem clk src */
#define MSS_MODEM_CXO_CLK                   214     /* header-only - modem CXO */
#define MSS_SLP_CLK                         215     /* header-only - modem sleep */
#define MSS_SYS_REF_CLK                     216     /* header-only - modem sys ref */
#define TSSC_CLK_SRC                        217     /* drv - TSSC src */
/* drv legacy:HTC,Sam,TP,Pre3 - TSSC (touchscreen ctrl) */
#define TSSC_CLK				218
#define PDM_SRC                             219     /* drv legacy:HTC,Sam,TP,Pre3 - PDM src */
/* drv legacy:HTC,Sam,TP,Pre3 - PDM (pulse density modulator) */
#define PDM_CLK					220
#define GP0_SRC                             221     /* drv - GP0 (general purpose) src */
#define GP0_CLK                             222     /* drv legacy:HTC,Sam - GP0 */
#define GP1_SRC                             223     /* drv - GP1 src */
#define GP1_CLK                             224     /* drv legacy:HTC,Sam - GP1 */
#define GP2_SRC                             225     /* drv - GP2 src */
#define GP2_CLK                             226     /* drv legacy:HTC,Sam - GP2 */
/* drv+dt legacy:HTC,Sam,TP - PMEM (protected mem) */
#define PMEM_CLK				227
#define MPM_CLK                             228     /* header-only - MPM (mode/power mgr) */
#define EBI1_ASFAB_SRC                      229     /* header-only - EBI1 AFAB src */
#define EBI1_CLK_SRC                        230     /* header-only - EBI1 (DDR) src */
#define EBI1_CH0_CLK                        231     /* header-only - EBI1 ch0 */
#define EBI1_CH1_CLK                        232     /* header-only - EBI1 ch1 */
#define SFAB_SMPSS_S_H_CLK                  233     /* header-only - SFAB->SMPSS slave AHB */
#define PRNG_SRC                            234     /* drv legacy:TP,Pre3 - PRNG src */
#define PRNG_CLK                            235     /* drv+dt legacy:HTC,Sam,TP,Pre3 - PRNG (RNG) */
#define PXO_SRC                             236     /* legacy:HTC,Sam,TP,Pre3 - PXO src */
#define LPASS_CXO_CLK                       237     /* header-only - LPASS CXO */
#define LPASS_PXO_CLK                       238     /* header-only - LPASS PXO */
#define SPDM_CY_PORT0_CLK                   239     /* header-only - SPDM cycle port0 */
#define SPDM_CY_PORT1_CLK                   240     /* header-only - SPDM cycle port1 */
#define SPDM_CY_PORT2_CLK                   241     /* header-only - SPDM cycle port2 */
#define SPDM_CY_PORT3_CLK                   242     /* header-only - SPDM cycle port3 */
#define SPDM_CY_PORT4_CLK                   243     /* header-only - SPDM cycle port4 */
#define SPDM_CY_PORT5_CLK                   244     /* header-only - SPDM cycle port5 */
#define SPDM_CY_PORT6_CLK                   245     /* header-only - SPDM cycle port6 */
#define SPDM_CY_PORT7_CLK                   246     /* header-only - SPDM cycle port7 */
#define PLL0                                247     /* legacy:Pre3 - PLL0 */
#define PLL0_VOTE                           248     /* header-only - PLL0 voter */
#define PLL5                                249     /* header-only - PLL5 */
#define PLL6                                250     /* header-only - PLL6 (mm/lpass shared) */
#define PLL6_VOTE                           251     /* header-only - PLL6 voter */
#define PLL8                                252     /* drv legacy:HTC,Sam,Pre3 - PLL8 (main 384MHz) */
#define PLL8_VOTE                           253     /* header-only - PLL8 voter */
#define PLL9                                254     /* header-only - PLL9 (Scorpion CPU0) */
#define PLL10                               255     /* header-only - PLL10 (Scorpion CPU1) */
#define PLL11                               256     /* header-only - PLL11 (Scorpion L2) */
#define PLL12                               257     /* header-only - PLL12 (DDR) */

/* New clock IDs added in this cleanup. Append-only — never insert in the
 * middle of the existing numbering, since that would shift IDs of
 * earlier entries and break DT ABI for boards already using them. */
#define CE2_P_CLK                           258     /* drv+dt legacy:HTC,Sam,TP - CE2 APB (newly assigned, was missing) */
#define PLL4_VOTE                           259     /* drv legacy - PLL4 software vote */

#endif
