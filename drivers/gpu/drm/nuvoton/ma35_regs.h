/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#ifndef _MA35_REGS_H_
#define _MA35_REGS_H_

#define MA35_FRAMEBUFFER_CONFIG                   0x1518
#define MA35_FRAMEBUFFER_ADDRESS                  0x1400
#define MA35_FRAMEBUFFER_STRIDE                   0x1408
#define MA35_HDISPLAY                             0x1430
#define MA35_HSYNC                                0x1438
#define MA35_VDISPLAY                             0x1440
#define MA35_VSYNC                                0x1448
#define MA35_PANEL_CONFIG                         0x1418
#define MA35_DPI_CONFIG                           0x14B8
#define MA35_CURSOR_ADDRESS                       0x146C
#define MA35_CURSOR_CONFIG                        0x1468
#define MA35_CURSOR_LOCATION                      0x1470
#define MA35_CURSOR_BACKGROUND                    0x1474
#define MA35_CURSOR_FOREGROUND                    0x1478
#define MA35_FRAMEBUFFER_UPLANAR_ADDRESS          0x1530
#define MA35_FRAMEBUFFER_VPLANAR_ADDRESS          0x1538
#define MA35_FRAMEBUFFER_USTRIDE                  0x1800
#define MA35_FRAMEBUFFER_VSTRIDE                  0x1808
#define MA35_INDEXCOLOR_TABLEINDEX                0x1818
#define MA35_INDEXCOLOR_TABLEDATA                 0x1820
#define MA35_FRAMEBUFFER_SIZE                     0x1810
#define MA35_FRAMEBUFFER_SCALEFACTORX             0x1828
#define MA35_FRAMEBUFFER_SCALEFACTORY             0x1830
#define MA35_FRAMEBUFFER_SCALEFCONFIG             0x1520
#define MA35_HORIFILTER_KERNELINDEX               0x1838
#define MA35_HORIFILTER_KERNEL                    0x1A00
#define MA35_VERTIFILTER_KERNELINDEX              0x1A08
#define MA35_VERTIFILTER_KERNEL                   0x1A10
#define MA35_FRAMEBUFFER_INITIALOFFSET            0x1A20
#define MA35_FRAMEBUFFER_COLORKEY                 0x1508
#define MA35_FRAMEBUFFER_COLORHIGHKEY             0x1510
#define MA35_FRAMEBUFFER_BGCOLOR                  0x1528
#define MA35_FRAMEBUFFER_CLEARVALUE               0x1A18
#define MA35_DISPLAY_INTRENABLE                   0x1480
#define MA35_INT_STATE                            0x147C
#define MA35_PANEL_DEST_ADDRESS                   0x14F0
#define MA35_MEM_DEST_ADDRESS                     0x14E8
#define MA35_DEST_CONFIG                          0x14F8
#define MA35_DEST_STRIDE                          0x1500
#define MA35_DBI_CONFIG                           0x1488
#define MA35_AQHICLOCKCONTROL                     0x0000
#define MA35_OVERLAY_CONFIG                       0x1540
#define MA35_OVERLAY_STRIDE                       0x1600
#define MA35_OVERLAY_USTRIDE                      0x18C0
#define MA35_OVERLAY_VSTRIDE                      0x1900
#define MA35_OVERLAY_TL                           0x1640
#define MA35_OVERLAY_BR                           0x1680
#define MA35_OVERLAY_ALPHA_BLEND_CONFIG           0x1580
#define MA35_OVERLAY_SRC_GLOBAL_COLOR             0x16C0
#define MA35_OVERLAY_DST_GLOBAL_COLOR             0x1700
#define MA35_OVERLAY_CLEAR_VALUE                  0x1940
#define MA35_OVERLAY_SIZE                         0x17C0
#define MA35_OVERLAY_COLOR_KEY                    0x1740
#define MA35_OVERLAY_COLOR_KEY_HIGH               0x1780
#define MA35_OVERLAY_ADDRESS                      0x15C0
#define MA35_OVERLAY_UPLANAR_ADDRESS              0x1840
#define MA35_OVERLAY_VPLANAR_ADDRESS              0x1880
#define MA35_OVERLAY_SCALE_CONFIG                 0x1C00
#define MA35_OVERLAY_SCALE_FACTOR_X               0x1A40
#define MA35_OVERLAY_SCALE_FACTOR_Y               0x1A80
#define MA35_OVERLAY_HORI_FILTER_KERNEL_INDEX     0x1AC0
#define MA35_OVERLAY_HORI_FILTER_KERNEL           0x1B00
#define MA35_OVERLAY_VERTI_FILTER_KERNEL_INDEX    0x1B40
#define MA35_OVERLAY_VERTI_FILTER_KERNEL          0x1B80
#define MA35_OVERLAY_INITIAL_OFFSET               0x1BC0
#define MA35_GAMMA_EX_INDEX                       0x1CF0
#define MA35_GAMMA_EX_DATA                        0x1CF8
#define MA35_GAMMA_EX_ONE_DATA                    0x1D80
#define MA35_GAMMA_INDEX                          0x1458
#define MA35_GAMMA_DATA                           0x1460
#define MA35_DISPLAY_DITHER_TABLE_LOW             0x1420
#define MA35_DISPLAY_DITHER_TABLE_HIGH            0x1428
#define MA35_DISPLAY_DITHER_CONFIG                0x1410
#define MA35_DISPLAY_CURRENT_LOCATION             0x1450

#endif
