// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Linear LTC4266 PoE PSE Controller
 *
 * Original work:
 *    Copyright 2019 Cradlepoint Technology, Inc.
 *    Cradlepoint Technology, Inc.  <source@cradlepoint.com>
 *
 * Re-written in 2026:
 *    Copyright 2026 Ericsson Software Technology
 *    Kyle Swenson <kyle.swenson@est.tech>
 *
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pse-pd/pse.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#define LTC4266_MAX_PORTS			4

/* The minimum and maximum here depend on the resolution of the I_CUT field,
 * which is 18.75mA.  To get 1000mW with a 50V port voltage, we need 20mA; but
 * with 18.75 mA steps we end up with a current limit of 37.5 mA resulting in
 * an actual power limit of 1875mW.
 */
#define LTC4266_PW_LIMIT_MAX			30000
#define LTC4266_PW_LIMIT_MIN			1000

/* Nominal PI voltage reported when the port is not delivering power and no
 * "vpwr-supply" is described in the device tree: V_Port_PSE min for a Type 2
 * PSE. IEEE 802.3-2022 Table 33-11 item 1 specifies the POWER_ON state output
 * voltage as 50.0 V to 57.0 V for a Type 2 PSE, 44.0 V to 57.0 V for Type 1.
 */
#define LTC4266_VPORT_NOMINAL_UV		50000000

/* Register definitions */
#define LTC4266_REG_INTSTAT			0x00
#define LTC4266_REG_INTMASK			0x01
#define LTC4266_REG_PWREVN_COR			0x03
#define LTC4266_REG_DETEVN_COR			0x05
#define LTC4266_REG_FLTEVN_COR			0x07
#define LTC4266_REG_TSEVN_COR			0x09
#define LTC4266_REG_SUPEVN_COR			0x0B
#define LTC4266_REG_STAT(_p)			(0x0C + (_p))
#define LTC4266_REG_STATPWR			0x10
#define LTC4266_REG_OPMD			0x12
#define LTC4266_REG_DISENA			0x13 /* Disconnect detect enable */
#define LTC4266_REG_MCONF			0x17
#define LTC4266_REG_DETPB			0x18 /*PB means "push button" */
#define LTC4266_REG_PWRPB			0x19
#define LTC4266_REG_RSTPB			0x1A
#define LTC4266_REG_ID				0x1B
#define LTC4266_REG_TLIM12			0x1E
#define LTC4266_REG_TLIM34			0x1F
#define LTC4266_REG_IPLSB(_p)			(0x30 | ((_p) << 2))
#define LTC4266_REG_VPLSB(_p)			(LTC4266_REG_IPLSB(_p) + 2)
#define LTC4266_REG_HPEN			0x44
#define LTC4266_REG_HPMD(_p)			(0x46 + (5 * (_p)))
#define LTC4266_REG_ICUT_HP(_p)			(LTC4266_REG_HPMD(_p) + 1)
#define LTC4266_REG_ILIM(_p)			(LTC4266_REG_HPMD(_p) + 2)

/* Register field definitions */

/* LTC4266_REG_INTSTAT and LTC4266_REG_INTMASK */
#define LTC4266_INT_TSTART			BIT(6)
#define LTC4266_INT_TCUT			BIT(5)
#define LTC4266_INT_CLASS			BIT(4)
#define LTC4266_INT_DETECT			BIT(3)
#define LTC4266_INT_DIS				BIT(2)
#define LTC4266_INT_PWRGD			BIT(1)

/* Per-port event bits within the CoR event registers. Every per-port event
 * register splits its 8 bits into a per-port low nibble and a per-port high
 * nibble:
 * pwrevn (03h): LO = power on/off change, HI = power good change
 * detevn (05h): LO = detection complete, HI = classification complete
 * fltevn (07h): LO = tCUT overcurrent,   HI = tDIS DC disconnect
 * tsevn  (09h): LO = tSTART overcurrent, HI = tLIM current-limit timeout
 */
#define LTC4266_EVN_HI(_p)			BIT((_p) + 4)	/* ports 0-3 */
#define LTC4266_EVN_LO(_p)			BIT(_p)		/* ports 0-3 */

/* statp<n> (0Ch-0Fh) detection result, and the "Signature Good" value */
#define LTC4266_PORT_CLASS(_stat)		FIELD_GET(GENMASK(6, 4), (_stat))
#define LTC4266_PORT_DETECT(_stat)		FIELD_GET(GENMASK(2, 0), (_stat))
#define LTC4266_DETECT_GOOD			0x4

/* LTC4266_REG_STATPWR */
#define LTC4266_STATPWR_PG(_p)			BIT((_p) + 4)
#define LTC4266_STATPWR_PE(_p)			BIT(_p)

/* LTC4266_REG_OPMD
 * There are three other operation modes possible this
 * driver doesn't support and so aren't defined.  The one supported mode,
 * OPMD_SEMI, means that a port will continuously detect and classify devices,
 * but will not power the device until instructed to do so.
 */
#define LTC4266_OPMD_SEMI			2
#define LTC4266_TWO_BIT_WORD_OFFSET(_v, _p)	((_v) << ((_p) * 2))
#define LTC4266_TWO_BIT_WORD_MASK(_p)		LTC4266_TWO_BIT_WORD_OFFSET(0x03, (_p))

/* LTC4266_REG_MCONF */
#define LTC4266_MCONF_INTERRUPT_ENABLE		BIT(7)
/* Only report a detect event when the result changes, not every cycle */
#define LTC4266_MCONF_DETCHG			BIT(6)

/* LTC4266_REG_DETPB */
#define LTC4266_DETPB_CLASS_ENABLE(_p)		BIT((_p) + 4)
#define LTC4266_DETPB_DETECT_ENABLE(_p)		BIT(_p)

/* LTC4266_REG_RSTPB */
#define LTC4266_RSTPB_INTCLR			BIT(7)
#define LTC4266_RSTPB_PINCLR			BIT(6)
#define LTC4266_RSTPB_RSTALL			BIT(4)
#define LTC4266_RSTPB_RSTPORTS			GENMASK(3, 0)

/* LTC4266_REG_ID */
#define LTC4266_ID				0x64

/* LTC4266_REG_TLIM* */
#define LTC4266_TLIM_VALUE			0x01

/* Current-sense scaling, in nA per LSB, dependent on the sense resistor. */
#define LTC4266_IP_NA_PER_LSB_RSENSE_025	122070	/* 122.07 uA/LSB */
#define LTC4266_IP_NA_PER_LSB_RSENSE_050	61035	/* 61.035 uA/LSB */

/* Voltage-sense scaling: 5.835 mV == 5835 uV per LSB. */
#define LTC4266_VP_UV_PER_LSB			5835

/* LTC4266_REG_HPEN, enable "High Power" mode (Type 2, Class 4) */
#define LTC4266_HPEN(_p)			BIT(_p)

/* LTC4266_REG_HPMD */
#define LTC4266_HPMD_PONGEN			0x01

/* LTC4266_REG_ICUT_HP.
 * Set if the sense resistor specified in DT is 0.25 Ohm to have accurately
 * scaled ICUT thresholds.
 */
#define LTC4266_ICUT_RSENSE_025_OHM		BIT(7)

/* To keep the ICUT resolution at a constant 18.75 mA, for the 0.25 Ohm sense
 * case we should also set this ICUT_RANGE
 */
#define LTC4266_ICUT_RANGE			BIT(6)

/* I_CUT is programmed in a 6-bit field; each step is 18.75 mA (18750 uA). */
#define LTC4266_ICUT_STEP_UA			18750
#define LTC4266_ICUT_MASK			GENMASK(5, 0)

/* Cap I_CUT at the suggested value for a Type 2 PD at 638mA */
#define LTC4266_ICUT_MAX_MA			638
#define LTC4266_ICUT_MAX_STEPS			34

/* Recommended lim<n> settings from datasheet Table 5.
 *
 *	I_LIM (mA)	RSENSE = 0.5 Ohm	RSENSE = 0.25 Ohm
 *	425 (Type 1)	0x00			0x80
 *	850 (Type 2)	0x40			0xC0
 */
#define LTC4266_ILIM_TYPE1_RSENSE_050		0x00
#define LTC4266_ILIM_TYPE1_RSENSE_025		0x80
#define LTC4266_ILIM_TYPE2_RSENSE_050		0x40
#define LTC4266_ILIM_TYPE2_RSENSE_025		0xC0

enum {
	LTC4266_READ_CURRENT = 0,
	LTC4266_READ_VOLTAGE = 2
};

/* Map LTC4266 Classification result to PD class.  Note for a PD that has a
 * valid detect signature, but doesn't produce a classification signature is
 * still a valid PD.  The LTC4266 indicates this with 0x06 in the statp<n>
 * register and calls it "Class 0".  This is a different state than when
 * statp<n> indicates 0, which means classification isn't complete.  This maps
 * the result to either an errno or classification value suitable for use up
 * the stack.
 */
static const int ltc4266_class_map[] = {
	-EAGAIN, /* Classification is incomplete */
	1,
	2,
	3,
	4,
	-EINVAL,
	0,
	-ERANGE
};

/* Map a PD Class to I_CUT thresholds from the LTC4266 datasheet Table 2 */
static const int ltc4266_class_to_icut[] = {
	375,
	112,
	206,
	375,
	638
};

/* Per-class power budget at the PSE PI in mW, indexed by class (0-4).
 * Classes 0-3 are the P_Class values from Table 33-7 in the IEEE802.3
 * standard, which Note 1 defines as the minimum power a PSE must supply at the
 * PI for that class (the PD input power maxima are in Table 33-18). Class 4 is
 * P_Type, which Table 33-11 item 12 defines as I_Cable x V_Port_PSE min:
 * 0.600 A x 50.0 V for a Type 2 PSE, with I_Cable from Table 33-1.
 *
 * Class 0 and Class 3 have the same value because a device without a classification
 * signature (Class 0) has to be assumed to consume up to the maximum power for
 * a Type 1 PD (Class 3).
 */
static const int ltc4266_class_pw[] = {
	15400,	/* Class 0 */
	4000,	/* Class 1 */
	7000,	/* Class 2 */
	15400,	/* Class 3 */
	30000,	/* Class 4 (Type 2) */
};

enum sense_resistor {
	LTC4266_RSENSE_500, /* Rsense 0.5 Ohm */
	LTC4266_RSENSE_250 /* Rsense 0.25 Ohm */
};

struct ltc4266;

/**
 * struct ltc4266_port - per-PSE-PI context
 *
 * @ltc4266: the controller owning this port
 * @chan: index of the LTC4266 delivery channel backing this PI, established
 *	  from the PI pairset phandle by ltc4266_map_pis().  All register
 *	  addressing uses this member.
 * @rsense: sense resistor on @chan, used to scale current readings and
 *	    to pick the I_CUT and I_LIM encodings.
 * @pw_limit: Admin-configured power limit in mW, always valid: it defaults to
 *	      LTC4266_PW_LIMIT_MAX, the most this controller can deliver, until
 *	      pi_set_pw_limit() lowers it.
 * @vpwr_uv: nominal PI voltage in uV, read once from the PI's "vpwr-supply" at
 *	     probe, or LTC4266_VPORT_NOMINAL_UV when the PI does not describe
 *	     one. Reported while the port is not delivering power, when the
 *	     controller has nothing to measure.
 */
struct ltc4266_port {
	struct ltc4266 *ltc4266;
	u8 chan;
	enum sense_resistor rsense;
	int pw_limit;
	int vpwr_uv;
};

/**
 * struct ltc4266 - LTC4266 controller context
 *
 * @client: the I2C client
 * @regmap: register map of @client
 * @ports: table of PSE PI contexts, indexed by PSE PI id. A NULL entry is a PI
 *	   that is not described in the device tree and therefore has no
 *	   channel mapped.
 * @dev: the underlying device
 * @np: device node of @dev
 * @pcdev: the PSE controller registered with the PSE core
 */
struct ltc4266 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct ltc4266_port *ports[LTC4266_MAX_PORTS];
	struct device *dev;
	struct device_node *np;
	struct pse_controller_dev pcdev;
};

static struct ltc4266_port *ltc4266_pi_port(struct pse_controller_dev *pcdev,
					    int id)
{
	struct ltc4266 *ltc4266 = container_of(pcdev, struct ltc4266, pcdev);

	return ltc4266->ports[id];
}

static int ltc4266_read_iv(struct ltc4266_port *port, u8 iv)
{
	struct ltc4266 *ltc4266 = port->ltc4266;
	unsigned int lsb, msb;
	unsigned int statpwr;
	int lsb_reg;
	int result;
	u64 ivbits;

	result = regmap_read(ltc4266->regmap, LTC4266_REG_STATPWR, &statpwr);
	if (result < 0)
		return result;

	/* LTC4266 IV readings are only meaningful while the port is delivering
	 * power. When the PG (power good) bit is not set, the port is
	 * delivering nothing, so report 0 rather than an error: returning an
	 * errno here would abort pse_ethtool_get_status() and fail the whole
	 * "ethtool --show-pse" query for an otherwise perfectly readable port.
	 * Callers that must not report 0 (ltc4266_port_voltage_uv()) substitute
	 * a nominal value.
	 */
	if (!(statpwr & LTC4266_STATPWR_PG(port->chan)))
		return 0;

	/* Since iv is either 0 (to read current) or 2 to (read voltage), we
	 * can get the LSB reg by adding
	 */
	lsb_reg = LTC4266_REG_IPLSB(port->chan) + iv;

	result = regmap_read(ltc4266->regmap, lsb_reg, &lsb);
	if (result < 0)
		return result;

	result = regmap_read(ltc4266->regmap, lsb_reg + 1, &msb);
	if (result < 0)
		return result;

	ivbits = (msb << 8) | lsb;

	if (iv == LTC4266_READ_CURRENT)
		if (port->rsense == LTC4266_RSENSE_250)
			result = DIV_ROUND_CLOSEST_ULL(ivbits * LTC4266_IP_NA_PER_LSB_RSENSE_025,
						       1000);
		else
			result = DIV_ROUND_CLOSEST_ULL(ivbits * LTC4266_IP_NA_PER_LSB_RSENSE_050,
						       1000);
	else
		result = ivbits * LTC4266_VP_UV_PER_LSB;

	return result;
}

/* Voltage at the PI in uV, if the PI is currently powering a device;
 * otherwise, return the nominal voltage
 */
static int ltc4266_port_voltage_uv(struct ltc4266_port *port)
{
	int uv = ltc4266_read_iv(port, LTC4266_READ_VOLTAGE);

	if (uv < 0)
		return uv;

	return uv ? uv : port->vpwr_uv;
}

/**
 * ltc4266_port_set_ilim - Set the active current limit (ILIM) for a port
 * @port: the port to configure
 * @class: the detected PD class (0-4)
 *
 * Given the PD class, configure the active current limit for a particular
 * channel.  The LTC4266 will actively enforce this current limit using the
 * sense resistor for the channel. The values written are from the LTC4266
 * Datasheet, Table 5, and correspond to 425mA for Type I PDs, and 850mA for
 * Type 2 PDs (per the datasheet requirement for IEEE compliance).
 *
 * IEEE Std 802.3-2022, Table 33-11 specifies ILIM parameter ranges:
 * - For Type 1 PSE operation (PD Classes 0-3):
 * The minimum ILIM is 0.400A. This driver uses 425mA. This value fits
 * within typical Type 1 ILIM specifications (e.g., 0.400A min to
 * around 0.440A-0.500A max for the programmed steady-state limit).
 *
 * - For Type 2 PSE operation (typically PD Class 4):
 * The minimum ILIM is 1.14 * ICable (or ~1.05 * IPort_max from other
 * interpretations, e.g., ~0.630A to ~0.684A). This driver uses 850mA.
 * This value meets the minimum requirement and is a supported operational
 * current limit for high power modes in the LTC4266.
 *
 * The overall PSE current output must not exceed the time-dependent PSE
 * upperbound template, IPSEUT(t), described in IEEE Std 802.3-2022,
 * Equation (33-6). The programmed ILIM values (425mA/850mA) serve as the
 * long-term current limit (Ilimmin segment of IPSEUT(t)) and are well
 * within the higher short-term current allowances of that template (e.g., 1.75A).
 *
 * Returns: 0 on success or a negative errno.
 */
static int ltc4266_port_set_ilim(struct ltc4266_port *port, int class)
{
	bool rsense_250 = port->rsense == LTC4266_RSENSE_250;
	u8 ilim;

	if (class > 4 || class < 0)
		return -EINVAL;

	if (class < 4)
		ilim = rsense_250 ? LTC4266_ILIM_TYPE1_RSENSE_025 :
				    LTC4266_ILIM_TYPE1_RSENSE_050;
	else
		ilim = rsense_250 ? LTC4266_ILIM_TYPE2_RSENSE_025 :
				    LTC4266_ILIM_TYPE2_RSENSE_050;

	return regmap_write(port->ltc4266->regmap,
			    LTC4266_REG_ILIM(port->chan), ilim);
}

static int ltc4266_port_set_icut(struct ltc4266_port *port, int icut)
{
	u8 val;

	if (icut > LTC4266_ICUT_MAX_MA)
		return -ERANGE;

	val = min(DIV_ROUND_UP(icut * 1000, LTC4266_ICUT_STEP_UA),
		  LTC4266_ICUT_MAX_STEPS) & LTC4266_ICUT_MASK;

	if (port->rsense == LTC4266_RSENSE_250)
		val |= LTC4266_ICUT_RSENSE_025_OHM | LTC4266_ICUT_RANGE;

	return regmap_write(port->ltc4266->regmap,
			    LTC4266_REG_ICUT_HP(port->chan), val);
}

/**
 * ltc4266_pw_limit_to_icut - Convert an admin power limit to an I_CUT threshold
 * @port: the port to convert for
 * @max_mw: the power limit in mW
 * @class: the detected PD class (0-4)
 *
 * The LTC4266 only enforces a current threshold, so a power limit has to be
 * divided by the port voltage.
 *
 * Lowering I_CUT below the class threshold is legitimate: IEEE 802.3-2022
 * 33.2.7.10 defines P_Class as either the class power or "PSE allocated power
 * ... added to the channel power loss", and an administrative limit is that
 * allocated power.
 *
 * Raising it above the class threshold is not. Table 33-11 item 7 bounds I_CUT
 * at I_LIM, and ltc4266_port_set_ilim() picks I_LIM from the class: 425 mA for
 * Type 1, 850 mA for Type 2. An administrative limit of LTC4266_PW_LIMIT_MAX
 * asks for 600 mA at the nominal port voltage, which would exceed I_LIM on a
 * Type 1 class. Capping at the class threshold from datasheet Table 2 keeps
 * I_CUT under I_LIM for every class; the LTC4266_ICUT_MAX_MA check in
 * ltc4266_port_set_icut() only covers Type 2.
 *
 * Return: the threshold in mA, or a negative errno if the voltage read failed.
 */
static int ltc4266_pw_limit_to_icut(struct ltc4266_port *port, int max_mw,
				    int class)
{
	int uv = ltc4266_port_voltage_uv(port);
	int icut_mA;

	if (uv < 0)
		return uv;

	/* 30000 mW x 1000000 overflows an int */
	icut_mA = DIV_ROUND_UP_ULL((u64)max_mw * 1000000, uv);

	return min(ltc4266_class_to_icut[class], icut_mA);
}

static int ltc4266_port_delivering(struct ltc4266_port *port)
{
	unsigned int result;
	int ret;

	ret = regmap_read(port->ltc4266->regmap, LTC4266_REG_STATPWR, &result);
	if (ret < 0)
		return ret;

	return !!((result & LTC4266_STATPWR_PG(port->chan)) &&
		  (result & LTC4266_STATPWR_PE(port->chan)));
}

static int ltc4266_port_init(struct ltc4266_port *port)
{
	struct ltc4266 *ltc4266 = port->ltc4266;
	u8 chan = port->chan;
	u8 tlim_shift;
	u8 tlim_mask;
	u8 tlim_reg;
	int ret;

	/* Reset the port */
	ret = regmap_write(ltc4266->regmap, LTC4266_REG_RSTPB, BIT(chan));
	if (ret < 0)
		return ret;

	/* Set Semi-auto mode */
	ret = regmap_update_bits(port->ltc4266->regmap, LTC4266_REG_OPMD,
				 LTC4266_TWO_BIT_WORD_MASK(port->chan),
				 LTC4266_TWO_BIT_WORD_OFFSET(LTC4266_OPMD_SEMI,
							     port->chan));
	if (ret < 0)
		return ret;

	/* Enable high power mode on the port (for Type 2 PD support) */
	ret = regmap_update_bits(ltc4266->regmap, LTC4266_REG_HPEN,
				 LTC4266_HPEN(chan), LTC4266_HPEN(chan));
	if (ret < 0)
		return ret;

	/* Enable 2-event classification (IEEE 802.3-2022, Clause 33), which the
	 * datasheet refers to as "Ping-Pong" classification.
	 */
	ret = regmap_update_bits(ltc4266->regmap, LTC4266_REG_HPMD(chan),
				 LTC4266_HPMD_PONGEN, LTC4266_HPMD_PONGEN);
	if (ret < 0)
		return ret;

	if (chan <= 1)
		tlim_reg = LTC4266_REG_TLIM12;
	else
		tlim_reg = LTC4266_REG_TLIM34;

	/* Each tlim register packs two ports: the even port in the low nibble
	 * and the odd port in the high nibble. Shift both the value and the
	 * mask into the correct nibble.
	 */
	if (chan & BIT(0))
		tlim_shift = 4;
	else
		tlim_shift = 0;

	tlim_mask = GENMASK(3, 0) << tlim_shift;

	ret = regmap_update_bits(ltc4266->regmap, tlim_reg,
				 tlim_mask, LTC4266_TLIM_VALUE << tlim_shift);
	if (ret < 0)
		return ret;

	/* Enable disconnect detect. */
	ret = regmap_update_bits(ltc4266->regmap, LTC4266_REG_DISENA,
				 BIT(chan), BIT(chan));
	if (ret < 0)
		return ret;

	/* Enable detection (low nibble), classification (high nibble) on the port */
	ret = regmap_write(ltc4266->regmap, LTC4266_REG_DETPB,
			   LTC4266_DETPB_CLASS_ENABLE(chan) |
			   LTC4266_DETPB_DETECT_ENABLE(chan));

	if (ret < 0)
		return ret;

	dev_dbg(ltc4266->dev, "Channel %d has been initialized\n", chan);
	return 0;
}

/* Read the port's classification result and return a class 0-4 or an error if
 * the result isn't valid.
 */
static int ltc4266_port_get_class(struct ltc4266_port *port)
{
	struct ltc4266 *ltc4266 = port->ltc4266;
	unsigned int val;
	int ret;

	ret = regmap_read(ltc4266->regmap, LTC4266_REG_STAT(port->chan), &val);
	if (ret < 0) {
		dev_warn(ltc4266->dev, "Failed to read status register, err=%d\n", ret);
		return ret;
	}

	/* Can't have a valid classification result if we've not yet had a good
	 * detection result.
	 */
	if (LTC4266_PORT_DETECT(val) != LTC4266_DETECT_GOOD)
		return -EINVAL;

	ret = ltc4266_class_map[LTC4266_PORT_CLASS(val)];
	return ret;
}

/* Maximum power the classified PD is allowed. It does not depend on the port
 * being powered, but it does depend on the port having a PD attached and being
 * enabled to the point of running classification and detection cycles.
 */
static int ltc4266_port_max_pw(struct ltc4266_port *port)
{
	int class = ltc4266_port_get_class(port);

	if (class < 0)
		return class;

	return ltc4266_class_pw[class];
}

static int ltc4266_pi_get_pw_status(struct pse_controller_dev *pcdev, int id,
				    struct pse_pw_status *pw_status)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);
	int ret;

	ret = ltc4266_port_delivering(port);
	if (ret < 0)
		return ret;

	if (ret)
		pw_status->c33_pw_status = ETHTOOL_C33_PSE_PW_D_STATUS_DELIVERING;
	else
		pw_status->c33_pw_status = ETHTOOL_C33_PSE_PW_D_STATUS_SEARCHING;

	return 0;
}

/* With the static budget evaluation strategy the PSE core calls this only
 * after a PD has been classified and the power budget has been allocated.
 * Program the current limit for the classified PD, reduced if an admin power
 * limit asks for less than the class is entitled to, and apply power.
 */
static int ltc4266_pi_enable(struct pse_controller_dev *pcdev, int id)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);
	int class, icut, ret;

	class = ltc4266_port_get_class(port);
	if (class < 0)
		return class;

	ret = ltc4266_port_set_ilim(port, class);
	if (ret < 0)
		return ret;

	/* Derive the threshold from the admin limit against the class in front of
	 * us now, since a different PD may have been plugged in since the limit
	 * was set.
	 */
	icut = ltc4266_pw_limit_to_icut(port, port->pw_limit, class);
	if (icut < 0)
		return icut;

	ret = ltc4266_port_set_icut(port, icut);
	if (ret < 0)
		return ret;

	/* Apply power to the port. */
	return regmap_write(port->ltc4266->regmap, LTC4266_REG_PWRPB,
			    BIT(port->chan));
}

static int ltc4266_pi_disable(struct pse_controller_dev *pcdev, int id)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);

	/* Resetting the port (RSTPB, issued at the start of ltc4266_port_init)
	 * removes power, disables detection and classification, and clears the
	 * port's status register. Re-init the port so that detection and
	 * classification can happen again.
	 */
	return ltc4266_port_init(port);
}

static int ltc4266_pi_get_voltage(struct pse_controller_dev *pcdev, int id)
{
	return ltc4266_port_voltage_uv(ltc4266_pi_port(pcdev, id));
}

static int ltc4266_pi_get_admin_state(struct pse_controller_dev *pcdev, int id,
				      struct pse_admin_state *admin_state)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);
	unsigned int val;
	int ret;

	/* The PSE core uses this as the hardware admin state: whether power has
	 * been commanded on, not whether it has finished coming up.
	 * pse_pi_is_hw_enabled() decides from it which software-enabled PIs
	 * still need power delivery attempted, so report the power-enable
	 * nibble (peN), which the controller sets as soon as it applies power to
	 * the port. The power-good nibble only sets once OUT has pulled down to
	 * VEE, up to a tSTART later, and a port reported as not enabled for that
	 * whole ramp is one the core will allocate power domain budget for a
	 * second time. Actual delivery is reported by pi_get_pw_status().
	 */
	ret = regmap_read(port->ltc4266->regmap, LTC4266_REG_STATPWR, &val);
	if (ret < 0)
		return ret;

	if (val & LTC4266_STATPWR_PE(port->chan))
		admin_state->c33_admin_state =
			ETHTOOL_C33_PSE_ADMIN_STATE_ENABLED;
	else
		admin_state->c33_admin_state =
			ETHTOOL_C33_PSE_ADMIN_STATE_DISABLED;

	return 0;
}

static int ltc4266_pi_get_pw_class(struct pse_controller_dev *pcdev, int id)
{
	int ret = ltc4266_port_get_class(ltc4266_pi_port(pcdev, id));

	/* ltc4266_port_get_class will return either the class, or an errno.
	 * Returning an errno from this function will mean the ethtool command
	 * will abort with the error and not emit later useful information.
	 * Additionally, returning 0 to indicate no classification signature
	 * will cause the ethtool output to omit the "Power Class" attribute
	 * entirely. Since the "Power Class" is expected to be returned here,
	 * and class 0 and class 3 are equivalent in terms of power allocation,
	 * we'll return 3.
	 */

	if (ret == 0)
		ret = 3;
	if (ret < 0)
		ret = 0;
	return ret;
}

/* Get the power requested by the PD before enabling the port: its
 * classification power.
 */
static int ltc4266_pi_get_pw_req(struct pse_controller_dev *pcdev, int id)
{
	return ltc4266_port_max_pw(ltc4266_pi_port(pcdev, id));
}

static int ltc4266_pi_get_actual_pw(struct pse_controller_dev *pcdev, int id)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);
	int uA, uV;

	uA = ltc4266_read_iv(port, LTC4266_READ_CURRENT);
	if (uA < 0)
		return uA;

	uV = ltc4266_read_iv(port, LTC4266_READ_VOLTAGE);
	if (uV < 0)
		return uV;

	/* mW = uA * uV / 1000000000 */
	return DIV_ROUND_CLOSEST_ULL((u64)uA * uV, 1000000000);
}

/* The range of administrative power limits this controller supports. It does
 * not depend on the detected class: the class only bounds the I_CUT threshold
 * that ltc4266_pw_limit_to_icut() programs.
 */
static int ltc4266_pi_get_pw_limit_ranges(struct pse_controller_dev *pcdev, int id,
					  struct pse_pw_limit_ranges *pw_limit_ranges)
{
	struct ethtool_c33_pse_pw_limit_range *c33_pw_limit_ranges;

	c33_pw_limit_ranges = kzalloc_obj(*c33_pw_limit_ranges);
	if (!c33_pw_limit_ranges)
		return -ENOMEM;

	c33_pw_limit_ranges[0].min = LTC4266_PW_LIMIT_MIN;
	c33_pw_limit_ranges[0].max = LTC4266_PW_LIMIT_MAX;

	pw_limit_ranges->c33_pw_limit_ranges = c33_pw_limit_ranges;

	/* Return the number of ranges */
	return 1;
}

/* Store the administrative power limit. The limit is independent of any PD in
 * front of us, so accept it whether or not the port has classified anything;
 * program I_CUT immediately when it has, so a change on a live port takes
 * effect, and leave it to ltc4266_pi_enable() otherwise.
 */
static int ltc4266_pi_set_pw_limit(struct pse_controller_dev *pcdev,
				   int id, int max_mw)
{
	struct ltc4266_port *port = ltc4266_pi_port(pcdev, id);
	int class;
	int icut;
	int ret;

	if (max_mw < LTC4266_PW_LIMIT_MIN || max_mw > LTC4266_PW_LIMIT_MAX) {
		dev_err(port->ltc4266->dev, "power limit %d is out of range [%d, %d]\n",
			max_mw, LTC4266_PW_LIMIT_MIN, LTC4266_PW_LIMIT_MAX);
		return -ERANGE;
	}

	class = ltc4266_port_get_class(port);
	if (class >= 0) {
		icut = ltc4266_pw_limit_to_icut(port, max_mw, class);
		if (icut < 0)
			return icut;

		ret = ltc4266_port_set_icut(port, icut);
		if (ret < 0)
			return ret;
	}

	port->pw_limit = max_mw;

	return 0;
}

static int ltc4266_pi_get_pw_limit(struct pse_controller_dev *pcdev, int id)
{
	return ltc4266_pi_port(pcdev, id)->pw_limit;
}

/* Description of one delivery channel parsed out of the "channels" node. Used
 * locally only during ltc4266_setup_pi_matrix()
 */
struct ltc4266_chan_desc {
	struct device_node *np;
	enum sense_resistor rsense;
};

static int ltc4266_get_of_channels(struct ltc4266 *ltc4266,
				   struct ltc4266_chan_desc *chans)
{
	struct device_node *channels_node __free(device_node) =
		of_get_child_by_name(ltc4266->np, "channels");
	u32 chan_id, sense;
	int ret;

	if (!channels_node)
		return dev_err_probe(ltc4266->dev, -EINVAL,
				     "missing \"channels\" node\n");

	for_each_child_of_node_scoped(channels_node, chan_node) {
		if (!of_node_name_eq(chan_node, "channel"))
			continue;

		ret = of_property_read_u32(chan_node, "reg", &chan_id);
		if (ret)
			return dev_err_probe(ltc4266->dev, ret,
					     "missing reg property in node %pOF\n",
					     chan_node);

		if (chan_id >= LTC4266_MAX_PORTS)
			return dev_err_probe(ltc4266->dev, -EINVAL,
					     "channel id %u is out of range in node %pOF\n",
					     chan_id, chan_node);

		if (chans[chan_id].np)
			return dev_err_probe(ltc4266->dev, -EINVAL,
					     "channel id %u is already used, please check the reg property in node %pOF\n",
					     chan_id, chan_node);

		ret = of_property_read_u32(chan_node, "sense-resistor-micro-ohms", &sense);
		if (ret)
			return dev_err_probe(ltc4266->dev, ret,
					     "missing sense-resistor-micro-ohms property in node %pOF\n",
					     chan_node);

		if (sense == 250000)
			chans[chan_id].rsense = LTC4266_RSENSE_250;
		else if (sense == 500000)
			chans[chan_id].rsense = LTC4266_RSENSE_500;
		else
			return dev_err_probe(ltc4266->dev, -EINVAL,
					     "invalid sense resistor value %u in node %pOF\n",
					     sense, chan_node);

		chans[chan_id].np = of_node_get(chan_node);
	}

	return 0;
}

/* Find the channel a PI pairset phandle points at. */
static int ltc4266_match_channel(const struct pse_pi_pairset *pairset,
				 struct ltc4266_chan_desc *chans)
{
	int i;

	for (i = 0; i < LTC4266_MAX_PORTS; i++)
		if (pairset->np == chans[i].np)
			return i;

	return -ENODEV;
}

/*
 * Nominal voltage of the rail feeding a PSE PI, taken from its "vpwr-supply".
 * This is intended to work around the limitation on the LTC4266 where the port
 * voltage is not valid until _after_ the port is powered.  Since the PSE core
 * expects to read the port voltage when the port is not powered we need
 * something non-zero to return to the core.
 *
 * In the event the regulator isn't present, or if the regulator doesn't have a
 * nominal voltage available, we'll fall back and use 50V, which is the minimum
 * allowed for a Type 2 PSE.
 *
 * Finally, we read this regulator voltage here instead of during
 * ltc4266_pi_get_voltage to avoid deadlock.
 *
 * Return: the nominal voltage in uV, or LTC4266_VPORT_NOMINAL_UV if the PI
 * describes no supply, its supply has not registered yet, or its voltage
 * cannot be determined.
 */
static int ltc4266_pi_nominal_uv(struct ltc4266 *ltc4266, struct device_node *np)
{
	struct regulator *vpwr;
	int uv;

	vpwr = of_regulator_get_optional(ltc4266->dev, np, "vpwr");
	if (IS_ERR(vpwr)) {
		/* -ENODEV means the PI describes no vpwr-supply at all, which
		 * is the case the fallback exists for. -EPROBE_DEFER means the
		 * rail _is_ described but has not registered yet, so the
		 * fallback is wrong for it. Asking for a probe retry is not an
		 * option from here: we run from setup_pi_matrix(), and
		 * pse_controller_register() unwinds neither its notification
		 * fifo nor its pse_pi array when that fails, so every retry
		 * would leak. Warn instead so the assumed voltage is visible.
		 */
		if (PTR_ERR(vpwr) == -EPROBE_DEFER)
			dev_warn(ltc4266->dev,
				 "%pOF: vpwr-supply is not registered yet, assuming %d uV\n",
				 np, LTC4266_VPORT_NOMINAL_UV);

		return LTC4266_VPORT_NOMINAL_UV;
	}

	uv = regulator_get_voltage(vpwr);
	regulator_put(vpwr);

	if (uv <= 0)
		return LTC4266_VPORT_NOMINAL_UV;

	return uv;
}

/* Build a port context for every PSE PI described in the device tree, bound to
 * the channel its pairset references. A PI with no node is left NULL; the PSE
 * core does not register a regulator for it and so never calls back for it.
 */
static int ltc4266_map_pis(struct ltc4266 *ltc4266,
			   struct ltc4266_chan_desc *chans)
{
	struct pse_controller_dev *pcdev = &ltc4266->pcdev;
	struct ltc4266_port *port;
	int i, j, chan, uv;

	for (i = 0; i < LTC4266_MAX_PORTS; i++) {
		struct pse_pi *pi = &pcdev->pi[i];

		if (!pi->np)
			continue;

		if (!pi->pairset[0].np)
			return dev_err_probe(ltc4266->dev, -EINVAL,
					     "%pOF has no pairsets\n", pi->np);

		/* The LTC4266 delivers over a single pairset per channel, so
		 * there is no 4-pair mode to spread a PI over two channels.
		 */
		if (pi->pairset[1].np)
			return dev_err_probe(ltc4266->dev, -EOPNOTSUPP,
					     "%pOF: 4-pair PSE PIs are not supported\n",
					     pi->np);

		chan = ltc4266_match_channel(&pi->pairset[0], chans);
		if (chan < 0)
			return dev_err_probe(ltc4266->dev, chan,
					     "%pOF: pairset %pOF is not a channel of this controller\n",
					     pi->np, pi->pairset[0].np);

		for (j = 0; j < i; j++)
			if (ltc4266->ports[j] && ltc4266->ports[j]->chan == chan)
				return dev_err_probe(ltc4266->dev, -EINVAL,
						     "%pOF: channel %d is already used by %pOF\n",
						     pi->np, chan, pcdev->pi[j].np);

		uv = ltc4266_pi_nominal_uv(ltc4266, pi->np);

		port = devm_kzalloc(ltc4266->dev, sizeof(*port), GFP_KERNEL);
		if (!port)
			return -ENOMEM;

		port->ltc4266 = ltc4266;
		port->chan = chan;
		port->rsense = chans[chan].rsense;
		port->vpwr_uv = uv;
		port->pw_limit = LTC4266_PW_LIMIT_MAX;
		ltc4266->ports[i] = port;

		dev_dbg(ltc4266->dev, "PI %d is backed by channel %d, nominal %d uV\n",
			i, chan, uv);
	}

	return 0;
}

static int ltc4266_setup_pi_matrix(struct pse_controller_dev *pcdev)
{
	struct ltc4266 *ltc4266 = container_of(pcdev, struct ltc4266, pcdev);
	struct ltc4266_chan_desc chans[LTC4266_MAX_PORTS] = { };
	int i, ret;

	if (pcdev->no_of_pse_pi)
		return dev_err_probe(ltc4266->dev, -EINVAL,
				     "a \"pse-pis\" node is required\n");

	ret = ltc4266_get_of_channels(ltc4266, chans);
	if (!ret)
		ret = ltc4266_map_pis(ltc4266, chans);

	for (i = 0; i < LTC4266_MAX_PORTS; i++)
		of_node_put(chans[i].np);

	if (ret)
		return ret;

	for (i = 0; i < LTC4266_MAX_PORTS; i++) {
		if (!ltc4266->ports[i])
			continue;

		ret = ltc4266_port_init(ltc4266->ports[i]);
		if (ret < 0)
			return dev_err_probe(ltc4266->dev, ret,
					     "Failed to initialize PI %d\n", i);
	}

	return 0;
}

static const struct pse_controller_ops ltc4266_ops = {
	.setup_pi_matrix = ltc4266_setup_pi_matrix,
	.pi_get_admin_state = ltc4266_pi_get_admin_state,
	.pi_get_pw_status = ltc4266_pi_get_pw_status,
	.pi_get_pw_class = ltc4266_pi_get_pw_class,
	.pi_get_actual_pw = ltc4266_pi_get_actual_pw,
	.pi_enable = ltc4266_pi_enable,
	.pi_disable = ltc4266_pi_disable,
	.pi_get_voltage = ltc4266_pi_get_voltage,
	.pi_get_pw_limit = ltc4266_pi_get_pw_limit,
	.pi_set_pw_limit = ltc4266_pi_set_pw_limit,
	.pi_get_pw_limit_ranges = ltc4266_pi_get_pw_limit_ranges,
	.pi_get_pw_req = ltc4266_pi_get_pw_req,
};

#define LTC4266_INTERRUPT_SOURCES	(LTC4266_INT_TSTART | LTC4266_INT_TCUT | \
					 LTC4266_INT_CLASS | LTC4266_INT_DETECT | \
					 LTC4266_INT_DIS | LTC4266_INT_PWRGD)

static int ltc4266_enable_interrupts(struct ltc4266 *ltc4266)
{
	/* Unmask interrupts */
	return regmap_write(ltc4266->regmap, LTC4266_REG_INTMASK,
		     LTC4266_INTERRUPT_SOURCES);
}

static int ltc4266_disable_interrupts(struct ltc4266 *ltc4266)
{
	int ret;

	ret = regmap_write(ltc4266->regmap, LTC4266_REG_INTMASK, 0x00);
	if (ret < 0)
		return ret;

	/* Reset the (SMBus Alert) interrupt pin */
	return regmap_write(ltc4266->regmap, LTC4266_REG_RSTPB, LTC4266_RSTPB_PINCLR);
}

static int ltc4266_map_event(int irq, struct pse_controller_dev *pcdev,
			     unsigned long *notifs, unsigned long *notifs_mask)
{
	struct ltc4266 *ltc4266 = container_of(pcdev, struct ltc4266, pcdev);
	unsigned int detevn = 0, fltevn = 0, tsevn = 0, pwrevn = 0;
	unsigned int statpwr = 0;
	unsigned int intstat;
	int ret;
	int i;

	ret = ltc4266_disable_interrupts(ltc4266);
	if (ret < 0)
		goto err;

	ret = regmap_read(ltc4266->regmap, LTC4266_REG_INTSTAT, &intstat);
	if (ret < 0)
		goto err;

	if (!intstat)
		goto done;

	if (intstat & LTC4266_INT_PWRGD) {
		ret = regmap_read(ltc4266->regmap, LTC4266_REG_PWREVN_COR, &pwrevn);
		if (ret < 0) {
			dev_err(&ltc4266->client->dev, "Failed to read pwrevn, err=%d\n", ret);
			goto err;
		}

		ret = regmap_read(ltc4266->regmap, LTC4266_REG_STATPWR, &statpwr);
		if (ret < 0) {
			dev_err(&ltc4266->client->dev, "Failed to read statpwr, err=%d\n", ret);
			goto err;
		}
	}

	if (intstat & (LTC4266_INT_DIS | LTC4266_INT_TCUT)) {
		ret = regmap_read(ltc4266->regmap, LTC4266_REG_FLTEVN_COR, &fltevn);
		if (ret < 0) {
			dev_err(&ltc4266->client->dev, "Failed to read fltevn err=%d\n", ret);
			goto err;
		}
	}

	if (intstat & (LTC4266_INT_TSTART | LTC4266_INT_TCUT)) {
		ret = regmap_read(ltc4266->regmap, LTC4266_REG_TSEVN_COR, &tsevn);
		if (ret < 0) {
			dev_err(&ltc4266->client->dev, "Failed to read tsevn, err=%d\n", ret);
			goto err;
		}
	}

	if (intstat & (LTC4266_INT_CLASS | LTC4266_INT_DETECT)) {
		ret = regmap_read(ltc4266->regmap, LTC4266_REG_DETEVN_COR, &detevn);
		if (ret < 0) {
			dev_err(&ltc4266->client->dev, "Failed to read detevn, err=%d\n", ret);
			goto err;
		}
	}

	/* The event registers are indexed by delivery channel while notifs[] is
	 * indexed by PSE PI id, so walk the PIs and use each one's channel to
	 * select the event bits.
	 */
	for (i = 0; i < LTC4266_MAX_PORTS; i++) {
		struct ltc4266_port *port = ltc4266->ports[i];
		unsigned int fault;
		bool pg_lost;
		u8 chan;

		if (!port)
			continue;

		chan = port->chan;

		fault = (tsevn | fltevn) &
			(LTC4266_EVN_HI(chan) | LTC4266_EVN_LO(chan));

		/* The controller also drops a port without reporting any
		 * per-port fault event, for instance on an over-temperature
		 * shutdown or when it finds a failed external MOSFET. Both of
		 * those clear the port's detection and classification enables,
		 * so the port stays dark until it is re-initialised. A power
		 * good change that leaves statpwr[pg] clear catches them, and
		 * ignores the rising edge of a port that has just been powered
		 * on.
		 */
		pg_lost = (pwrevn & LTC4266_EVN_HI(chan)) &&
			  !(statpwr & LTC4266_STATPWR_PG(chan));

		if (fault || pg_lost) {
			/* The port has gone down; if we see this, then we don't
			 * care if any of the remaining events are set.  If the
			 * device did disconnect briefly, it'll redetect and
			 * reclassify accordingly
			 */
			notifs[i] |= ETHTOOL_C33_PSE_EVENT_DISCONNECTION;
			*notifs_mask |= BIT(i);

			/* Report over-current if the port went down for any
			 * overcurrent reason: tSTART (tsevn low nibble, startup
			 * inrush), tLIM (tsevn high nibble, current-limit
			 * timeout) or tCUT (fltevn low nibble, I_CUT timeout).
			 */
			if ((tsevn & (LTC4266_EVN_LO(chan) | LTC4266_EVN_HI(chan))) ||
			    (fltevn & LTC4266_EVN_LO(chan)))
				notifs[i] |= ETHTOOL_PSE_EVENT_OVER_CURRENT;

			dev_dbg(&ltc4266->client->dev,
				"tsevn=0x%02X fltevn=0x%02X pwrevn=0x%02X statpwr=0x%02X\n",
				tsevn, fltevn, pwrevn, statpwr);
			continue;
		}
		if (detevn & LTC4266_EVN_LO(chan)) {
			/* Read the detect result, and if it isn't detect good,
			 * call it a disconnect
			 */
			unsigned int detval;

			ret = regmap_read(ltc4266->regmap, LTC4266_REG_STAT(chan), &detval);
			if (ret < 0) {
				dev_warn(ltc4266->dev, "Failed to read status register, err=%d\n",
					 ret);
				continue;
			}

			if (LTC4266_PORT_DETECT(detval) != LTC4266_DETECT_GOOD) {
				notifs[i] |= ETHTOOL_C33_PSE_EVENT_DISCONNECTION;
				*notifs_mask |= BIT(i);
				continue;
			}
		}

		if (detevn & LTC4266_EVN_HI(chan)) {
			int class = ltc4266_port_get_class(port);

			if (class >= 0) {
				notifs[i] |= ETHTOOL_C33_PSE_EVENT_CLASSIFICATION;
				*notifs_mask |= BIT(i);
			}
		}
	}

done:
	return ltc4266_enable_interrupts(ltc4266);

err:
	/* (Attempt to) clear any remaining event registers that we might've
	 * missed in the event a previous read has failed.
	 */
	ret = regmap_write(ltc4266->regmap, LTC4266_REG_RSTPB, LTC4266_RSTPB_INTCLR);
	if (ret)
		dev_warn(&ltc4266->client->dev, "Failed to clear pending interrupts, err=%d\n",
			 ret);

	return ltc4266_enable_interrupts(ltc4266);
}

static const struct regmap_config ltc4266_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x5F,
};

static void ltc4266_teardown(void *data)
{
	struct ltc4266 *ltc4266 = data;

	ltc4266_disable_interrupts(ltc4266);

	/* Prevent the chip from asserting interrupts */
	regmap_update_bits(ltc4266->regmap, LTC4266_REG_MCONF,
			   LTC4266_MCONF_INTERRUPT_ENABLE, 0);

	/* Reset all the ports and do not re-init: a port reset removes power and
	 * clears the port's detection and classification enables, leaving it in
	 * semi-auto mode, which never powers a port without a host request.
	 */
	regmap_write(ltc4266->regmap, LTC4266_REG_RSTPB, LTC4266_RSTPB_RSTPORTS);
}

static int ltc4266_probe(struct i2c_client *client)
{
	struct ltc4266 *ltc4266;
	struct regmap *regmap;
	unsigned int id_reg;
	int ret;

	struct pse_irq_desc irq_desc = {
		.name = "ltc4266-irq",
		.map_event = ltc4266_map_event,
	};

	/* We need IRQ for static power budgeting and if don't have it, fail
	 * probe early
	 */
	if (!client->irq)
		return dev_err_probe(&client->dev, -EINVAL,
				     "Interrupt is required for power budget management\n");

	regmap = devm_regmap_init_i2c(client, &ltc4266_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "Failed to allocate regmap\n");

	/* Confirm we are talking to an LTC4266: the id register (0x1B) should
	 * read back its documented reset value of 0x64.
	 */
	ret = regmap_read(regmap, LTC4266_REG_ID, &id_reg);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "Failed to read ID register\n");

	if (id_reg != LTC4266_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "Expected an ID of 0x64, saw 0x%02X\n", id_reg);

	/* Reset the chip */
	ret = regmap_write(regmap, LTC4266_REG_RSTPB, LTC4266_RSTPB_INTCLR | LTC4266_RSTPB_RSTALL);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret, "Failed to reset\n");

	/* LTC4266 requires approximately 10 ms after reset to be stable; if it
	 * isn't, then there is typically an undervoltage lockout/something pretty bad
	 * going on. We give it 50 ms here so we don't need to poll the chip and use I2C bandwidth
	 */
	msleep(50);

	/* Let's make sure the chip came out of reset (if not, the chip is probably
	 * either (no longer?) present, in thermal shutdown, or watchdogged....either
	 * way, there's nothing we can do in software to fix it)
	 */
	ret = regmap_read(regmap, LTC4266_REG_ID, &id_reg);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to re-read ID register after reset\n");

	if (id_reg != LTC4266_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "Failed to re-read device ID after reset 0x%02X\n",
				     id_reg);

	ltc4266 = devm_kzalloc(&client->dev, sizeof(struct ltc4266), GFP_KERNEL);
	if (!ltc4266)
		return -ENOMEM;

	ltc4266->client = client;
	ltc4266->regmap = regmap;
	ltc4266->np = client->dev.of_node;
	ltc4266->dev = &client->dev;

	/* After reset, the LTC4266 will interrupt with a (single) supply fault.
	 * Clear it here and discard the result
	 */
	regmap_read(ltc4266->regmap, LTC4266_REG_SUPEVN_COR, &id_reg);

	ret = ltc4266_disable_interrupts(ltc4266);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to disable interrupts\n");

	/* Registered before the controller and the IRQ so devres ordering runs
	 * it after free_irq() and pse_controller_unregister(), and so a failed
	 * probe cannot leave ports running detection with no driver bound.
	 */
	ret = devm_add_action_or_reset(&client->dev, ltc4266_teardown, ltc4266);
	if (ret)
		return ret;

	ltc4266->pcdev.owner = THIS_MODULE;
	ltc4266->pcdev.ops = &ltc4266_ops;
	ltc4266->pcdev.dev = &client->dev;
	ltc4266->pcdev.types = ETHTOOL_PSE_C33;
	ltc4266->pcdev.nr_lines = LTC4266_MAX_PORTS;
	ltc4266->pcdev.supp_budget_eval_strategies = PSE_BUDGET_EVAL_STRAT_STATIC;

	ret = devm_pse_controller_register(ltc4266->dev, &ltc4266->pcdev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register PSE controller\n");

	/* Enable the interrupt pin, and only report detect events on
	 * change (detchg) so idle ports continuously re-running
	 * detection in semi-auto mode don't flood the host with a
	 * detect event every cycle.
	 */
	ret = regmap_update_bits(ltc4266->regmap, LTC4266_REG_MCONF,
				 LTC4266_MCONF_INTERRUPT_ENABLE | LTC4266_MCONF_DETCHG,
				 LTC4266_MCONF_INTERRUPT_ENABLE | LTC4266_MCONF_DETCHG);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to configure interrupts\n");

	ret = devm_pse_irq_helper(&ltc4266->pcdev, client->irq,
				  0, &irq_desc);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to register PSE IRQ\n");

	/* Unmask the chip interrupt sources now the handler is ready. */
	ret = ltc4266_enable_interrupts(ltc4266);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "Failed to enable interrupts\n");

	return 0;
}

static const struct i2c_device_id ltc4266_id[] = {
	{.name = "ltc4266"},
	{ }
};
MODULE_DEVICE_TABLE(i2c, ltc4266_id);

static const struct of_device_id ltc4266_of_match[] = {
	{ .compatible = "lltc,ltc4266" },
	{ }
};
MODULE_DEVICE_TABLE(of, ltc4266_of_match);

static struct i2c_driver ltc4266_driver = {
	.driver		= {
		.name	= "ltc4266",
		.of_match_table = ltc4266_of_match,
	},
	.probe		= ltc4266_probe,
	.id_table	= ltc4266_id,
};
module_i2c_driver(ltc4266_driver);

MODULE_AUTHOR("Kyle Swenson <kyle.swenson@est.tech>");
MODULE_DESCRIPTION("LTC4266 PoE PSE Controller Driver");
MODULE_LICENSE("GPL");
