/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DDK750_REG_H__
#define DDK750_REG_H__

/* New register for SM750LE */
#define DE_STATE1                                        0x100054
#define DE_STATE1_DE_ABORT                               BIT(0)

#define DE_STATE2                                        0x100058
#define DE_STATE2_DE_FIFO_EMPTY                          BIT(3)
#define DE_STATE2_DE_STATUS_BUSY                         BIT(2)
#define DE_STATE2_DE_MEM_FIFO_EMPTY                      BIT(1)

#define SYSTEM_CTRL                                   0x000000
#define SYSTEM_CTRL_DPMS_MASK                         (0x3 << 30)
#define SYSTEM_CTRL_DPMS_VPHP                         (0x0 << 30)
#define SYSTEM_CTRL_DPMS_VPHN                         (0x1 << 30)
#define SYSTEM_CTRL_DPMS_VNHP                         (0x2 << 30)
#define SYSTEM_CTRL_DPMS_VNHN                         (0x3 << 30)
#define SYSTEM_CTRL_PCI_BURST                         BIT(29)
#define SYSTEM_CTRL_DE_FIFO_EMPTY                     BIT(23)
#define SYSTEM_CTRL_DE_STATUS_BUSY                    BIT(22)
#define SYSTEM_CTRL_DE_MEM_FIFO_EMPTY                 BIT(21)
#define SYSTEM_CTRL_PANEL_VSYNC_ACTIVE                BIT(18)
#define SYSTEM_CTRL_DE_ABORT                          BIT(13)

#define MISC_CTRL                                     0x000004
#define MISC_CTRL_DAC_POWER_OFF                       BIT(20)
#define MISC_CTRL_LOCALMEM_SIZE_MASK                  (0x3 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_8M                    (0x3 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_16M                   (0x0 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_32M                   (0x1 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_64M                   (0x2 << 12)
#define MISC_CTRL_LOCALMEM_RESET                      BIT(6)

#define GPIO_MUX                                      0x000008

#define CURRENT_GATE                                  0x000040
#define CURRENT_GATE_MCLK_MASK                        (0x3 << 14)
#ifdef VALIDATION_CHIP
    #define CURRENT_GATE_MCLK_112MHZ                  (0x0 << 14)
    #define CURRENT_GATE_MCLK_84MHZ                   (0x1 << 14)
    #define CURRENT_GATE_MCLK_56MHZ                   (0x2 << 14)
    #define CURRENT_GATE_MCLK_42MHZ                   (0x3 << 14)
#else
    #define CURRENT_GATE_MCLK_DIV_3                   (0x0 << 14)
    #define CURRENT_GATE_MCLK_DIV_4                   (0x1 << 14)
    #define CURRENT_GATE_MCLK_DIV_6                   (0x2 << 14)
    #define CURRENT_GATE_MCLK_DIV_8                   (0x3 << 14)
#endif
#define CURRENT_GATE_M2XCLK_MASK                      (0x3 << 12)
#ifdef VALIDATION_CHIP
    #define CURRENT_GATE_M2XCLK_336MHZ                (0x0 << 12)
    #define CURRENT_GATE_M2XCLK_168MHZ                (0x1 << 12)
    #define CURRENT_GATE_M2XCLK_112MHZ                (0x2 << 12)
    #define CURRENT_GATE_M2XCLK_84MHZ                 (0x3 << 12)
#else
    #define CURRENT_GATE_M2XCLK_DIV_1                 (0x0 << 12)
    #define CURRENT_GATE_M2XCLK_DIV_2                 (0x1 << 12)
    #define CURRENT_GATE_M2XCLK_DIV_3                 (0x2 << 12)
    #define CURRENT_GATE_M2XCLK_DIV_4                 (0x3 << 12)
#endif
#define CURRENT_GATE_GPIO                             BIT(6)
#define CURRENT_GATE_CSC                              BIT(4)
#define CURRENT_GATE_DE                               BIT(3)
#define CURRENT_GATE_DISPLAY                          BIT(2)
#define CURRENT_GATE_LOCALMEM                         BIT(1)

#define MODE0_GATE                                    0x000044
#define MODE0_GATE_GPIO                               BIT(6)

#define MODE1_GATE                                    0x000048

#define POWER_MODE_CTRL                               0x00004C
#ifdef VALIDATION_CHIP
    #define POWER_MODE_CTRL_336CLK                    BIT(4)
#endif
#define POWER_MODE_CTRL_OSC_INPUT                     BIT(3)
#define POWER_MODE_CTRL_MODE_MASK                     (0x3 << 0)
#define POWER_MODE_CTRL_MODE_MODE0                    (0x0 << 0)
#define POWER_MODE_CTRL_MODE_MODE1                    (0x1 << 0)
#define POWER_MODE_CTRL_MODE_SLEEP                    (0x2 << 0)

#define PANEL_PLL_CTRL                                0x00005C
#define PLL_CTRL_POWER                                BIT(17)
#ifdef VALIDATION_CHIP
    #define PLL_CTRL_OD_SHIFT                         14
    #define PLL_CTRL_OD_MASK                          (0x3 << 14)
#else
    #define PLL_CTRL_POD_SHIFT                        14
    #define PLL_CTRL_POD_MASK                         (0x3 << 14)
    #define PLL_CTRL_OD_SHIFT                         12
    #define PLL_CTRL_OD_MASK                          (0x3 << 12)
#endif
#define PLL_CTRL_N_SHIFT                              8
#define PLL_CTRL_N_MASK                               (0xf << 8)
#define PLL_CTRL_M_SHIFT                              0
#define PLL_CTRL_M_MASK                               0xff

#define CRT_PLL_CTRL                                  0x000060

#ifndef VALIDATION_CHIP

#define MXCLK_PLL_CTRL                                0x000070

#define VGA_CONFIGURATION                             0x000088
#define VGA_CONFIGURATION_PLL                         BIT(2)
#define VGA_CONFIGURATION_MODE                        BIT(1)

#endif

#define GPIO_DATA                                       0x010000

#define GPIO_DATA_DIRECTION                             0x010004

#define PANEL_DISPLAY_CTRL                            0x080000
#define PANEL_DISPLAY_CTRL_RESERVED_MASK              0xc0f08000
#define PANEL_DISPLAY_CTRL_SELECT_SHIFT               28
#define PANEL_DISPLAY_CTRL_SELECT_MASK                (0x3 << 28)
#define PANEL_DISPLAY_CTRL_FPEN                       BIT(27)
#define PANEL_DISPLAY_CTRL_VBIASEN                    BIT(26)
#define PANEL_DISPLAY_CTRL_DATA                       BIT(25)
#define PANEL_DISPLAY_CTRL_DUAL_DISPLAY               BIT(19)
#define PANEL_DISPLAY_CTRL_DOUBLE_PIXEL               BIT(18)
#define DISPLAY_CTRL_CLOCK_PHASE                      BIT(14)
#define DISPLAY_CTRL_VSYNC_PHASE                      BIT(13)
#define DISPLAY_CTRL_HSYNC_PHASE                      BIT(12)
#define PANEL_DISPLAY_CTRL_VSYNC                      BIT(11)
#define DISPLAY_CTRL_TIMING                           BIT(8)
#define DISPLAY_CTRL_PLANE                            BIT(2)

#define PANEL_FB_ADDRESS                              0x08000C
#define PANEL_FB_ADDRESS_ADDRESS_MASK                 0x1ffffff

#define PANEL_FB_WIDTH                                0x080010
#define PANEL_FB_WIDTH_WIDTH_SHIFT                    16
#define PANEL_FB_WIDTH_WIDTH_MASK                     (0x3fff << 16)
#define PANEL_FB_WIDTH_OFFSET_MASK                    0x3fff

#define PANEL_WINDOW_WIDTH                            0x080014
#define PANEL_WINDOW_WIDTH_WIDTH_SHIFT                16
#define PANEL_WINDOW_WIDTH_WIDTH_MASK                 (0xfff << 16)
#define PANEL_WINDOW_WIDTH_X_MASK                     0xfff

#define PANEL_WINDOW_HEIGHT                           0x080018
#define PANEL_WINDOW_HEIGHT_HEIGHT_SHIFT              16
#define PANEL_WINDOW_HEIGHT_HEIGHT_MASK               (0xfff << 16)
#define PANEL_WINDOW_HEIGHT_Y_MASK                    0xfff

#define PANEL_PLANE_TL                                0x08001C

#define PANEL_PLANE_BR                                0x080020
#define PANEL_PLANE_BR_BOTTOM_SHIFT                   16
#define PANEL_PLANE_BR_BOTTOM_MASK                    (0x7ff << 16)
#define PANEL_PLANE_BR_RIGHT_MASK                     0x7ff

#define PANEL_HORIZONTAL_TOTAL                        0x080024
#define PANEL_HORIZONTAL_TOTAL_TOTAL_SHIFT            16
#define PANEL_HORIZONTAL_TOTAL_TOTAL_MASK             (0xfff << 16)
#define PANEL_HORIZONTAL_TOTAL_DISPLAY_END_MASK       0xfff

#define PANEL_HORIZONTAL_SYNC                         0x080028
#define PANEL_HORIZONTAL_SYNC_WIDTH_SHIFT             16
#define PANEL_HORIZONTAL_SYNC_WIDTH_MASK              (0xff << 16)
#define PANEL_HORIZONTAL_SYNC_START_MASK              0xfff

#define PANEL_VERTICAL_TOTAL                          0x08002C
#define PANEL_VERTICAL_TOTAL_TOTAL_SHIFT              16
#define PANEL_VERTICAL_TOTAL_TOTAL_MASK               (0x7ff << 16)
#define PANEL_VERTICAL_TOTAL_DISPLAY_END_MASK         0x7ff

#define PANEL_VERTICAL_SYNC                           0x080030
#define PANEL_VERTICAL_SYNC_HEIGHT_SHIFT              16
#define PANEL_VERTICAL_SYNC_HEIGHT_MASK               (0x3f << 16)
#define PANEL_VERTICAL_SYNC_START_MASK                0x7ff







/* CRT Graphics Control */

#define CRT_DISPLAY_CTRL                              0x080200
#define CRT_DISPLAY_CTRL_RESERVED_MASK                0xfb008200

/* SM750LE definition */
#define CRT_DISPLAY_CTRL_DPMS_SHIFT                   30
#define CRT_DISPLAY_CTRL_DPMS_MASK                    (0x3 << 30)
#define CRT_DISPLAY_CTRL_DPMS_0                       (0x0 << 30)
#define CRT_DISPLAY_CTRL_DPMS_1                       (0x1 << 30)
#define CRT_DISPLAY_CTRL_DPMS_2                       (0x2 << 30)
#define CRT_DISPLAY_CTRL_DPMS_3                       (0x3 << 30)
#define CRT_DISPLAY_CTRL_CLK_MASK                     (0x7 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL25                    (0x0 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL41                    (0x1 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL65                    (0x3 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL74                    (0x4 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL80                    (0x5 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL108                   (0x6 << 27)

/* SM750LE definition */
#define CRT_DISPLAY_CTRL_CRTSELECT                    BIT(25)
#define CRT_DISPLAY_CTRL_RGBBIT                       BIT(24)

#ifndef VALIDATION_CHIP
    #define CRT_DISPLAY_CTRL_CENTERING                BIT(24)
#endif
#define CRT_DISPLAY_CTRL_SELECT_SHIFT                 18
#define CRT_DISPLAY_CTRL_SELECT_MASK                  (0x3 << 18)
#define CRT_DISPLAY_CTRL_BLANK                        BIT(10)
#define CRT_DISPLAY_CTRL_FORMAT_MASK                  (0x3 << 0)

#define CRT_FB_ADDRESS                                0x080204
#define CRT_FB_ADDRESS_ADDRESS_MASK                   0x3ffffff

#define CRT_FB_WIDTH                                  0x080208
#define CRT_FB_WIDTH_WIDTH_SHIFT                      16
#define CRT_FB_WIDTH_WIDTH_MASK                       (0x3fff << 16)
#define CRT_FB_WIDTH_OFFSET_MASK                      0x3fff

#define CRT_HORIZONTAL_TOTAL                          0x08020C
#define CRT_HORIZONTAL_TOTAL_TOTAL_SHIFT              16
#define CRT_HORIZONTAL_TOTAL_TOTAL_MASK               (0xfff << 16)
#define CRT_HORIZONTAL_TOTAL_DISPLAY_END_MASK         0xfff

#define CRT_HORIZONTAL_SYNC                           0x080210
#define CRT_HORIZONTAL_SYNC_WIDTH_SHIFT               16
#define CRT_HORIZONTAL_SYNC_WIDTH_MASK                (0xff << 16)
#define CRT_HORIZONTAL_SYNC_START_MASK                0xfff

#define CRT_VERTICAL_TOTAL                            0x080214
#define CRT_VERTICAL_TOTAL_TOTAL_SHIFT                16
#define CRT_VERTICAL_TOTAL_TOTAL_MASK                 (0x7ff << 16)
#define CRT_VERTICAL_TOTAL_DISPLAY_END_MASK           (0x7ff)

#define CRT_VERTICAL_SYNC                             0x080218
#define CRT_VERTICAL_SYNC_HEIGHT_SHIFT                16
#define CRT_VERTICAL_SYNC_HEIGHT_MASK                 (0x3f << 16)
#define CRT_VERTICAL_SYNC_START_MASK                  0x7ff


/* This vertical expansion below start at 0x080240 ~ 0x080264 */
#ifndef VALIDATION_CHIP
    #define CRT_VERTICAL_CENTERING_VALUE_MASK         (0xff << 24)
#endif

/* This horizontal expansion below start at 0x080268 ~ 0x08027C */
#ifndef VALIDATION_CHIP
    #define CRT_HORIZONTAL_CENTERING_VALUE_MASK       (0xff << 24)
#endif

#ifndef VALIDATION_CHIP
    /* Auto Centering */
    #define CRT_AUTO_CENTERING_TL                     0x080280
    #define CRT_AUTO_CENTERING_TL_TOP_MASK            (0x7ff << 16)
    #define CRT_AUTO_CENTERING_TL_LEFT_MASK           0x7ff

    #define CRT_AUTO_CENTERING_BR                     0x080284
    #define CRT_AUTO_CENTERING_BR_BOTTOM_MASK         (0x7ff << 16)
    #define CRT_AUTO_CENTERING_BR_BOTTOM_SHIFT        16
    #define CRT_AUTO_CENTERING_BR_RIGHT_MASK          0x7ff
#endif

/* sm750le new register to control panel output */
#define DISPLAY_CONTROL_750LE			      0x80288

/* Panel Palette register starts at 0x080400 ~ 0x0807FC */
#define PANEL_PALETTE_RAM                             0x080400

/* Panel Palette register starts at 0x080C00 ~ 0x080FFC */
#define CRT_PALETTE_RAM                               0x080C00

/* Color Space Conversion registers. */

#ifndef VALIDATION_CHIP
    #define ZV0_CAPTURE_BUF_OFFSET_YCLIP_ODD_FIELD      (0x3ff << 16)
#endif


/* Default i2c CLK and Data GPIO. These are the default i2c pins */
#define DEFAULT_I2C_SCL                     30
#define DEFAULT_I2C_SDA                     31

#define GPIO_DATA_SM750LE                               0x020018

#define GPIO_DATA_DIRECTION_SM750LE                     0x02001C

#endif
