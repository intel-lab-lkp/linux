/* SPDX-License-Identifier: GPL-2.0
 *
 * CS40L26 Boosted Haptic Driver with Integrated DSP and
 * Waveform Memory with Advanced Closed Loop Algorithms and LRA protection
 *
 * Copyright 2025 Cirrus Logic, Inc.
 *
 * Author: Fred Treven <ftreven@opensource.cirrus.com>
 */

#ifndef __MFD_CS40L26_H__
#define __MFD_CS40L26_H__

#include <linux/bitops.h>
#include <linux/firmware/cirrus/cs_dsp.h>
#include <linux/firmware/cirrus/wmfw.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

/* Register Addresses */
#define CS40L26_LASTREG			0x3C7DFE8
#define CS40L26_DEVID			0x0
#define CS40L26_REVID			0x4
#define CS40L26_GLOBAL_ENABLES		0x2014
#define CS40L26_ERROR_RELEASE		0x2034
#define CS40L26_PWRMGT_CTL		0x2900
#define CS40L26_WAKESRC_CTL		0x2904
#define CS40L26_PWRMGT_STS		0x290C
#define CS40L26_REFCLK_INPUT		0x2C04
#define CS40L26_PLL_REFCLK_DETECT_0	0x2C28
#define CS40L26_VBST_CTL_1		0x3800
#define CS40L26_VBST_CTL_2		0x3804
#define CS40L26_BST_IPK_CTL		0x3808
#define CS40L26_TEST_LBST		0x391C
#define CS40L26_DAC_MSM_CONFIG		0x7400
#define CS40L26_TST_DAC_MSM_CONFIG	0x7404
#define CS40L26_IRQ1_STATUS		0x10004
#define CS40L26_IRQ1_EINT_1		0x10010
#define CS40L26_IRQ1_EINT_2		0x10014
#define CS40L26_IRQ1_MASK_1		0x10110
#define CS40L26_IRQ1_MASK_2		0x10114
#define CS40L26_DSP_QUEUE		0x13020
#define CS40L26_DSP1_XMEM_PACKED_0	0x2000000
#define CS40L26_DSP1_SYS_INFO_ID	0x25E0000
#define CS40L26_DSP1_XMEM_UNPACKED24_0	0x2800000
#define CS40L26_DSP1_CCM_CORE_CONTROL	0x2BC1000
#define CS40L26_DSP1_YMEM_PACKED_0	0x2C00000
#define CS40L26_DSP1_YMEM_UNPACKED32_0	0x3000000
#define CS40L26_DSP1_YMEM_UNPACKED24_0	0x3400000
#define CS40L26_DSP1_PMEM_0		0x3800000

/* Device */
#define CS40L26_DEVID_L26		0x40A260
#define CS40L26_DEVID_L27		0x40A270
#define CS40L26_REVID_A1		0xA1
#define CS40L26_REVID_B1		0xB1
#define CS40L26_REVID_B2		0xB2
#define CS40L26_MIN_RESET_PULSE_US	1500
#define CS40L26_CP_READY_DELAY_US	6000
#define CS40L26_SPK_DEFAULT_HIZ		BIT(28)
#define CS40L26_DSP_CCM_CORE_KILL	0x00000080
#define CS40L26_MEM_RDY			BIT(1)

/* Errata */
#define CS40L26_DISABLE_EXPL_MODE	0x0100C080

#define CS40L26_PLL_REFCLK_DET_DISABLE	0x0

/* Boost Converter Control */
#define CS40L26_GLOBAL_EN		BIT(0)

#define CS40L26_BST_IPK_UA_MAX		4800000
#define CS40L26_BST_IPK_UA_DEFAULT	4500000
#define CS40L26_BST_IPK_UA_MIN		1600000
#define CS40L26_BST_IPK_UA_STEP		50000
#define CS40L26_BST_IPK_UA_OFFSET	800000

#define CS40L26_BST_UV_MIN		2500000
#define CS40L26_BST_UV_MAX		11000000
#define CS40L26_BST_UV_STEP		50000

#define CS40L26_BST_CTL_VP		0x00
#define CS40L26_BST_CTL_MASK		GENMASK(7, 0)
#define CS40L26_BST_CTL_SEL_MASK	GENMASK(1, 0)
#define CS40L26_BST_CTL_SEL_FIXED	0x0
#define CS40L26_BST_CTL_LIM_EN_MASK	BIT(2)
#define CS40L26_BST_CTL_LIM_EN		1

#define CS40L26_BST_TIME_US		10000

/* Phase Locked Loop */
#define CS40L26_PLL_REFCLK_LOOP_MASK	BIT(11)
#define CS40L26_PLL_REFCLK_LOOP_SHIFT	11
#define CS40L26_PLL_NUM_SET_ATTEMPTS	5

/* GPIO */
#define CS40L26_EVENT_MAP_GPI_DISABLE	0x1FF

#define CS40L26_A1_B1_EVENT_MAP_1	0x02806FC4
#define CS40L26_A1_B1_EVENT_MAP_2	0x02806FC8

#define CS40L26_B2_EVENT_MAP_1		0x02806FB0
#define CS40L26_B2_EVENT_MAP_2		0x02806FB4

/* Power Management */
#define CS40L26_PM_STDBY_TICKS_OFFSET	16
#define CS40L26_PM_ACTIVE_TICKS_OFFSET	24

#define CS40L26_A1_B1_PM_CUR_STATE	0x02800370
#define CS40L26_A1_B1_PM_STATE_LOCKS	0x02800378
#define CS40L26_A1_B1_PM_STATE_LOCKS3	(CS40L26_A1_B1_PM_STATE_LOCKS + \
					CS40L26_DSP_LOCK3_OFFSET_BYTES)

#define	CS40L26_A1_B1_PM_TIMEOUT_TICKS	0x02800350
#define CS40L26_A1_B1_PM_STDBY_TICKS	(CS40L26_A1_B1_PM_TIMEOUT_TICKS + \
					CS40L26_PM_STDBY_TICKS_OFFSET)
#define CS40L26_A1_B1_PM_ACTIVE_TICKS	(CS40L26_A1_B1_PM_TIMEOUT_TICKS + \
					CS40L26_PM_ACTIVE_TICKS_OFFSET)

#define CS40L26_A1_B1_HALO_STATE	0x02800FA8

#define CS40L26_B2_PM_CUR_STATE		0x02801F98
#define CS40L26_B2_PM_STATE_LOCKS	0x02801FA0
#define CS40L26_B2_PM_STATE_LOCKS3	(CS40L26_B2_PM_STATE_LOCKS + CS40L26_DSP_LOCK3_OFFSET_BYTES)
#define CS40L26_B2_PM_TIMEOUT_TICKS	0x02801F78
#define CS40L26_B2_PM_STDBY_TICKS	(CS40L26_B2_PM_TIMEOUT_TICKS + \
					CS40L26_PM_STDBY_TICKS_OFFSET)
#define CS40L26_B2_PM_ACTIVE_TICKS	(CS40L26_B2_PM_TIMEOUT_TICKS + \
					CS40L26_PM_ACTIVE_TICKS_OFFSET)

#define CS40L26_B2_HALO_STATE		0x02806AF8

#define CS40L26_AUTOSUSPEND_DELAY_MS	2000
#define CS40L26_PM_TICKS_PER_SEC	32768

/* Firmware Handling */
#define CS40L26_FW_ID			0x1800D4
#define CS40L26_FW_A1_B1_MIN_REV	0x070247
#define CS40L26_FW_B2_MIN_REV		0x0A0000

#define CS40L26_NUM_COEFF_FILES 3

/* Algorithms */
#define CS40L26_VIBEGEN_ALGO_ID_A1	0x000400BD
#define CS40L26_VIBEGEN_ALGO_ID_B2	0x000A00BD

#define CS40L26_BUZZGEN_ALGO_ID	0x0004F202
#define CS40L26_A2H_ALGO_ID	0x00040110
#define CS40L26_EXT_ALGO_ID	0x0004013C
#define CS40L26_DSP_ALGO_ID	0x0004F203
#define CS40L26_PM_ALGO_ID	0x0004F206

/* DSP */
#define CS40L26_DSP_LOCK3_OFFSET_BYTES	8
#define CS40L26_DSP_LOCK3_OFFSET_WORDS	(CS40L26_DSP_LOCK3_OFFSET_BYTES / sizeof(u32))
#define CS40L26_DSP_LOCK3_MASK		BIT(1)
#define CS40L26_DSP_HALO_STATE_RUN	2
#define CS40L26_DSP_CTRL_BASE		0x2B80000
#define CS40L26_DSP_POLL_US		1000
#define CS40L26_DSP_TIMEOUT_COUNT	100
#define CS40L26_PM_LOCKS_TIMEOUT_COUNT	10
#define CS40L26_DSP_STATE_TIMEOUT_COUNT	10

#define CS40L26_DSP_CMD_PREVENT_HIBER	0x02000003
#define CS40L26_DSP_CMD_ALLOW_HIBER	0x02000004
#define CS40L26_DSP_CMD_INDEX_MASK	GENMASK(28, 24)
#define CS40L26_DSP_CMD_PAYLOAD_MASK	GENMASK(23, 0)

#define CS40L26_DSP_COMPLETE_CP		0x01000000
#define CS40L26_DSP_COMPLETE_I2S	0x01000002
#define CS40L26_DSP_TRIGGER_CP		0x01000010
#define CS40L26_DSP_TRIGGER_I2S		0x01000012
#define CS40L26_DSP_PM_AWAKE		0x02000002
#define CS40L26_DSP_SYS_ACK		0x0A000000
#define CS40L26_DSP_PANIC		0x0C000000

/* Wake Sources */
#define CS40L26_WKSRC_STS_MASK		GENMASK(9, 4)
#define CS40L26_WKSRC_STS_SHIFT		4
#define CS40L26_WKSRC_STS_EN		BIT(7)
#define CS40L26_WKSRC_POL_SPI		BIT(4)
#define CS40L26_WKSRC_EN_SPI		BIT(9)
#define CS40L26_WKSRC_EN_I2C		BIT(10)
#define CS40L26_WKSRC_GPIO_POL_MASK	GENMASK(3, 0)

/* Interrupts */
#define CS40L26_IRQ_GPIO1_RISE		0
#define CS40L26_IRQ_GPIO1_FALL		1
#define CS40L26_IRQ_WKSRC_STS_ANY	8
#define CS40L26_IRQ_WKSRC_STS_GPIO1	9
#define CS40L26_IRQ_WKSRC_STS_SPI	13
#define CS40L26_IRQ_WKSRC_STS_I2C	14
#define CS40L26_IRQ_BST_OVP_FLAG_RISE	18
#define CS40L26_IRQ_BST_OVP_FLAG_FALL	19
#define CS40L26_IRQ_BST_OVP_ERR		20
#define CS40L26_IRQ_BST_DCM_UVP_ERR	21
#define CS40L26_IRQ_BST_SHORT_ERR	22
#define CS40L26_IRQ_BST_IPK_FLAG	23
#define CS40L26_IRQ_TEMP_WARN_RISE	24
#define CS40L26_IRQ_TEMP_WARN_FALL	25
#define CS40L26_IRQ_TEMP_ERR		26
#define CS40L26_IRQ_AMP_ERR		27
#define CS40L26_IRQ_DSP_RX_QUEUE	31

#define CS40L26_IRQ_1_NBITS		32

#define CS40L26_IRQ_REFCLK_PRESENT	6
#define CS40L26_IRQ_REFCLK_MISSING_FALL	7
#define CS40L26_IRQ_REFCLK_MISSING_RISE	8
#define CS40L26_IRQ_VPMON_CLIPPED	23
#define CS40L26_IRQ_VBSTMON_CLIPPED	24
#define CS40L26_IRQ_VMON_CLIPPED	25
#define CS40L26_IRQ_IMON_CLIPPED	26

#define CS40L26_IRQ_2_NBITS		30

#define CS40L26_IRQ_1_ALL_MASKED	0xFFFFFFFF
#define CS40L26_IRQ_2_ALL_MASKED	0x3FFFFFFF

#define CS40L26_IRQ_STATUS_ASSERT	0x1

/* Playback */
#define CS40L26_STOP_PLAYBACK	0x05000000

#define CS40L26_START_I2S	0x03000002
#define CS40L26_STOP_I2S	0x03000003

/* Error Release */
enum cs40l26_error {
	CS40L26_ERROR_NONE,
	CS40L26_ERROR_AMP_SHORT,
	CS40L26_ERROR_BST_SHORT,
	CS40L26_ERROR_BST_OVP,
	CS40L26_ERROR_BST_DCM_UVP,
	CS40L26_ERROR_TEMP_WARN,
	CS40L26_ERROR_TEMP_ERR,
};

struct cs40l26_irq {
	int virq;
	u32 mask;
	const char *name;
	int (*handler)(void *data);
};

#define CS40L26_IRQ(_irq, _name, _hand)			\
	{						\
		.virq = CS40L26_IRQ_ ## _irq,		\
		.mask = BIT(CS40L26_ ## IRQ_ ## _irq),	\
		.name = _name,				\
		.handler = _hand,			\
	}

enum cs40l26_dsp_state {
	CS40L26_DSP_STATE_HIBERNATE,
	CS40L26_DSP_STATE_SHUTDOWN,
	CS40L26_DSP_STATE_STANDBY,
	CS40L26_DSP_STATE_ACTIVE,
	CS40L26_DSP_STATE_NONE,
};

enum cs40l26_gpio_map {
	CS40L26_GPIO_MAP_A_PRESS,
	CS40L26_GPIO_MAP_A_RELEASE,
	CS40L26_GPIO_MAP_NUM_AVAILABLE,
	CS40L26_GPIO_MAP_INVALID,
};

enum cs40l26_pll {
	CS40L26_PLL_CLOSED,
	CS40L26_PLL_OPEN,
};

enum cs40l50_wseqs {
	CS40L26_WSEQ_POWER_ON,
	CS40L26_WSEQ_ACTIVE,
	CS40L26_WSEQ_STANDBY,
	CS40L26_NUM_WSEQS,
};

struct cs40l26_variant_info {
	u32 pm_cur_state;
	u32 pm_state_locks;
	u32 pm_state_locks3;
	u32 pm_stdby_ticks;
	u32 pm_active_ticks;
	u32 halo_state;
	u32 event_map_1;
	u32 event_map_2;
	u32 fw_min_rev;
	u32 ram_ext_algo_id;
	u32 vibegen_algo_id;
};

struct cs40l26_variant;

struct cs40l26 {
	struct device *dev;
	struct regmap *regmap;
	struct cs_dsp dsp;
	int irq;
	struct mutex lock;
	struct gpio_desc *reset_gpio;
	u32 devid;
	u32 revid;
	const struct cs40l26_variant *variant;
	struct cs_dsp_wseq wseqs[CS40L26_NUM_WSEQS];
	u8 wksrc_sts;
	u8 last_wksrc_pol;
	u32 queue_base;
	u32 queue_len;
	u32 queue_last;
	unsigned int bst_ipk_ua;
	unsigned int vbst_uv;
	const struct firmware *wmfw;
	const struct bus_type *bus;
	struct reg_sequence *irq_masks;
};

struct cs40l26_variant {
	const struct cs40l26_variant_info *info;
	int (*handle_errata)(struct cs40l26 *cs40l26);
};

inline void cs40l26_pm_exit(struct device *dev);
int cs40l26_probe(struct cs40l26 *cs40l26);
int cs40l26_set_pll_loop(struct cs40l26 *cs40l26, const u32 pll_loop);
int cs40l26_dsp_write(struct cs40l26 *cs40l26, const u32 val);
int cs40l26_dsp_state_get(struct cs40l26 *cs40l26, u32 *state);
inline int cs40l26_fw_write(struct cs_dsp *dsp, const char *const name,
			    const unsigned int algo_id, u32 val);
inline int cs40l26_fw_read(struct cs_dsp *dsp, const char *const name,
			   const unsigned int algo_id, u32 *buf);

extern const struct regmap_config cs40l26_regmap;
extern const struct dev_pm_ops cs40l26_pm_ops;

#endif /* __CS40L26_H__ */
