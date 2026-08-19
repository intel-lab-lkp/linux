// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2026 Marek Vasut
 *
 * STM32MP2 UCPD Type-C Driver
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/usb/pd.h>
#include <linux/usb/tcpm.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_mux.h>

#define UCPD_CFGR1					0x00
#define UCPD_CFGR1_UCPDEN				BIT(31)
#define UCPD_CFGR1_RXDMAEN				BIT(30)
#define UCPD_CFGR1_TXDMAEN				BIT(29)
#define UCPD_CFGR1_RXORDSETEN				GENMASK(28, 20)
#define UCPD_CFGR1_RXORDSETEN_SOP			BIT(20)
#define UCPD_CFGR1_RXORDSETEN_SOP_PRIME			BIT(21)
#define UCPD_CFGR1_RXORDSETEN_SOP_PRIME_PRIME		BIT(22)
#define UCPD_CFGR1_RXORDSETEN_HRST			BIT(23)
#define UCPD_CFGR1_RXORDSETEN_CRST			BIT(24)
#define UCPD_CFGR1_RXORDSETEN_SOP_PRIME_DEBUG		BIT(25)
#define UCPD_CFGR1_RXORDSETEN_SOP_PRIME_PRIME_DEBUG	BIT(26)
#define UCPD_CFGR1_RXORDSETEN_SOP_EXT1			BIT(27)
#define UCPD_CFGR1_RXORDSETEN_SOP_EXT2			BIT(28)
#define UCPD_CFGR1_PSC_UCPDCLK				GENMASK(19, 17)
#define UCPD_CFGR1_TRANSWIN				GENMASK(15, 11)
#define UCPD_CFGR1_IFRGAP				GENMASK(10, 6)
#define UCPD_CFGR1_HBITCLKDIV				GENMASK(5, 0)

#define UCPD_CFGR2					0x04
#define UCPD_CFGR2_WUPEN				BIT(3)
#define UCPD_CFGR2_FORCECLK				BIT(2)
#define UCPD_CFGR2_RXFILT2N3				BIT(1)
#define UCPD_CFGR2_RXFILTDIS				BIT(0)

#define UCPD_CFGR3					0x08
#define UCPD_CFGR3_TRIM_CC2_RP				GENMASK(28, 25)
#define UCPD_CFGR3_TRIM_CC2_RD				GENMASK(19, 16)
#define UCPD_CFGR3_TRIM_CC1_RP				GENMASK(12, 9)
#define UCPD_CFGR3_TRIM_CC1_RD				GENMASK(3, 0)

#define UCPD_CR						0x0c
#define UCPD_CR_CC2TCDIS				BIT(21)
#define UCPD_CR_CC1TCDIS				BIT(20)
#define UCPD_CR_RDCH					BIT(18)
#define UCPD_CR_CCENABLE_CC2				BIT(11)
#define UCPD_CR_CCENABLE_CC1				BIT(10)
#define UCPD_CR_ANAMODE					BIT(9)	/* 0:SRC 1:SNK */
#define UCPD_CR_ANASUBMODE				GENMASK(8, 7)
#define UCPD_CR_PHYCCSEL				BIT(6)	/* 0:CC1 1:CC2 */
#define UCPD_CR_PHYRXEN					BIT(5)
#define UCPD_CR_RXMODE					BIT(4)
#define UCPD_CR_TXHRST					BIT(3)
#define UCPD_CR_TXSEND					BIT(2)
#define UCPD_CR_TXMODE					GENMASK(1, 0)
#define UCPD_CR_TXMODE_PACKET				0
#define UCPD_CR_TXMODE_CABLE				1
#define UCPD_CR_TXMODE_BIST				2

#define UCPD_IMR					0x10
#define UCPD_IMR_TYPECEVT2IE				BIT(15)
#define UCPD_IMR_TYPECEVT1IE				BIT(14)
#define UCPD_IMR_RXMSGENDIE				BIT(12)
#define UCPD_IMR_RXOVRIE				BIT(11)
#define UCPD_IMR_RXHRSTDETIE				BIT(10)
#define UCPD_IMR_RXORDDETIE				BIT(9)
#define UCPD_IMR_RXNEIE					BIT(8)
#define UCPD_IMR_TXUNDIE				BIT(6)
#define UCPD_IMR_HRSTSENTIE				BIT(5)
#define UCPD_IMR_HRSTDISCIE				BIT(4)
#define UCPD_IMR_TXMSGABTIE				BIT(3)
#define UCPD_IMR_TXMSGSENTIE				BIT(2)
#define UCPD_IMR_TXMSGDISCIE				BIT(1)
#define UCPD_IMR_TXISIE					BIT(0)

#define UCPD_SR						0x14
#define UCPD_SR_TYPEC_VSTATE_CC2			GENMASK(19, 18)
#define UCPD_SR_TYPEC_VSTATE_CC1			GENMASK(17, 16)
#define UCPD_SR_TYPEC_VSTATE_CC_LOWEST			0
#define UCPD_SR_TYPEC_VSTATE_CC_LOW			1
#define UCPD_SR_TYPEC_VSTATE_CC_HIGH			2
#define UCPD_SR_TYPEC_VSTATE_CC_HIGHEST			3
#define UCPD_SR_TYPECEVT2				BIT(15)
#define UCPD_SR_TYPECEVT1				BIT(14)
#define UCPD_SR_RXERR					BIT(13)
#define UCPD_SR_RXMSGEND				BIT(12)
#define UCPD_SR_RXOVR					BIT(11)
#define UCPD_SR_RXHRSTDET				BIT(10)
#define UCPD_SR_RXORDDET				BIT(9)
#define UCPD_SR_RXNE					BIT(8)
#define UCPD_SR_TXUND					BIT(6)
#define UCPD_SR_HRSTSENT				BIT(5)
#define UCPD_SR_HRSTDISC				BIT(4)
#define UCPD_SR_TXMSGABT				BIT(3)
#define UCPD_SR_TXMSGSENT				BIT(2)
#define UCPD_SR_TXMSGDISC				BIT(1)
#define UCPD_SR_TXIS					BIT(0)

#define UCPD_ICR					0x18
#define UCPD_ICR_TYPECEVT2CF				BIT(15)
#define UCPD_ICR_TYPECEVT1CF				BIT(14)
#define UCPD_ICR_RXMSGENDCF				BIT(12)
#define UCPD_ICR_RXOVRCF				BIT(11)
#define UCPD_ICR_RXHRSTDETCF				BIT(10)
#define UCPD_ICR_RXORDDETCF				BIT(9)
#define UCPD_ICR_TXUNDCF				BIT(6)
#define UCPD_ICR_HRSTSENTCF				BIT(5)
#define UCPD_ICR_HRSTDISCCF				BIT(4)
#define UCPD_ICR_TXMSGABTCF				BIT(3)
#define UCPD_ICR_TXMSGSENTCF				BIT(2)
#define UCPD_ICR_TXMSGDISCCF				BIT(1)

#define UCPD_TX_ORDSETR					0x1c
#define UCPD_TX_ORDSETR_TXORDSET			GENMASK(19, 0)
#define SYNC1						0x18
#define SYNC2						0x11
#define SYNC3						0x06
#define RST1						0x07
#define RST2						0x19
#define EOP						0x0d
#define ORDSET(x, y, z, w)				((x) | ((x) << 5) | ((x) << 10) | ((x) << 15))
#define UCPD_TX_ORDSETR_TXORDSET_SOP			ORDSET(SYNC1, SYNC1, SYNC1, SYNC2)
#define UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME		ORDSET(SYNC1, SYNC1, SYNC3, SYNC3)
#define UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_PRIME	ORDSET(SYNC1, SYNC3, SYNC1, SYNC3)
#define UCPD_TX_ORDSETR_TXORDSET_HRST			ORDSET(RST1, RST1, RST1, RST2)
#define UCPD_TX_ORDSETR_TXORDSET_CRST			ORDSET(RST1, SYNC1, RST1, RST3)
#define UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_DEBUG	ORDSET(SYNC1, RST2, RST2, SYNC3)
#define UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_PRIME_DEBUG	ORDSET(SYNC1, RST2, SYNC3, SYNC2)

#define UCPD_TX_PAYSZR					0x20
#define UCPD_TX_PAYSZR_TXPAYSZ				GENMASK(9, 0)

#define UCPD_TXDR					0x24
#define UCPD_TXDR_TXDATA				GENMASK(7, 0)

#define UCPD_RX_ORDSETR					0x28
#define UCPD_RX_ORDSETR_RXSOPKINVALID			GENMASK(6, 4)
#define UCPD_RX_ORDSETR_RXSOP3OF4			BIT(3)
#define UCPD_RX_ORDSETR_RXORDSET			GENMASK(2, 0)

#define UCPD_RX_PAYSZR					0x2c
#define UCPD_RX_PAYSZR_RXPAYSZ				GENMASK(9, 0)

#define UCPD_RXDR					0x30
#define UCPD_RXDR_RXDATA				GENMASK(7, 0)

#define UCPD_RX_ORDEXTR1				0x34
#define UCPD_RX_ORDEXTR1_RXSOPX1			GENMASK(19, 0)

#define UCPD_RX_ORDEXTR2				0x38
#define UCPD_RX_ORDEXTR2_RXSOPX2			GENMASK(19, 0)

/* IDs that have no impact on the IP operation */
#define UCPD_VERR					0x3f4
#define UCPD_IPIDR					0x3f8
#define UCPD_SIDR					0x3fc

/* OTP trim offsets (base offset is handled by NVMEM) */
#define OTP_TRIM_1A5_RP					GENMASK(15, 12)
#define OTP_TRIM_CC2_RD					GENMASK(11, 8)
#define OTP_TRIM_3A0_RP					GENMASK(7, 4)
#define OTP_TRIM_CC1_RD					GENMASK(3, 0)

static const struct regmap_range ucpd_readable_ranges[] = {
	regmap_reg_range(UCPD_CFGR1, UCPD_SR),
	regmap_reg_range(UCPD_TX_ORDSETR, UCPD_RX_ORDEXTR2),
	regmap_reg_range(UCPD_VERR, UCPD_SIDR),
};

static const struct regmap_access_table ucpd_readable_table = {
	.yes_ranges	= ucpd_readable_ranges,
	.n_yes_ranges	= ARRAY_SIZE(ucpd_readable_ranges),
};

static const struct regmap_range ucpd_writeable_ranges[] = {
	regmap_reg_range(UCPD_CFGR1, UCPD_IMR),
	regmap_reg_range(UCPD_ICR, UCPD_TXDR),
	regmap_reg_range(UCPD_RX_ORDEXTR1, UCPD_RX_ORDEXTR2),
};

static const struct regmap_access_table ucpd_writeable_table = {
	.yes_ranges	= ucpd_writeable_ranges,
	.n_yes_ranges	= ARRAY_SIZE(ucpd_writeable_ranges),
};

static const struct regmap_range ucpd_volatile_ranges[] = {
	regmap_reg_range(UCPD_SR, UCPD_SR),
	regmap_reg_range(UCPD_TXDR, UCPD_TXDR),
	regmap_reg_range(UCPD_RX_ORDSETR, UCPD_RXDR),
};

static const struct regmap_access_table ucpd_volatile_table = {
	.yes_ranges	= ucpd_volatile_ranges,
	.n_yes_ranges	= ARRAY_SIZE(ucpd_volatile_ranges),
};

static const struct regmap_config ucpd_regmap_config = {
	.reg_bits		= 32,
	.reg_stride		= 4,
	.val_bits		= 32,
	.max_register		= UCPD_SIDR,
	.rd_table		= &ucpd_readable_table,
	.wr_table		= &ucpd_writeable_table,
	.volatile_table		= &ucpd_volatile_table,
	.cache_type		= REGCACHE_NONE/*FLAT*/,
};

struct ucpd {
	struct device *dev;
	struct tcpm_port *tcpm_port;
	struct tcpc_dev tcpc_dev;
	struct regmap *regmap;
	struct reset_control *phy_reset;
	struct regulator *vbus;
	struct gpio_desc *vbus_in;
	struct gpio_descs *vconn_gpios;
	struct typec_switch *orient_sw;

	/* Rp/Rd trimming */
	u32 trim;

	/* lock for sharing ucpd states */
	struct mutex lock;

	/* port status */
	bool vconn_on;
	bool vbus_on;
	bool tx_pending;
	enum typec_cc_polarity cc_polarity;
	enum typec_cc_status cc1;
	enum typec_cc_status cc2;
	enum typec_role pwr_role;
	enum typec_data_role data_role;
};

static struct ucpd *tcpm_to_ucpd(struct tcpc_dev *dev)
{
	return container_of(dev, struct ucpd, tcpc_dev);
}

static void tcpm_load_trim_rd(struct ucpd *ucpd)
{
	const u32 rd_cc1 = FIELD_GET(OTP_TRIM_CC1_RD, ucpd->trim);
	const u32 rd_cc2 = FIELD_GET(OTP_TRIM_CC2_RD, ucpd->trim);
	const u32 msk = UCPD_CFGR3_TRIM_CC2_RD | UCPD_CFGR3_TRIM_CC1_RD;
	const u32 val = FIELD_PREP(UCPD_CFGR3_TRIM_CC1_RD, rd_cc1) |
			FIELD_PREP(UCPD_CFGR3_TRIM_CC2_RD, rd_cc2);

	regmap_update_bits(ucpd->regmap, UCPD_CFGR3, msk, val);
}

static void tcpm_load_trim_rp(struct ucpd *ucpd, bool rp3a0)
{
	const u32 rp_1a5 = FIELD_GET(OTP_TRIM_1A5_RP, ucpd->trim);
	const u32 rp_3a0 = FIELD_GET(OTP_TRIM_3A0_RP, ucpd->trim);
	const u32 msk = UCPD_CFGR3_TRIM_CC2_RP | UCPD_CFGR3_TRIM_CC1_RP;
	const u32 val = FIELD_PREP(UCPD_CFGR3_TRIM_CC1_RP, rp3a0 ? rp_1a5 : rp_3a0) |
			FIELD_PREP(UCPD_CFGR3_TRIM_CC2_RP, rp3a0 ? rp_1a5 : rp_3a0);

	regmap_update_bits(ucpd->regmap, UCPD_CFGR3, msk, val);
}

static int tcpm_init(struct tcpc_dev *dev)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);

	reset_control_assert(ucpd->phy_reset);
	reset_control_status(ucpd->phy_reset);
	fsleep(1000);
	reset_control_deassert(ucpd->phy_reset);

	/*
	 * UCPD clock is 16 MHz / 2 = 8 MHz
	 * Bit clock is 16 MHz / 2 / (13 + 1) ~= 570 kHz
	 */
	regmap_write(ucpd->regmap, UCPD_CFGR1,
		     UCPD_CFGR1_RXORDSETEN_SOP |
		     UCPD_CFGR1_RXORDSETEN_SOP_PRIME |
		     UCPD_CFGR1_RXORDSETEN_SOP_PRIME_PRIME |
		     UCPD_CFGR1_RXORDSETEN_HRST |
		     UCPD_CFGR1_RXORDSETEN_CRST |
		     FIELD_PREP(UCPD_CFGR1_PSC_UCPDCLK, 1) |
		     FIELD_PREP(UCPD_CFGR1_TRANSWIN, 7) |
		     FIELD_PREP(UCPD_CFGR1_IFRGAP, 0x1f) |
		     FIELD_PREP(UCPD_CFGR1_HBITCLKDIV, 13));

	regmap_set_bits(ucpd->regmap, UCPD_CFGR1, UCPD_CFGR1_UCPDEN);

	tcpm_load_trim_rd(ucpd);
	tcpm_load_trim_rp(ucpd, true);

	regmap_set_bits(ucpd->regmap, UCPD_CR, UCPD_CR_CCENABLE_CC1 | UCPD_CR_CCENABLE_CC2);

	regmap_write(ucpd->regmap, UCPD_ICR,
		     UCPD_IMR_TYPECEVT2IE | UCPD_IMR_TYPECEVT1IE |
		     UCPD_IMR_HRSTSENTIE | UCPD_IMR_HRSTDISCIE);
	regmap_write(ucpd->regmap, UCPD_IMR,
		     UCPD_IMR_TYPECEVT2IE | UCPD_IMR_TYPECEVT1IE |
		     UCPD_IMR_HRSTSENTIE | UCPD_IMR_HRSTDISCIE);

	return 0;
}

static int tcpm_get_vbus(struct tcpc_dev *dev)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);

	return gpiod_get_value_cansleep(ucpd->vbus_in);
}

static int tcpm_set_cc(struct tcpc_dev *dev, enum typec_cc_status cc)
{
	const u32 ccmsk = UCPD_CR_CCENABLE_CC2 | UCPD_CR_CCENABLE_CC1 | UCPD_CR_ANASUBMODE;
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	int ret = 0;
	u32 ccset;

	mutex_lock(&ucpd->lock);

	if (ucpd->vconn_on) {
		if (ucpd->cc_polarity == TYPEC_POLARITY_CC1)
			ccset = UCPD_CR_CCENABLE_CC1;
		if (ucpd->cc_polarity == TYPEC_POLARITY_CC2)
			ccset = UCPD_CR_CCENABLE_CC2;
	} else {
		ccset = UCPD_CR_CCENABLE_CC1 | UCPD_CR_CCENABLE_CC2;
	}

	switch (cc) {
	case TYPEC_CC_OPEN:
		regmap_clear_bits(ucpd->regmap, UCPD_CR, ccmsk);
		break;
	case TYPEC_CC_RA:
		regmap_update_bits(ucpd->regmap, UCPD_CR, ccmsk,
				   ccset | FIELD_PREP(UCPD_CR_ANASUBMODE, 0));
		break;
	case TYPEC_CC_RD:
		regmap_update_bits(ucpd->regmap, UCPD_CR, ccmsk | UCPD_CR_ANAMODE,
				   ccset | UCPD_CR_ANAMODE | FIELD_PREP(UCPD_CR_ANASUBMODE, 1));
		break;
	case TYPEC_CC_RP_DEF:
	case TYPEC_CC_RP_1_5:
		tcpm_load_trim_rp(ucpd, false);
		regmap_update_bits(ucpd->regmap, UCPD_CR, ccmsk | UCPD_CR_ANAMODE,
				   ccset | FIELD_PREP(UCPD_CR_ANASUBMODE, 2));
		break;
	case TYPEC_CC_RP_3_0:
		tcpm_load_trim_rp(ucpd, true);
		regmap_update_bits(ucpd->regmap, UCPD_CR, ccmsk | UCPD_CR_ANAMODE,
				   ccset | FIELD_PREP(UCPD_CR_ANASUBMODE, 3));
		break;
	default:
		dev_err(ucpd->dev, "unsupported cc value %d", cc);
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&ucpd->lock);

	return ret;
}

static enum typec_cc_status tcpm_decode_cc(const int cr, const int cc)
{
	bool sink = !!(cr & UCPD_CR_ANAMODE);

	switch (cc) {
	case UCPD_SR_TYPEC_VSTATE_CC_LOWEST:
		return TYPEC_CC_RA;
	case UCPD_SR_TYPEC_VSTATE_CC_LOW:
		return TYPEC_CC_RD;
	case UCPD_SR_TYPEC_VSTATE_CC_HIGH:
		return sink ? TYPEC_CC_RP_1_5 : TYPEC_CC_OPEN;
	default:
		return sink ? TYPEC_CC_RP_3_0 : TYPEC_CC_OPEN;
	}
}

static int tcpm_get_cc(struct tcpc_dev *dev,
		       enum typec_cc_status *cc1,
		       enum typec_cc_status *cc2)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	int cr, sr;

	mutex_lock(&ucpd->lock);

	regmap_read(ucpd->regmap, UCPD_CR, &cr);
	regmap_read(ucpd->regmap, UCPD_SR, &sr);

	if (cr & UCPD_CR_CCENABLE_CC1)
		ucpd->cc1 = tcpm_decode_cc(cr, FIELD_GET(UCPD_SR_TYPEC_VSTATE_CC1, sr));
	*cc1 = ucpd->cc1;

	if (cr & UCPD_CR_CCENABLE_CC2)
		ucpd->cc2 = tcpm_decode_cc(cr, FIELD_GET(UCPD_SR_TYPEC_VSTATE_CC2, sr));
	*cc2 = ucpd->cc2;

	mutex_unlock(&ucpd->lock);

	return 0;
}

static int tcpm_set_polarity(struct tcpc_dev *dev,
			     enum typec_cc_polarity polarity)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	int ret;

	regmap_update_bits(ucpd->regmap, UCPD_CR, UCPD_CR_PHYCCSEL,
			   (polarity == TYPEC_POLARITY_CC1) ? 0 : UCPD_CR_PHYCCSEL);

	if (!ucpd->orient_sw)
		return 0;

	if (polarity == TYPEC_POLARITY_CC1)
		ret = typec_switch_set(ucpd->orient_sw, TYPEC_ORIENTATION_NORMAL);
	else if (polarity == TYPEC_POLARITY_CC2)
		ret = typec_switch_set(ucpd->orient_sw, TYPEC_ORIENTATION_REVERSE);
	else
		ret = typec_switch_set(ucpd->orient_sw, TYPEC_ORIENTATION_NONE);
	if (ret)
		dev_err(ucpd->dev, "failed to set orientation %d\n", ret);

	return ret;
}

static int tcpm_set_vconn(struct tcpc_dev *dev, bool on)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	DECLARE_BITMAP(bitmap, 2) = { };
	enum typec_cc_status cc1, cc2;
	int cr, sr;
	int ret;

	mutex_lock(&ucpd->lock);
	if (ucpd->vconn_on == on) {
		dev_err(ucpd->dev, "vconn is already %d\n", on);
		goto done;
	}

	regmap_read(ucpd->regmap, UCPD_CR, &cr);
	regmap_read(ucpd->regmap, UCPD_SR, &sr);

	if (cr & UCPD_CR_CCENABLE_CC1)
		cc1 = tcpm_decode_cc(cr, FIELD_GET(UCPD_SR_TYPEC_VSTATE_CC1, sr));
	else
		cc1 = TYPEC_CC_RA;

	if (cr & UCPD_CR_CCENABLE_CC2)
		cc2 = tcpm_decode_cc(cr, FIELD_GET(UCPD_SR_TYPEC_VSTATE_CC2, sr));
	else
		cc2 = TYPEC_CC_RA;

	if (cc1 == TYPEC_CC_OPEN || cc1 == TYPEC_CC_RA) {
		ucpd->cc_polarity = TYPEC_POLARITY_CC2;
	} else if (cc2 == TYPEC_CC_OPEN || cc2 == TYPEC_CC_RA) {
		ucpd->cc_polarity = TYPEC_POLARITY_CC1;
	} else {
		dev_err(ucpd->dev, "unknown CC polarity %d %d\n", cc1, cc2);
		goto done;
	}

	if (on) {
		regmap_update_bits(ucpd->regmap, UCPD_CR,
				   UCPD_CR_CCENABLE_CC2 | UCPD_CR_CCENABLE_CC1,
				   (ucpd->cc_polarity == TYPEC_POLARITY_CC1) ?
				   UCPD_CR_CCENABLE_CC1 : UCPD_CR_CCENABLE_CC2);
		bitmap_write(bitmap, (ucpd->cc_polarity == TYPEC_POLARITY_CC1) ? 2 : 1, 0, 2);
		ret = gpiod_multi_set_value_cansleep(ucpd->vconn_gpios, bitmap);
	} else {
		bitmap_write(bitmap, 0, 0, 2);
		ret = gpiod_multi_set_value_cansleep(ucpd->vconn_gpios, bitmap);
		regmap_set_bits(ucpd->regmap, UCPD_CR,
				UCPD_CR_CCENABLE_CC2 | UCPD_CR_CCENABLE_CC1);
	}

	ucpd->vconn_on = on;

done:
	mutex_unlock(&ucpd->lock);

	return ret;
}

static int tcpm_set_vbus(struct tcpc_dev *dev, bool on, bool charge)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	int i, ret = 0;

	mutex_lock(&ucpd->lock);
	if (ucpd->vbus_on == on) {
		dev_err(ucpd->dev, "vbus is already %d\n", on);
		goto done;
	}

	if (on)
		ret = regulator_enable(ucpd->vbus);
	else
		ret = regulator_disable(ucpd->vbus);

	/* Wait a bit for the regulator and readback to sync. */
	for (i = 0; i < 10; i++) {
		if (regulator_is_enabled(ucpd->vbus) == gpiod_get_value_cansleep(ucpd->vbus_in))
			break;
		fsleep(100);
	}
	tcpm_vbus_change(ucpd->tcpm_port);

	if (ret < 0) {
		dev_err(ucpd->dev, "cannot set vbus regulator %d, ret=%d",
			on, ret);
		goto done;
	}

	ucpd->vbus_on = on;

done:
	mutex_unlock(&ucpd->lock);

	return ret;
}

static int tcpm_set_pd_rx(struct tcpc_dev *dev, bool on)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	const u32 rx_imr_mask = UCPD_IMR_RXNEIE | UCPD_IMR_RXORDDETIE |
				UCPD_IMR_RXHRSTDETIE | UCPD_IMR_RXOVRIE;
	const u32 rx_icr_mask = UCPD_ICR_RXORDDETCF | UCPD_ICR_RXHRSTDETCF |
				UCPD_ICR_RXOVRCF;

	mutex_lock(&ucpd->lock);

	if (on) {
		/* Clear out stale bits, unmask and enable PHY PD RX */
		regmap_set_bits(ucpd->regmap, UCPD_ICR, rx_icr_mask);
		regmap_set_bits(ucpd->regmap, UCPD_IMR, rx_imr_mask);
		regmap_set_bits(ucpd->regmap, UCPD_CR, UCPD_CR_PHYRXEN);
	} else {
		/* Disable PHY PD RX and clear out stale bits. */
		regmap_clear_bits(ucpd->regmap, UCPD_CR, UCPD_CR_PHYRXEN);
		regmap_clear_bits(ucpd->regmap, UCPD_IMR, rx_imr_mask);
	}

	mutex_unlock(&ucpd->lock);

	return 0;
}

static int tcpm_set_roles(struct tcpc_dev *dev, bool attached,
			  enum typec_role pwr, enum typec_data_role data)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);

	ucpd->pwr_role = pwr;
	ucpd->data_role = data;

	return 0;
}

static int ucpd_pd_get_ordset(enum tcpm_transmit_type type, u32 *ordset)
{
	if (type == TCPC_TX_SOP)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_SOP;
	else if (type == TCPC_TX_SOP_PRIME)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME;
	else if (type == TCPC_TX_SOP_PRIME_PRIME)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_PRIME;
	else if (type == TCPC_TX_HARD_RESET)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_HRST;
	else if (type == TCPC_TX_CABLE_RESET)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_CRST;
	else if (type == TCPC_TX_SOP_DEBUG_PRIME)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_DEBUG;
	else if (type == TCPC_TX_SOP_DEBUG_PRIME_PRIME)
		*ordset = UCPD_TX_ORDSETR_TXORDSET_SOP_PRIME_PRIME_DEBUG;
	else
		return -EINVAL;

	return 0;
}

static int ucpd_pd_send_message(struct ucpd *ucpd, enum tcpm_transmit_type type,
				const struct pd_message *msg)
{
	const int len = (pd_header_cnt_le(msg->header) * 4) + 2;
	u8 *mh = (u8 *)(&msg->header);
	u8 *mp = (u8 *)(msg->payload);
	int count = 0, ret;
	u32 ordset, val;

	regmap_update_bits(ucpd->regmap, UCPD_CR,
			   UCPD_CR_TXHRST | UCPD_CR_TXSEND | UCPD_CR_TXMODE,
			   UCPD_CR_TXMODE_PACKET);

	ret = ucpd_pd_get_ordset(type, &ordset);
	if (ret)
		return ret;

	regmap_write(ucpd->regmap, UCPD_TX_ORDSETR, ordset);
	regmap_write(ucpd->regmap, UCPD_TX_PAYSZR, len);

	if (ucpd->tx_pending) {
		regmap_read_poll_timeout(ucpd->regmap, UCPD_SR,
					       val, val & UCPD_SR_TXMSGSENT,
					       0, 100000);
		regmap_write(ucpd->regmap, UCPD_ICR, UCPD_ICR_TXMSGSENTCF);
		ucpd->tx_pending = false;
	}

	/* Start the transfer */
	regmap_set_bits(ucpd->regmap, UCPD_CR, UCPD_CR_TXSEND);

	while (count < len) {
		ret = regmap_read_poll_timeout(ucpd->regmap, UCPD_SR,
					       val, val & UCPD_SR_TXIS,
					       0, 1000000);
		if (ret)
			return ret;

		/* First two bytes are header, rest is message */
		if (count < 2)
			regmap_write(ucpd->regmap, UCPD_TXDR, mh[count]);
		else
			regmap_write(ucpd->regmap, UCPD_TXDR, mp[count - 2]);

		count++;
	}

	ucpd->tx_pending = true;

	return 0;
}

static int tcpm_pd_transmit(struct tcpc_dev *dev, enum tcpm_transmit_type type,
			    const struct pd_message *msg, unsigned int negotiated_rev)
{
	struct ucpd *ucpd = tcpm_to_ucpd(dev);
	u32 ordset;
	int ret;

	mutex_lock(&ucpd->lock);
	switch (type) {
	case TCPC_TX_SOP:
		ret = ucpd_pd_send_message(ucpd, type, msg);
		break;
	case TCPC_TX_HARD_RESET:
		ret = ucpd_pd_get_ordset(type, &ordset);
		if (ret)
			return ret;

		regmap_write(ucpd->regmap, UCPD_TX_ORDSETR, ordset);
		regmap_update_bits(ucpd->regmap, UCPD_CR,
				   UCPD_CR_TXHRST | UCPD_CR_TXSEND | UCPD_CR_TXMODE,
				   UCPD_CR_TXHRST | UCPD_CR_TXMODE_PACKET);
		break;
	case TCPC_TX_CABLE_RESET:
		ret = ucpd_pd_get_ordset(type, &ordset);
		if (ret)
			return ret;

		regmap_write(ucpd->regmap, UCPD_TX_ORDSETR, ordset);
		regmap_update_bits(ucpd->regmap, UCPD_CR,
				   UCPD_CR_TXHRST | UCPD_CR_TXSEND | UCPD_CR_TXMODE,
				   UCPD_CR_TXHRST | UCPD_CR_TXMODE_CABLE);
		break;
	case TCPC_TX_BIST_MODE_2:
		regmap_update_bits(ucpd->regmap, UCPD_CR,
				   UCPD_CR_TXHRST | UCPD_CR_TXSEND | UCPD_CR_TXMODE,
				   UCPD_CR_TXHRST | UCPD_CR_TXMODE_BIST);
		ret = 0;
		break;
	default:
		dev_err(ucpd->dev, "type %d not supported", type);
		ret = -EINVAL;
	}
	mutex_unlock(&ucpd->lock);

	return ret;
}

static void ucpd_pd_read_message(struct ucpd *ucpd)
{
	const u32 mask = UCPD_SR_RXORDDET | UCPD_SR_RXNE | UCPD_SR_RXMSGEND;
	struct pd_message pd_msg = { 0 };
	u8 *mh = (u8 *)(&(pd_msg.header));
	u8 *mp = (u8 *)(pd_msg.payload);
	u32 val, rxdata;
	int count = 0;
	int len, ret;

	/* RX */
	for (;;) {
		ret = regmap_read_poll_timeout(ucpd->regmap, UCPD_SR,
					       val, val & mask,
					       0, PD_T_RECEIVER_RESPONSE * 1000);
		if (ret) {
			dev_err(ucpd->dev, "PD RX timeout (SR 0x%x, ret=%d)\n",
				val, ret);
			return;
		}

		if (val & UCPD_SR_RXORDDET)
			regmap_write(ucpd->regmap, UCPD_ICR, UCPD_ICR_RXORDDETCF);

		if (val & UCPD_SR_RXNE) {
			regmap_read(ucpd->regmap, UCPD_RXDR, &rxdata);

			/* First two bytes are header, rest is message */
			if (count < 2)
				mh[count] = rxdata & 0xff;
			else
				mp[count - 2] = rxdata & 0xff;
			count++;
		}

		if (val & UCPD_SR_RXMSGEND) {
			regmap_write(ucpd->regmap, UCPD_ICR, UCPD_ICR_RXMSGENDCF);
			break;
		}
	}

	len = pd_header_cnt_le(pd_msg.header) * 4;

	/* Add 4 to length to include the CRC */
	if (len > PD_MAX_PAYLOAD * 4) {
		dev_err(ucpd->dev, "PD RX message too long %d", len);
		return;
	}

	/*
	 * Check if we've read off a GoodCRC message. If so then indicate to
	 * TCPM that the previous transmission has completed. Otherwise we pass
	 * the received message over to TCPM for processing.
	 *
	 * We make this check here instead of basing the reporting decision on
	 * the IRQ event type, as it's possible for the ucpd to report the
	 * TX_SUCCESS and GCRCSENT events out of order on occasion, so we need
	 * to check the message type to ensure correct reporting to TCPM.
	 */
	if (!len && (pd_header_type_le(pd_msg.header) == PD_CTRL_GOOD_CRC)) {
		tcpm_pd_transmit_complete(ucpd->tcpm_port, TCPC_TX_SUCCESS);
	} else {
		struct pd_message msg = {
			.header = PD_HEADER_LE(PD_CTRL_GOOD_CRC,
					       ucpd->pwr_role,
					       ucpd->data_role,
					       pd_header_rev_le(pd_msg.header),
					       pd_header_msgid_le(pd_msg.header),
					       0),
		};
		ucpd_pd_send_message(ucpd, TCPC_TX_SOP, &msg);
		tcpm_pd_receive(ucpd->tcpm_port, &pd_msg, TCPC_TX_SOP);
	}
}

static irqreturn_t ucpd_irq(int irq, void *dev_id)
{
	struct ucpd *ucpd = dev_id;
	int sr, mr;

	mutex_lock(&ucpd->lock);

	regmap_read(ucpd->regmap, UCPD_SR, &sr);
	regmap_read(ucpd->regmap, UCPD_IMR, &mr);
	sr &= mr;

	if (sr & UCPD_SR_RXOVR) {
		regmap_write(ucpd->regmap, UCPD_ICR, UCPD_ICR_RXOVRCF);
		dev_warn_ratelimited(ucpd->dev, "PD RX overflow\n");
	}

	/* Received hard reset */
	if (sr & UCPD_SR_RXHRSTDET) {
		regmap_write(ucpd->regmap, UCPD_ICR, UCPD_ICR_RXHRSTDETCF);
		tcpm_pd_hard_reset(ucpd->tcpm_port);
	}

	/* Received ordered set received or data, RX PD message */
	if (sr & (UCPD_SR_RXORDDET | UCPD_SR_RXNE))
		ucpd_pd_read_message(ucpd);

	/* Handle CC line changes only for the enabled CC lines */
	if (sr & (UCPD_SR_TYPECEVT2 | UCPD_SR_TYPECEVT1)) {
		regmap_write(ucpd->regmap, UCPD_ICR,
			     sr & (UCPD_ICR_TYPECEVT2CF | UCPD_ICR_TYPECEVT1CF));

		tcpm_cc_change(ucpd->tcpm_port);
		tcpm_vbus_change(ucpd->tcpm_port);
	}

	mutex_unlock(&ucpd->lock);
	return IRQ_HANDLED;
}

static int ucpd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data *clk;
	void __iomem *mmio;
	struct ucpd *ucpd;
	int irq, ret;

	ucpd = devm_kzalloc(dev, sizeof(*ucpd), GFP_KERNEL);
	if (!ucpd)
		return -ENOMEM;

	ret = nvmem_cell_read_variable_le_u32(dev, "trim", &ucpd->trim);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to access nvmem\n");

	mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmio))
		return PTR_ERR(mmio);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ucpd->dev = dev;
	mutex_init(&ucpd->lock);

	ret = devm_regulator_get_enable(ucpd->dev, "vdd33");
	if (ret)
		return ret;

	ucpd->vconn_gpios = devm_gpiod_get_array(dev, "vconn", GPIOD_OUT_LOW);
	if (IS_ERR(ucpd->vconn_gpios))
		return PTR_ERR(ucpd->vconn_gpios);

	if (ucpd->vconn_gpios->ndescs != 2)
		return -EINVAL;

	ucpd->phy_reset = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(ucpd->phy_reset))
		return dev_err_probe(dev, PTR_ERR(ucpd->phy_reset),
				     "Failed to get PHY reset\n");

	ret = devm_clk_bulk_get_all_enabled(dev, &clk);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	ucpd->regmap = devm_regmap_init_mmio(dev, mmio, &ucpd_regmap_config);
	if (IS_ERR(ucpd->regmap))
		return dev_err_probe(dev, PTR_ERR(ucpd->regmap),
				     "Failed to initialize regmap\n");

	/* Set TCPC callbacks */
	ucpd->tcpc_dev.init = tcpm_init;
	ucpd->tcpc_dev.get_vbus = tcpm_get_vbus;
	ucpd->tcpc_dev.set_vbus = tcpm_set_vbus;
	ucpd->tcpc_dev.set_vconn = tcpm_set_vconn;
	ucpd->tcpc_dev.set_cc = tcpm_set_cc;
	ucpd->tcpc_dev.get_cc = tcpm_get_cc;
	ucpd->tcpc_dev.set_polarity = tcpm_set_polarity;
	ucpd->tcpc_dev.set_pd_rx = tcpm_set_pd_rx;
	ucpd->tcpc_dev.set_roles = tcpm_set_roles;
	ucpd->tcpc_dev.pd_transmit = tcpm_pd_transmit;

	ucpd->tcpc_dev.fwnode = device_get_named_child_node(dev, "connector");
	if (IS_ERR(ucpd->tcpc_dev.fwnode))
		return PTR_ERR(ucpd->tcpc_dev.fwnode);

	platform_set_drvdata(pdev, ucpd);

	/* Vbus input GPIO is on the connector subnode. */
	ucpd->vbus_in = devm_fwnode_gpiod_get(dev, ucpd->tcpc_dev.fwnode,
					      "vbus", GPIOD_IN, "vbus");
	if (IS_ERR(ucpd->vbus_in)) {
		ret = PTR_ERR(ucpd->vbus_in);
		goto tcpm_put_fwnode;
	}

	ucpd->vbus = devm_regulator_get(ucpd->dev, "vbus");
	if (IS_ERR(ucpd->vbus)) {
		ret = PTR_ERR(ucpd->vbus);
		goto tcpm_put_fwnode;
	}

	ucpd->orient_sw = fwnode_typec_switch_get(ucpd->tcpc_dev.fwnode);
	if (IS_ERR(ucpd->orient_sw)) {
		return dev_err_probe(dev, PTR_ERR(ucpd->orient_sw),
				     "Failed to get orientation switch\n");
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, ucpd_irq,
					IRQF_ONESHOT, "ucpd", ucpd);
	if (ret < 0)
		goto tcpm_put_fwnode;

	ucpd->tcpm_port = tcpm_register_port(dev, &ucpd->tcpc_dev);
	if (IS_ERR(ucpd->tcpm_port)) {
		ret = PTR_ERR(ucpd->tcpm_port);
		goto tcpm_put_fwnode;
	}

	return ret;

tcpm_put_fwnode:
	fwnode_handle_put(ucpd->tcpc_dev.fwnode);

	return ret;
}

static void ucpd_remove(struct platform_device *pdev)
{
	struct ucpd *ucpd = platform_get_drvdata(pdev);

	tcpm_unregister_port(ucpd->tcpm_port);
	fwnode_handle_put(ucpd->tcpc_dev.fwnode);
}

static const struct of_device_id ucpd_dt_match[] = {
	{ .compatible = "st,stm32mp25-ucpd" },
	{},
};
MODULE_DEVICE_TABLE(of, ucpd_dt_match);

static struct platform_driver ucpd_driver = {
	.probe = ucpd_probe,
	.remove = ucpd_remove,
	.driver = {
		.name = "ucpd",
		.of_match_table = ucpd_dt_match,
	},
};
module_platform_driver(ucpd_driver);

MODULE_AUTHOR("Marek Vasut");
MODULE_DESCRIPTION("ST UCPD Type-C Chip Driver");
MODULE_LICENSE("GPL");
