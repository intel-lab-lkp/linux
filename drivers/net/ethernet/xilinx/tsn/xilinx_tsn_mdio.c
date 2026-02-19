// SPDX-License-Identifier: GPL-2.0
/*
 * TSN MDIO bus driver
 */

#include "xilinx_tsn.h"

#define MAX_MDIO_FREQ		2500000 /* 2.5 MHz */
#define DEFAULT_AXI_CLK_FREQ	150000000 /* 150 MHz */

/**
 * emac_ior_read_mcr - Read MDIO Control Register
 * @emac: Pointer to TSN EMAC structure
 *
 * This function reads the MDIO Control Register (MCR) and is used
 * as a callback for polling operations.
 *
 * Return: Value of MCR register
 */
static inline u32 emac_ior_read_mcr(struct tsn_emac *emac)
{
	return emac_ior(emac, TSN_MDIO_MCR_OFFSET);
}

/**
 * tsn_mdio_wait_until_ready - Wait for MDIO interface to be ready
 * @emac: Pointer to TSN EMAC structure
 *
 * This function polls the MDIO Control Register until the READY bit
 * is set, indicating the interface is ready for a new transaction.
 *
 * Return: 0 on success, -ETIMEDOUT on timeout
 */
static int tsn_mdio_wait_until_ready(struct tsn_emac *emac)
{
	u32 val;

	return readx_poll_timeout(emac_ior_read_mcr, emac,
				  val, val & TSN_MDIO_MCR_READY,
				  1, 20000);
}

/**
 * tsn_mdio_mdc_enable - Enable MDIO MDC clock
 * @emac: Pointer to TSN EMAC structure
 *
 * This function enables the MDIO Management Data Clock (MDC) by setting
 * the appropriate bits in the MDIO Control register. Called prior to
 * read/write operations.
 */
static void tsn_mdio_mdc_enable(struct tsn_emac *emac)
{
	emac_iow(emac, TSN_MDIO_MC_OFFSET,
		 ((u32)emac->mii_clk_div | TSN_MDIO_MC_MDIOEN));
}

/**
 * tsn_mdio_mdc_disable - Disable MDIO MDC clock
 * @emac: Pointer to TSN EMAC structure
 *
 * This function disables the MDIO Management Data Clock (MDC) by clearing
 * the enable bit in the MDIO Control register. Called after read/write
 * operations to save power.
 */
static void tsn_mdio_mdc_disable(struct tsn_emac *emac)
{
	u32 mc_reg;

	mc_reg = emac_ior(emac, TSN_MDIO_MC_OFFSET);
	emac_iow(emac, TSN_MDIO_MC_OFFSET,
		 (mc_reg & ~TSN_MDIO_MC_MDIOEN));
}

/**
 * tsn_mdio_read - MDIO interface read function
 * @bus:	Pointer to mii bus structure
 * @phy_id:	Address of the PHY device
 * @reg:	PHY register to read
 *
 * Return:	The register contents on success, -ETIMEDOUT on a timeout
 *
 * Reads the contents of the requested register from the requested PHY
 * address by first writing the details into MCR register. After a while
 * the register MRD is read to obtain the PHY register content.
 */
static int tsn_mdio_read(struct mii_bus *bus, int phy_id, int reg)
{
	u32 rc;
	int ret;
	struct tsn_emac *emac = bus->priv;
	struct tsn_priv *common = emac->common;

	scoped_guard(mutex, &common->mdio_lock) {
		tsn_mdio_mdc_enable(emac);

		ret = tsn_mdio_wait_until_ready(emac);
		if (ret < 0) {
			tsn_mdio_mdc_disable(emac);
			return ret;
		}

		emac_iow(emac, TSN_MDIO_MCR_OFFSET,
			 FIELD_PREP(TSN_MDIO_MCR_PHYAD_MASK, phy_id) |
			 FIELD_PREP(TSN_MDIO_MCR_REGAD_MASK, reg) |
			 TSN_MDIO_MCR_INITIATE |
			 TSN_MDIO_MCR_OP_READ);

		ret = tsn_mdio_wait_until_ready(emac);
		if (ret < 0) {
			tsn_mdio_mdc_disable(emac);
			return ret;
		}

		rc = FIELD_GET(TSN_MDIO_MRD_MASK,
			       emac_ior(emac, TSN_MDIO_MRD_OFFSET));
		tsn_mdio_mdc_disable(emac);
	}
	dev_dbg(common->dev, "%s (phy_id=%i, reg=%x) == %x\n",
		__func__, phy_id, reg, rc);

	return rc;
}

/**
 * tsn_mdio_write - MDIO interface write function
 * @bus:	Pointer to mii bus structure
 * @phy_id:	Address of the PHY device
 * @reg:	PHY register to write to
 * @val:	Value to be written into the register
 *
 * Return:	0 on success, -ETIMEDOUT on a timeout
 *
 * Writes the value to the requested register by first writing the value
 * into MWD register. The MCR register is then appropriately setup
 * to finish the write operation.
 */
static int tsn_mdio_write(struct mii_bus *bus, int phy_id, int reg,
			  u16 val)
{
	struct tsn_emac *emac = bus->priv;
	struct tsn_priv *common = emac->common;
	int ret;

	dev_dbg(common->dev, "%s (phy_id=%i, reg=%x, val=%x)\n",
		__func__, phy_id, reg, val);
	scoped_guard(mutex, &common->mdio_lock) {
		tsn_mdio_mdc_enable(emac);

		ret = tsn_mdio_wait_until_ready(emac);
		if (ret < 0) {
			tsn_mdio_mdc_disable(emac);
			return ret;
		}

		emac_iow(emac, TSN_MDIO_MWD_OFFSET, (u32)val);
		emac_iow(emac, TSN_MDIO_MCR_OFFSET,
			 FIELD_PREP(TSN_MDIO_MCR_PHYAD_MASK, phy_id) |
			 FIELD_PREP(TSN_MDIO_MCR_REGAD_MASK, reg) |
			 TSN_MDIO_MCR_INITIATE |
			 TSN_MDIO_MCR_OP_WRITE);

		ret = tsn_mdio_wait_until_ready(emac);
		if (ret < 0) {
			tsn_mdio_mdc_disable(emac);
			return ret;
		}
		tsn_mdio_mdc_disable(emac);
	}
	return 0;
}

/**
 * tsn_mdio_enable - Configure and enable MDIO controller
 * @emac: Pointer to TSN EMAC structure
 *
 * This function calculates the appropriate clock divisor for MDIO timing
 * based on the host clock frequency, programs the divisor, and enables
 * the MDIO controller. It ensures MDIO frequency does not exceed 2.5 MHz.
 *
 * Return: 0 on success, negative error code on failure
 */
static int tsn_mdio_enable(struct tsn_emac *emac)
{
	struct tsn_priv *common = emac->common;
	u32 axi_clk_freq;
	u32 clk_div;
	int i;

	emac->mii_clk_div = 0;

	/* Pick the right clock for MDIO timing */
	axi_clk_freq = 0;
	for (i = 0; i < TSN_NUM_CLOCKS; i++) {
		const char *id = common->clks[i].id;

		if (id && !strcmp(id, "s_axi_aclk") && common->clks[i].clk) {
			axi_clk_freq = clk_get_rate(common->clks[i].clk);
			break;
		}
	}

	if (!axi_clk_freq) {
		dev_warn(common->dev,
			 "Could not get s_axi_aclk, assuming %d Hz\n",
			 DEFAULT_AXI_CLK_FREQ);
		axi_clk_freq = DEFAULT_AXI_CLK_FREQ;
	}

	/* Equation: fMDIO = fHOST / ((1 + clk_div) * 2)
	 * Must ensure fMDIO <= 2.5 MHz
	 */
	clk_div = (axi_clk_freq / (MAX_MDIO_FREQ * 2)) - 1;
	if (axi_clk_freq % (MAX_MDIO_FREQ * 2))
		clk_div++;

	emac->mii_clk_div = clk_div;

	dev_dbg(common->dev,
		"MDIO: host_clk=%u Hz, clk_div=%u\n",
		axi_clk_freq, clk_div);

	/* Program divisor and enable MDIO controller */
	dev_info(common->dev,
		 "MDIO: writing to offset=0x%x, value=0x%lx\n",
		 TSN_MDIO_MC_OFFSET,
		 (unsigned long)(emac->mii_clk_div | TSN_MDIO_MC_MDIOEN));

	/* Program divisor and enable MDIO controller */
	emac_iow(emac, TSN_MDIO_MC_OFFSET,
		 emac->mii_clk_div | TSN_MDIO_MC_MDIOEN);
	return tsn_mdio_wait_until_ready(emac);
}

/**
 * tsn_mdio_setup - Setup MDIO bus for TSN EMAC
 * @emac: Pointer to TSN EMAC structure
 * @mac_np: Device tree node for MAC
 *
 * This function initializes the MDIO bus for the TSN EMAC interface.
 * It allocates an MII bus structure, configures MDIO timing, finds
 * the MDIO device tree node, and registers the MDIO bus with the kernel.
 *
 * Return: 0 on success, negative error code on failure
 */
int tsn_mdio_setup(struct tsn_emac *emac, struct device_node *mac_np)
{
	struct tsn_priv *common = emac->common;
	struct device_node *mdio_node;
	struct mii_bus *bus;
	int ret;

	bus = mdiobus_alloc();
	if (!bus)
		return -ENOMEM;

	snprintf(bus->id, MII_BUS_ID_SIZE, "tsn-mac-%.8llx",
		 (unsigned long long)emac->regs_start);

	bus->priv = emac;
	bus->name = "Xilinx TSN Ethernet MDIO";
	bus->read = tsn_mdio_read;
	bus->write = tsn_mdio_write;
	bus->parent = common->dev;
	emac->mii_bus = bus;

	mdio_node = of_get_child_by_name(mac_np, "mdio");
	if (!mdio_node) {
		dev_err(common->dev, "MAC%d: missing 'mdio' child node\n",
			emac->emac_num);
		ret = -ENODEV;
		goto unregister;
	}
	ret = tsn_mdio_enable(emac);
	if (ret < 0)
		goto unregister;
	ret = of_mdiobus_register(bus, mdio_node);
	if (ret) {
		dev_err(common->dev, "Failed to register MDIO bus for MAC%d\n",
			emac->emac_num);
		goto unregister_mdio_enabled;
	}
	of_node_put(mdio_node);
	tsn_mdio_mdc_disable(emac);
	return 0;

unregister_mdio_enabled:
	tsn_mdio_mdc_disable(emac);
unregister:
	of_node_put(mdio_node);
	mdiobus_free(bus);
	emac->mii_bus = NULL;
	return ret;
}

/**
 * tsn_mdio_teardown - Cleanup MDIO bus for TSN EMAC
 * @emac: Pointer to TSN EMAC structure
 *
 * This function performs cleanup operations for the MDIO bus.
 * It unregisters the MDIO bus from the kernel and frees any
 * associated memory for the MII bus structure.
 */
void tsn_mdio_teardown(struct tsn_emac *emac)
{
	mdiobus_unregister(emac->mii_bus);
	mdiobus_free(emac->mii_bus);
	emac->mii_bus = NULL;
}
