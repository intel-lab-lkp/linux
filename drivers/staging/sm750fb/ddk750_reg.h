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
#define SYSTEM_CTRL_PCI_MASTER                        BIT(25)
#define SYSTEM_CTRL_LATENCY_TIMER_OFF                 BIT(24)
#define SYSTEM_CTRL_DE_FIFO_EMPTY                     BIT(23)
#define SYSTEM_CTRL_DE_STATUS_BUSY                    BIT(22)
#define SYSTEM_CTRL_DE_MEM_FIFO_EMPTY                 BIT(21)
#define SYSTEM_CTRL_CSC_STATUS_BUSY                   BIT(20)
#define SYSTEM_CTRL_CRT_VSYNC_ACTIVE                  BIT(19)
#define SYSTEM_CTRL_PANEL_VSYNC_ACTIVE                BIT(18)
#define SYSTEM_CTRL_CURRENT_BUFFER_FLIP_PENDING       BIT(17)
#define SYSTEM_CTRL_DMA_STATUS_BUSY                   BIT(16)
#define SYSTEM_CTRL_PCI_BURST_READ                    BIT(15)
#define SYSTEM_CTRL_DE_ABORT                          BIT(13)
#define SYSTEM_CTRL_PCI_SUBSYS_ID_LOCK                BIT(11)
#define SYSTEM_CTRL_PCI_RETRY_OFF                     BIT(7)
#define SYSTEM_CTRL_PCI_SLAVE_BURST_READ_SIZE_MASK    (0x3 << 4)
#define SYSTEM_CTRL_PCI_SLAVE_BURST_READ_SIZE_1       (0x0 << 4)
#define SYSTEM_CTRL_PCI_SLAVE_BURST_READ_SIZE_2       (0x1 << 4)
#define SYSTEM_CTRL_PCI_SLAVE_BURST_READ_SIZE_4       (0x2 << 4)
#define SYSTEM_CTRL_PCI_SLAVE_BURST_READ_SIZE_8       (0x3 << 4)
#define SYSTEM_CTRL_CRT_TRISTATE                      BIT(3)
#define SYSTEM_CTRL_PCIMEM_TRISTATE                   BIT(2)
#define SYSTEM_CTRL_LOCALMEM_TRISTATE                 BIT(1)
#define SYSTEM_CTRL_PANEL_TRISTATE                    BIT(0)

#define MISC_CTRL                                     0x000004
#define MISC_CTRL_DRAM_RERESH_COUNT                   BIT(27)
#define MISC_CTRL_DRAM_REFRESH_TIME_MASK              (0x3 << 25)
#define MISC_CTRL_DRAM_REFRESH_TIME_8                 (0x0 << 25)
#define MISC_CTRL_DRAM_REFRESH_TIME_16                (0x1 << 25)
#define MISC_CTRL_DRAM_REFRESH_TIME_32                (0x2 << 25)
#define MISC_CTRL_DRAM_REFRESH_TIME_64                (0x3 << 25)
#define MISC_CTRL_INT_OUTPUT_INVERT                   BIT(24)
#define MISC_CTRL_PLL_CLK_COUNT                       BIT(23)
#define MISC_CTRL_DAC_POWER_OFF                       BIT(20)
#define MISC_CTRL_CLK_SELECT_TESTCLK                  BIT(16)
#define MISC_CTRL_DRAM_COLUMN_SIZE_MASK               (0x3 << 14)
#define MISC_CTRL_DRAM_COLUMN_SIZE_256                (0x0 << 14)
#define MISC_CTRL_DRAM_COLUMN_SIZE_512                (0x1 << 14)
#define MISC_CTRL_DRAM_COLUMN_SIZE_1024               (0x2 << 14)
#define MISC_CTRL_LOCALMEM_SIZE_MASK                  (0x3 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_8M                    (0x3 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_16M                   (0x0 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_32M                   (0x1 << 12)
#define MISC_CTRL_LOCALMEM_SIZE_64M                   (0x2 << 12)
#define MISC_CTRL_DRAM_TWTR                           BIT(11)
#define MISC_CTRL_DRAM_TWR                            BIT(10)
#define MISC_CTRL_DRAM_TRP                            BIT(9)
#define MISC_CTRL_DRAM_TRFC                           BIT(8)
#define MISC_CTRL_DRAM_TRAS                           BIT(7)
#define MISC_CTRL_LOCALMEM_RESET                      BIT(6)
#define MISC_CTRL_LOCALMEM_STATE_INACTIVE             BIT(5)
#define MISC_CTRL_CPU_CAS_LATENCY                     BIT(4)
#define MISC_CTRL_DLL_OFF                             BIT(3)
#define MISC_CTRL_DRAM_OUTPUT_HIGH                    BIT(2)
#define MISC_CTRL_LOCALMEM_BUS_SIZE                   BIT(1)
#define MISC_CTRL_EMBEDDED_LOCALMEM_OFF               BIT(0)

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
#define CURRENT_GATE_I2C                              BIT(8)
#define CURRENT_GATE_GPIO                             BIT(6)
#define CURRENT_GATE_CSC                              BIT(4)
#define CURRENT_GATE_DE                               BIT(3)
#define CURRENT_GATE_DISPLAY                          BIT(2)
#define CURRENT_GATE_LOCALMEM                         BIT(1)
#define CURRENT_GATE_DMA                              BIT(0)

#define MODE0_GATE                                    0x000044
#define MODE0_GATE_MCLK_MASK                          (0x3 << 14)
#define MODE0_GATE_MCLK_112MHZ                        (0x0 << 14)
#define MODE0_GATE_MCLK_84MHZ                         (0x1 << 14)
#define MODE0_GATE_MCLK_56MHZ                         (0x2 << 14)
#define MODE0_GATE_MCLK_42MHZ                         (0x3 << 14)
#define MODE0_GATE_M2XCLK_MASK                        (0x3 << 12)
#define MODE0_GATE_M2XCLK_336MHZ                      (0x0 << 12)
#define MODE0_GATE_M2XCLK_168MHZ                      (0x1 << 12)
#define MODE0_GATE_M2XCLK_112MHZ                      (0x2 << 12)
#define MODE0_GATE_M2XCLK_84MHZ                       (0x3 << 12)
#define MODE0_GATE_VGA                                BIT(10)
#define MODE0_GATE_PWM                                BIT(9)
#define MODE0_GATE_I2C                                BIT(8)
#define MODE0_GATE_SSP                                BIT(7)
#define MODE0_GATE_GPIO                               BIT(6)
#define MODE0_GATE_ZVPORT                             BIT(5)
#define MODE0_GATE_CSC                                BIT(4)
#define MODE0_GATE_DE                                 BIT(3)
#define MODE0_GATE_DISPLAY                            BIT(2)
#define MODE0_GATE_LOCALMEM                           BIT(1)
#define MODE0_GATE_DMA                                BIT(0)

#define MODE1_GATE                                    0x000048
#define MODE1_GATE_MCLK_MASK                          (0x3 << 14)
#define MODE1_GATE_MCLK_112MHZ                        (0x0 << 14)
#define MODE1_GATE_MCLK_84MHZ                         (0x1 << 14)
#define MODE1_GATE_MCLK_56MHZ                         (0x2 << 14)
#define MODE1_GATE_MCLK_42MHZ                         (0x3 << 14)
#define MODE1_GATE_M2XCLK_MASK                        (0x3 << 12)
#define MODE1_GATE_M2XCLK_336MHZ                      (0x0 << 12)
#define MODE1_GATE_M2XCLK_168MHZ                      (0x1 << 12)
#define MODE1_GATE_M2XCLK_112MHZ                      (0x2 << 12)
#define MODE1_GATE_M2XCLK_84MHZ                       (0x3 << 12)
#define MODE1_GATE_VGA                                BIT(10)
#define MODE1_GATE_PWM                                BIT(9)
#define MODE1_GATE_I2C                                BIT(8)
#define MODE1_GATE_SSP                                BIT(7)
#define MODE1_GATE_GPIO                               BIT(6)
#define MODE1_GATE_ZVPORT                             BIT(5)
#define MODE1_GATE_CSC                                BIT(4)
#define MODE1_GATE_DE                                 BIT(3)
#define MODE1_GATE_DISPLAY                            BIT(2)
#define MODE1_GATE_LOCALMEM                           BIT(1)
#define MODE1_GATE_DMA                                BIT(0)

#define POWER_MODE_CTRL                               0x00004C
#ifdef VALIDATION_CHIP
    #define POWER_MODE_CTRL_336CLK                    BIT(4)
#endif
#define POWER_MODE_CTRL_OSC_INPUT                     BIT(3)
#define POWER_MODE_CTRL_ACPI                          BIT(2)
#define POWER_MODE_CTRL_MODE_MASK                     (0x3 << 0)
#define POWER_MODE_CTRL_MODE_MODE0                    (0x0 << 0)
#define POWER_MODE_CTRL_MODE_MODE1                    (0x1 << 0)
#define POWER_MODE_CTRL_MODE_SLEEP                    (0x2 << 0)

#define PCI_MASTER_BASE                               0x000050
#define PCI_MASTER_BASE_ADDRESS_MASK                  0xff

#define DEVICE_ID                                     0x000054
#define DEVICE_ID_DEVICE_ID_MASK                      (0xffff << 16)
#define DEVICE_ID_REVISION_ID_MASK                    0xff

#define PLL_CLK_COUNT                                 0x000058
#define PLL_CLK_COUNT_COUNTER_MASK                    0xffff

#define PANEL_PLL_CTRL                                0x00005C
#define PLL_CTRL_BYPASS                               BIT(18)
#define PLL_CTRL_POWER                                BIT(17)
#define PLL_CTRL_INPUT                                BIT(16)
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

#define VGA_PLL0_CTRL                                 0x000064

#define VGA_PLL1_CTRL                                 0x000068

#define SCRATCH_DATA                                  0x00006c

#ifndef VALIDATION_CHIP

#define MXCLK_PLL_CTRL                                0x000070

#define VGA_CONFIGURATION                             0x000088
#define VGA_CONFIGURATION_USER_DEFINE_MASK            (0x3 << 4)
#define VGA_CONFIGURATION_PLL                         BIT(2)
#define VGA_CONFIGURATION_MODE                        BIT(1)

#endif

#define GPIO_DATA                                       0x010000

#define GPIO_DATA_DIRECTION                             0x010004

#define PANEL_DISPLAY_CTRL                            0x080000
#define PANEL_DISPLAY_CTRL_RESERVED_MASK              0xc0f08000
#define PANEL_DISPLAY_CTRL_SELECT_SHIFT               28
#define PANEL_DISPLAY_CTRL_SELECT_MASK                (0x3 << 28)
#define PANEL_DISPLAY_CTRL_SELECT_PANEL               (0x0 << 28)
#define PANEL_DISPLAY_CTRL_SELECT_VGA                 (0x1 << 28)
#define PANEL_DISPLAY_CTRL_SELECT_CRT                 (0x2 << 28)
#define PANEL_DISPLAY_CTRL_FPEN                       BIT(27)
#define PANEL_DISPLAY_CTRL_VBIASEN                    BIT(26)
#define PANEL_DISPLAY_CTRL_DATA                       BIT(25)
#define PANEL_DISPLAY_CTRL_FPVDDEN                    BIT(24)
#define PANEL_DISPLAY_CTRL_DUAL_DISPLAY               BIT(19)
#define PANEL_DISPLAY_CTRL_DOUBLE_PIXEL               BIT(18)
#define PANEL_DISPLAY_CTRL_FIFO                       (0x3 << 16)
#define PANEL_DISPLAY_CTRL_FIFO_1                     (0x0 << 16)
#define PANEL_DISPLAY_CTRL_FIFO_3                     (0x1 << 16)
#define PANEL_DISPLAY_CTRL_FIFO_7                     (0x2 << 16)
#define PANEL_DISPLAY_CTRL_FIFO_11                    (0x3 << 16)
#define DISPLAY_CTRL_CLOCK_PHASE                      BIT(14)
#define DISPLAY_CTRL_VSYNC_PHASE                      BIT(13)
#define DISPLAY_CTRL_HSYNC_PHASE                      BIT(12)
#define PANEL_DISPLAY_CTRL_VSYNC                      BIT(11)
#define PANEL_DISPLAY_CTRL_CAPTURE_TIMING             BIT(10)
#define PANEL_DISPLAY_CTRL_COLOR_KEY                  BIT(9)
#define DISPLAY_CTRL_TIMING                           BIT(8)
#define PANEL_DISPLAY_CTRL_VERTICAL_PAN_DIR           BIT(7)
#define PANEL_DISPLAY_CTRL_VERTICAL_PAN               BIT(6)
#define PANEL_DISPLAY_CTRL_HORIZONTAL_PAN_DIR         BIT(5)
#define PANEL_DISPLAY_CTRL_HORIZONTAL_PAN             BIT(4)
#define DISPLAY_CTRL_GAMMA                            BIT(3)
#define DISPLAY_CTRL_PLANE                            BIT(2)
#define PANEL_DISPLAY_CTRL_FORMAT                     (0x3 << 0)
#define PANEL_DISPLAY_CTRL_FORMAT_8                   (0x0 << 0)
#define PANEL_DISPLAY_CTRL_FORMAT_16                  (0x1 << 0)
#define PANEL_DISPLAY_CTRL_FORMAT_32                  (0x2 << 0)

#define PANEL_PAN_CTRL                                0x080004
#define PANEL_PAN_CTRL_VERTICAL_PAN_MASK              (0xff << 24)
#define PANEL_PAN_CTRL_VERTICAL_VSYNC_MASK            (0x3f << 16)
#define PANEL_PAN_CTRL_HORIZONTAL_PAN_MASK            (0xff << 8)
#define PANEL_PAN_CTRL_HORIZONTAL_VSYNC_MASK          0x3f

#define PANEL_COLOR_KEY                               0x080008
#define PANEL_COLOR_KEY_MASK_MASK                     (0xffff << 16)
#define PANEL_COLOR_KEY_VALUE_MASK                    0xffff

#define PANEL_FB_ADDRESS                              0x08000C
#define PANEL_FB_ADDRESS_STATUS                       BIT(31)
#define PANEL_FB_ADDRESS_EXT                          BIT(27)
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
#define PANEL_PLANE_TL_TOP_SHIFT                      16
#define PANEL_PLANE_TL_TOP_MASK                       (0x7ff << 16)
#define PANEL_PLANE_TL_LEFT_MASK                      0x7ff

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

#define PANEL_CURRENT_LINE                            0x080034
#define PANEL_CURRENT_LINE_LINE_MASK                  0x7ff

/* Video Control */

#define VIDEO_DISPLAY_CTRL                              0x080040
#define VIDEO_DISPLAY_CTRL_LINE_BUFFER                  BIT(18)
#define VIDEO_DISPLAY_CTRL_FIFO_MASK                    (0x3 << 16)
#define VIDEO_DISPLAY_CTRL_FIFO_1                       (0x0 << 16)
#define VIDEO_DISPLAY_CTRL_FIFO_3                       (0x1 << 16)
#define VIDEO_DISPLAY_CTRL_FIFO_7                       (0x2 << 16)
#define VIDEO_DISPLAY_CTRL_FIFO_11                      (0x3 << 16)
#define VIDEO_DISPLAY_CTRL_BUFFER                       BIT(15)
#define VIDEO_DISPLAY_CTRL_CAPTURE                      BIT(14)
#define VIDEO_DISPLAY_CTRL_DOUBLE_BUFFER                BIT(13)
#define VIDEO_DISPLAY_CTRL_BYTE_SWAP                    BIT(12)
#define VIDEO_DISPLAY_CTRL_VERTICAL_SCALE               BIT(11)
#define VIDEO_DISPLAY_CTRL_HORIZONTAL_SCALE             BIT(10)
#define VIDEO_DISPLAY_CTRL_VERTICAL_MODE                BIT(9)
#define VIDEO_DISPLAY_CTRL_HORIZONTAL_MODE              BIT(8)
#define VIDEO_DISPLAY_CTRL_PIXEL_MASK                   (0xf << 4)
#define VIDEO_DISPLAY_CTRL_GAMMA                        BIT(3)
#define VIDEO_DISPLAY_CTRL_FORMAT_MASK                  0x3
#define VIDEO_DISPLAY_CTRL_FORMAT_8                     0x0
#define VIDEO_DISPLAY_CTRL_FORMAT_16                    0x1
#define VIDEO_DISPLAY_CTRL_FORMAT_32                    0x2
#define VIDEO_DISPLAY_CTRL_FORMAT_YUV                   0x3

#define VIDEO_FB_0_ADDRESS                            0x080044
#define VIDEO_FB_0_ADDRESS_STATUS                     BIT(31)
#define VIDEO_FB_0_ADDRESS_EXT                        BIT(27)
#define VIDEO_FB_0_ADDRESS_ADDRESS_MASK               0x3ffffff

#define VIDEO_FB_WIDTH                                0x080048
#define VIDEO_FB_WIDTH_WIDTH_MASK                     (0x3fff << 16)
#define VIDEO_FB_WIDTH_OFFSET_MASK                    0x3fff

#define VIDEO_FB_0_LAST_ADDRESS                       0x08004C
#define VIDEO_FB_0_LAST_ADDRESS_EXT                   BIT(27)
#define VIDEO_FB_0_LAST_ADDRESS_ADDRESS_MASK          0x3ffffff

#define VIDEO_PLANE_TL                                0x080050
#define VIDEO_PLANE_TL_TOP_MASK                       (0x7ff << 16)
#define VIDEO_PLANE_TL_LEFT_MASK                      0x7ff

#define VIDEO_PLANE_BR                                0x080054
#define VIDEO_PLANE_BR_BOTTOM_MASK                    (0x7ff << 16)
#define VIDEO_PLANE_BR_RIGHT_MASK                     0x7ff

#define VIDEO_SCALE                                   0x080058
#define VIDEO_SCALE_VERTICAL_MODE                     BIT(31)
#define VIDEO_SCALE_VERTICAL_SCALE_MASK               (0xfff << 16)
#define VIDEO_SCALE_HORIZONTAL_MODE                   BIT(15)
#define VIDEO_SCALE_HORIZONTAL_SCALE_MASK             0xfff

#define VIDEO_INITIAL_SCALE                           0x08005C
#define VIDEO_INITIAL_SCALE_FB_1_MASK                 (0xfff << 16)
#define VIDEO_INITIAL_SCALE_FB_0_MASK                 0xfff

#define VIDEO_YUV_CONSTANTS                           0x080060
#define VIDEO_YUV_CONSTANTS_Y_MASK                    (0xff << 24)
#define VIDEO_YUV_CONSTANTS_R_MASK                    (0xff << 16)
#define VIDEO_YUV_CONSTANTS_G_MASK                    (0xff << 8)
#define VIDEO_YUV_CONSTANTS_B_MASK                    0xff

#define VIDEO_FB_1_ADDRESS                            0x080064
#define VIDEO_FB_1_ADDRESS_STATUS                     BIT(31)
#define VIDEO_FB_1_ADDRESS_EXT                        BIT(27)
#define VIDEO_FB_1_ADDRESS_ADDRESS_MASK               0x3ffffff

#define VIDEO_FB_1_LAST_ADDRESS                       0x080068
#define VIDEO_FB_1_LAST_ADDRESS_EXT                   BIT(27)
#define VIDEO_FB_1_LAST_ADDRESS_ADDRESS_MASK          0x3ffffff

/* Video Alpha Control */

#define VIDEO_ALPHA_DISPLAY_CTRL                        0x080080

/* Alpha Control */

#define ALPHA_DISPLAY_CTRL                            0x080100

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
#define CRT_DISPLAY_CTRL_CLK_PLL62                    (0x2 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL65                    (0x3 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL74                    (0x4 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL80                    (0x5 << 27)
#define CRT_DISPLAY_CTRL_CLK_PLL108                   (0x6 << 27)
#define CRT_DISPLAY_CTRL_CLK_RESERVED                 (0x7 << 27)
#define CRT_DISPLAY_CTRL_SHIFT_VGA_DAC                BIT(26)

/* SM750LE definition */
#define CRT_DISPLAY_CTRL_CRTSELECT                    BIT(25)
#define CRT_DISPLAY_CTRL_RGBBIT                       BIT(24)

#ifndef VALIDATION_CHIP
    #define CRT_DISPLAY_CTRL_CENTERING                BIT(24)
#endif
#define CRT_DISPLAY_CTRL_LOCK_TIMING                  BIT(23)
#define CRT_DISPLAY_CTRL_EXPANSION                    BIT(22)
#define CRT_DISPLAY_CTRL_VERTICAL_MODE                BIT(21)
#define CRT_DISPLAY_CTRL_HORIZONTAL_MODE              BIT(20)
#define CRT_DISPLAY_CTRL_SELECT_SHIFT                 18
#define CRT_DISPLAY_CTRL_SELECT_MASK                  (0x3 << 18)
#define CRT_DISPLAY_CTRL_SELECT_PANEL                 (0x0 << 18)
#define CRT_DISPLAY_CTRL_SELECT_VGA                   (0x1 << 18)
#define CRT_DISPLAY_CTRL_SELECT_CRT                   (0x2 << 18)
#define CRT_DISPLAY_CTRL_FIFO_MASK                    (0x3 << 16)
#define CRT_DISPLAY_CTRL_FIFO_1                       (0x0 << 16)
#define CRT_DISPLAY_CTRL_FIFO_3                       (0x1 << 16)
#define CRT_DISPLAY_CTRL_FIFO_7                       (0x2 << 16)
#define CRT_DISPLAY_CTRL_FIFO_11                      (0x3 << 16)
#define CRT_DISPLAY_CTRL_BLANK                        BIT(10)
#define CRT_DISPLAY_CTRL_PIXEL_MASK                   (0xf << 4)
#define CRT_DISPLAY_CTRL_FORMAT_MASK                  (0x3 << 0)
#define CRT_DISPLAY_CTRL_FORMAT_8                     (0x0 << 0)
#define CRT_DISPLAY_CTRL_FORMAT_16                    (0x1 << 0)
#define CRT_DISPLAY_CTRL_FORMAT_32                    (0x2 << 0)

#define CRT_FB_ADDRESS                                0x080204
#define CRT_FB_ADDRESS_STATUS                         BIT(31)
#define CRT_FB_ADDRESS_EXT                            BIT(27)
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

#define CRT_SIGNATURE_ANALYZER                        0x08021C
#define CRT_SIGNATURE_ANALYZER_STATUS_MASK            (0xffff << 16)
#define CRT_SIGNATURE_ANALYZER_ENABLE                 BIT(3)
#define CRT_SIGNATURE_ANALYZER_RESET                  BIT(2)
#define CRT_SIGNATURE_ANALYZER_SOURCE_MASK            0x3
#define CRT_SIGNATURE_ANALYZER_SOURCE_RED             0
#define CRT_SIGNATURE_ANALYZER_SOURCE_GREEN           1
#define CRT_SIGNATURE_ANALYZER_SOURCE_BLUE            2

#define CRT_CURRENT_LINE                              0x080220
#define CRT_CURRENT_LINE_LINE_MASK                    0x7ff

#define CRT_MONITOR_DETECT                            0x080224
#define CRT_MONITOR_DETECT_VALUE                      BIT(25)
#define CRT_MONITOR_DETECT_ENABLE                     BIT(24)
#define CRT_MONITOR_DETECT_RED_MASK                   (0xff << 16)
#define CRT_MONITOR_DETECT_GREEN_MASK                 (0xff << 8)
#define CRT_MONITOR_DETECT_BLUE_MASK                  0xff

#define CRT_SCALE                                     0x080228
#define CRT_SCALE_VERTICAL_MODE                       BIT(31)
#define CRT_SCALE_VERTICAL_SCALE_MASK                 (0xfff << 16)
#define CRT_SCALE_HORIZONTAL_MODE                     BIT(15)
#define CRT_SCALE_HORIZONTAL_SCALE_MASK               0xfff

/* This vertical expansion below start at 0x080240 ~ 0x080264 */
#define CRT_VERTICAL_EXPANSION                        0x080240
#ifndef VALIDATION_CHIP
    #define CRT_VERTICAL_CENTERING_VALUE_MASK         (0xff << 24)
#endif
#define CRT_VERTICAL_EXPANSION_COMPARE_VALUE_MASK     (0xff << 16)
#define CRT_VERTICAL_EXPANSION_LINE_BUFFER_MASK       (0xf << 12)
#define CRT_VERTICAL_EXPANSION_SCALE_FACTOR_MASK      0xfff

/* This horizontal expansion below start at 0x080268 ~ 0x08027C */
#define CRT_HORIZONTAL_EXPANSION                      0x080268
#ifndef VALIDATION_CHIP
    #define CRT_HORIZONTAL_CENTERING_VALUE_MASK       (0xff << 24)
#endif
#define CRT_HORIZONTAL_EXPANSION_COMPARE_VALUE_MASK   (0xff << 16)
#define CRT_HORIZONTAL_EXPANSION_SCALE_FACTOR_MASK    0xfff

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
/* Palette RAM */

/* Panel Palette register starts at 0x080400 ~ 0x0807FC */
#define PANEL_PALETTE_RAM                             0x080400

/* Panel Palette register starts at 0x080C00 ~ 0x080FFC */
#define CRT_PALETTE_RAM                               0x080C00

#define DE_DATA_PORT                                    0x110000

#define I2C_BYTE_COUNT                                  0x010040
#define I2C_BYTE_COUNT_COUNT_MASK                       0xf

#define I2C_CTRL                                        0x010041
#define I2C_CTRL_INT                                    BIT(4)
#define I2C_CTRL_DIR                                    BIT(3)
#define I2C_CTRL_CTRL                                   BIT(2)
#define I2C_CTRL_MODE                                   BIT(1)
#define I2C_CTRL_EN                                     BIT(0)

#define I2C_STATUS                                      0x010042
#define I2C_STATUS_TX                                   BIT(3)
#define I2C_STATUS_ERR                                  BIT(2)
#define I2C_STATUS_ACK                                  BIT(1)
#define I2C_STATUS_BSY                                  BIT(0)

#define I2C_RESET                                       0x010042
#define I2C_RESET_BUS_ERROR                             BIT(2)

#define I2C_SLAVE_ADDRESS                               0x010043
#define I2C_SLAVE_ADDRESS_ADDRESS_MASK                  (0x7f << 1)
#define I2C_SLAVE_ADDRESS_RW                            BIT(0)

#define I2C_DATA0                                       0x010044
#define I2C_DATA1                                       0x010045
#define I2C_DATA2                                       0x010046
#define I2C_DATA3                                       0x010047
#define I2C_DATA4                                       0x010048
#define I2C_DATA5                                       0x010049
#define I2C_DATA6                                       0x01004A
#define I2C_DATA7                                       0x01004B
#define I2C_DATA8                                       0x01004C
#define I2C_DATA9                                       0x01004D
#define I2C_DATA10                                      0x01004E
#define I2C_DATA11                                      0x01004F
#define I2C_DATA12                                      0x010050
#define I2C_DATA13                                      0x010051
#define I2C_DATA14                                      0x010052
#define I2C_DATA15                                      0x010053

#define DMA_ABORT_INTERRUPT                             0x0D0020
#define DMA_ABORT_INTERRUPT_ABORT_1                     BIT(5)

/* Default i2c CLK and Data GPIO. These are the default i2c pins */
#define DEFAULT_I2C_SCL                     30
#define DEFAULT_I2C_SDA                     31

#define GPIO_DATA_SM750LE                               0x020018
#define GPIO_DATA_SM750LE_1                             BIT(1)
#define GPIO_DATA_SM750LE_0                             BIT(0)

#define GPIO_DATA_DIRECTION_SM750LE                     0x02001C
#define GPIO_DATA_DIRECTION_SM750LE_1                   BIT(1)
#define GPIO_DATA_DIRECTION_SM750LE_0                   BIT(0)

#endif
