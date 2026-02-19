// SPDX-License-Identifier: GPL-2.0

#include "xilinx_tsn.h"

#define TSN_SW_MAX_PORTS		3
#define TSN_PORT_STATE_CTRL_OFFSET	0x0004c
#define TSN_SW_MAC_LSB_OFFSET		0x0000c
#define TSN_SW_MAC_MSB_OFFSET		0x00010

#define TSN_SW_MAC_MSB_FF_MASK_SHIFT	16

/* EP Port (Port 0) control bits */
#define EP_PORT_STATUS_CHG_BIT		BIT(0)
#define EP_PORT_STATUS_SHIFT		1
#define EP_PORT_STATUS_MASK		GENMASK(3, 1)

/* MAC1 Port (Port 1) control bits */
#define MAC1_PORT_STATUS_CHG_BIT	BIT(8)
#define MAC1_PORT_STATUS_SHIFT		9
#define MAC1_PORT_STATUS_MASK		GENMASK(11, 9)

/* MAC2 Port (Port 2) control bits */
#define MAC2_PORT_STATUS_CHG_BIT	BIT(16)
#define MAC2_PORT_STATUS_SHIFT		17
#define MAC2_PORT_STATUS_MASK		GENMASK(19, 17)

#define DELAY_OF_ONE_MILLISEC		1000
#define DELAY_OF_FIVE_MILLISEC		(5 * DELAY_OF_ONE_MILLISEC)

/**
 * enum tsn_port_state - TSN switch port STP states
 * @TSN_PORT_STATE_DISABLED: Port disabled, no traffic forwarding
 * @TSN_PORT_STATE_BLOCKING: Port blocking frames
 * @TSN_PORT_STATE_LISTENING: Port listening for BPDU frames
 * @TSN_PORT_STATE_LEARNING: Port learning MAC addresses
 * @TSN_PORT_STATE_FORWARDING: Port forwarding frames normally
 */
enum tsn_port_state {
	TSN_PORT_STATE_DISABLED = 0,
	TSN_PORT_STATE_BLOCKING,
	TSN_PORT_STATE_LISTENING,
	TSN_PORT_STATE_LEARNING,
	TSN_PORT_STATE_FORWARDING,
};

/**
 * sw_iow - Memory mapped TSN switch register write
 * @sw: Pointer to TSN switch structure
 * @off: Address offset from the base address of switch registers
 * @val: Value to be written into the switch register
 *
 * This function writes the desired value into the corresponding TSN
 * switch register.
 */
static inline void sw_iow(struct tsn_switch *sw, off_t off, u32 val)
{
	iowrite32(val, sw->regs + off);
}

/**
 * sw_ior - Memory mapped TSN switch register read
 * @sw: Pointer to TSN switch structure
 * @off: Address offset from the base address of switch registers
 *
 * This function reads a value from the corresponding TSN switch
 * register.
 *
 * Return: Value read from the switch register
 */
static inline u32 sw_ior(struct tsn_switch *sw, u32 off)
{
	return ioread32(sw->regs + off);
}

/**
 * enum switch_port - TSN switch port identifiers
 * @PORT_EP: Endpoint port (port 1)
 * @PORT_MAC1: MAC1 port (port 2)
 * @PORT_MAC2: MAC2 port (port 3)
 */
enum switch_port {
	PORT_EP = 1,
	PORT_MAC1 = 2,
	PORT_MAC2 = 3,
};

/**
 * struct port_status - Port configuration structure
 * @port_num: Port number identifier
 * @port_status: STP state for the port
 */
struct port_status {
	u8 port_num;
	u8 port_status;
};

/**
 * tsn_sw_set_port_mac - Set MAC address for a specific TSN port
 * @common: TSN common private structure
 * @ndev: Network device to update
 * @base_mac: Base MAC address (first 44 bits)
 * @port_id: Port identifier (0x01 for EP, 0x02 for EMAC0, 0x03 for EMAC1)
 * @port_name: Human-readable port name for logging
 *
 * Helper function to assign a MAC address to a port with the specified
 * port ID in the lower 4 bits while preserving the upper 44 bits from
 * the base MAC address.
 */
static void tsn_sw_set_port_mac(struct tsn_priv *common,
				struct net_device *ndev,
				const u8 *base_mac, u8 port_id,
				const char *port_name)
{
	u8 new_mac[ETH_ALEN];

	memcpy(new_mac, base_mac, ETH_ALEN);
	new_mac[5] = (base_mac[5] & 0xF0) | (port_id & 0x0F);

	eth_hw_addr_set(ndev, new_mac);
	dev_info(common->dev, "%s MAC: %pM\n", port_name, ndev->dev_addr);
}

/**
 * tsn_sw_generate_consistent_macs - Generate consistent MAC addresses for all ports
 * @common: TSN common private structure
 * @base_mac: Base MAC address to use (first 44 bits preserved)
 *
 * Generates MAC addresses for EP, EMAC0, EMAC1 based on a common prefix.
 * The last 4 bits are set to 0x1, 0x2, 0x3 respectively to distinguish ports.
 *
 * Example output:
 *   Base:  32:77:6a:ed:7a:35
 *   EP:    32:77:6a:ed:7a:31
 *   EMAC0: 32:77:6a:ed:7a:32
 *   EMAC1: 32:77:6a:ed:7a:33
 */
static void tsn_sw_generate_consistent_macs(struct tsn_priv *common,
					    const u8 *base_mac)
{
	struct net_device *emac0_ndev = (common->num_emacs >= 1 && common->emacs[0]) ?
					 common->emacs[0]->ndev : NULL;
	struct net_device *emac1_ndev = (common->num_emacs >= 2 && common->emacs[1]) ?
					 common->emacs[1]->ndev : NULL;
	struct net_device *ep_ndev = common->ep ? common->ep->ndev : NULL;

	if (ep_ndev)
		tsn_sw_set_port_mac(common, ep_ndev, base_mac, 0x01, "EP");

	if (emac0_ndev)
		tsn_sw_set_port_mac(common, emac0_ndev, base_mac, 0x02, "EMAC0");

	if (emac1_ndev)
		tsn_sw_set_port_mac(common, emac1_ndev, base_mac, 0x03, "EMAC1");
}

/**
 * tsn_sw_is_mac_invalid - Check if MAC address is invalid or zero
 * @addr: MAC address to validate
 *
 * Return: true if MAC is invalid/zero, false if valid
 */
static inline bool tsn_sw_is_mac_invalid(const u8 *addr)
{
	return !is_valid_ether_addr(addr) || is_zero_ether_addr(addr);
}

/**
 * tsn_sw_check_mac_consistency - Validate MAC prefix consistency
 * @common: TSN common private structure
 * @ep_ndev: EP network device
 * @emac0_ndev: EMAC0 network device
 * @emac1_ndev: EMAC1 network device
 * @mac_mask: Mask for comparing first 44 bits
 *
 * Checks if all active ports have matching 44-bit MAC prefixes.
 *
 * Return: true if mismatch found, false if all consistent
 */
static bool tsn_sw_check_mac_consistency(struct tsn_priv *common,
					 struct net_device *ep_ndev,
					 struct net_device *emac0_ndev,
					 struct net_device *emac1_ndev,
					 const u8 *mac_mask)
{
	bool mismatch = false;

	/* Check EP vs EMAC0 */
	if (ep_ndev && emac0_ndev &&
	    !ether_addr_equal_masked(ep_ndev->dev_addr,
				     emac0_ndev->dev_addr, mac_mask)) {
		dev_warn(common->dev,
			 "MAC prefix mismatch: EP (%pM) vs EMAC0 (%pM)\n",
			 ep_ndev->dev_addr, emac0_ndev->dev_addr);
		mismatch = true;
	}

	/* Check EP vs EMAC1 */
	if (ep_ndev && emac1_ndev &&
	    !ether_addr_equal_masked(ep_ndev->dev_addr,
				     emac1_ndev->dev_addr, mac_mask)) {
		dev_warn(common->dev,
			 "MAC prefix mismatch: EP (%pM) vs EMAC1 (%pM)\n",
			 ep_ndev->dev_addr, emac1_ndev->dev_addr);
		mismatch = true;
	}

	/* Check EMAC0 vs EMAC1 */
	if (emac0_ndev && emac1_ndev &&
	    !ether_addr_equal_masked(emac0_ndev->dev_addr,
				     emac1_ndev->dev_addr, mac_mask)) {
		dev_warn(common->dev,
			 "MAC prefix mismatch: EMAC0 (%pM) vs EMAC1 (%pM)\n",
			 emac0_ndev->dev_addr, emac1_ndev->dev_addr);
		mismatch = true;
	}

	return mismatch;
}

/**
 * tsn_sw_select_base_mac - Select best available MAC as base
 * @common: TSN common private structure
 * @ep_ndev: EP network device
 * @emac0_ndev: EMAC0 network device
 * @emac1_ndev: EMAC1 network device
 * @base_mac: Output buffer for selected base MAC
 *
 * Selects the best available MAC address to use as base for generating
 * consistent MACs. Priority: EP > EMAC0 > EMAC1 > Random.
 */
static void tsn_sw_select_base_mac(struct tsn_priv *common,
				   struct net_device *ep_ndev,
				   struct net_device *emac0_ndev,
				   struct net_device *emac1_ndev,
				   u8 *base_mac)
{
	/* Try EP first */
	if (ep_ndev && !tsn_sw_is_mac_invalid(ep_ndev->dev_addr)) {
		ether_addr_copy(base_mac, ep_ndev->dev_addr);
		dev_info(common->dev, "Using EP MAC as base: %pM\n", base_mac);
		return;
	}

	/* Try EMAC0 */
	if (emac0_ndev && !tsn_sw_is_mac_invalid(emac0_ndev->dev_addr)) {
		ether_addr_copy(base_mac, emac0_ndev->dev_addr);
		dev_info(common->dev, "Using EMAC0 MAC as base: %pM\n", base_mac);
		return;
	}

	/* Try EMAC1 */
	if (emac1_ndev && !tsn_sw_is_mac_invalid(emac1_ndev->dev_addr)) {
		ether_addr_copy(base_mac, emac1_ndev->dev_addr);
		dev_info(common->dev, "Using EMAC1 MAC as base: %pM\n", base_mac);
		return;
	}

	/* Generate random locally administered MAC */
	eth_random_addr(base_mac);
	base_mac[0] = (base_mac[0] & 0xFE) | 0x02;  /* Set local bit, clear multicast */
	dev_info(common->dev, "Generated random MAC base: %pM\n", base_mac);
}

/**
 * tsn_sw_frame_filter_config - Configure frame filtering and validate MAC addresses
 * @common: TSN common private structure
 *
 * This function validates that all TSN ports (EP, EMAC0, EMAC1) share the same
 * 44-bit MAC address prefix. If any mismatch is detected, it generates a new
 * consistent set of MAC addresses with:
 *   - Same 44-bit prefix (OUI + 12 bits)
 *   - Last 4 bits set to 0x1, 0x2, 0x3 for EP, EMAC0, EMAC1 respectively
 *
 * This ensures proper frame forwarding in the TSN switch where all ports must
 * appear to belong to the same logical bridge.
 *
 * The function uses ether_addr_equal_masked() with mask FF:FF:FF:FF:FF:F0
 * to compare only the first 44 bits of MAC addresses.
 */
static void tsn_sw_frame_filter_config(struct tsn_priv *common)
{
	struct net_device *emac0_ndev = (common->num_emacs >= 1 && common->emacs[0]) ?
					 common->emacs[0]->ndev : NULL;
	struct net_device *emac1_ndev = (common->num_emacs >= 2 && common->emacs[1]) ?
					 common->emacs[1]->ndev : NULL;
	static const u8 mac_mask[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
	struct net_device *ep_ndev = common->ep ? common->ep->ndev : NULL;
	struct tsn_switch *sw = common->sw;
	bool need_new_macs = false;
	bool mac_mismatch = false;
	u8 base_mac[ETH_ALEN];
	u32 mac_lsb, mac_msb;
	int active_ports;

	dev_info(common->dev, "Validating TSN switch MAC address consistency\n");

	/* Count active ports */
	active_ports = !!ep_ndev + !!emac0_ndev + !!emac1_ndev;
	if (active_ports < 2) {
		dev_info(common->dev, "Less than 2 ports active, skipping MAC validation\n");
		return;
	}

	/* Check for invalid/zero MAC addresses */
	if (ep_ndev && tsn_sw_is_mac_invalid(ep_ndev->dev_addr)) {
		dev_info(common->dev, "EP has invalid/zero MAC: %pM\n",
			 ep_ndev->dev_addr);
		need_new_macs = true;
	}

	if (emac0_ndev && tsn_sw_is_mac_invalid(emac0_ndev->dev_addr)) {
		dev_info(common->dev, "EMAC0 has invalid/zero MAC: %pM\n",
			 emac0_ndev->dev_addr);
		need_new_macs = true;
	}

	if (emac1_ndev && tsn_sw_is_mac_invalid(emac1_ndev->dev_addr)) {
		dev_info(common->dev, "EMAC1 has invalid/zero MAC: %pM\n",
			 emac1_ndev->dev_addr);
		need_new_macs = true;
	}

	/* If all MACs are valid, check consistency */
	if (!need_new_macs)
		mac_mismatch = tsn_sw_check_mac_consistency(common, ep_ndev,
							    emac0_ndev, emac1_ndev,
							    mac_mask);

	/* Generate consistent MACs if needed */
	if (need_new_macs || mac_mismatch) {
		dev_info(common->dev, "Generating consistent MAC addresses\n");

		tsn_sw_select_base_mac(common, ep_ndev, emac0_ndev,
				       emac1_ndev, base_mac);

		tsn_sw_generate_consistent_macs(common, base_mac);

		dev_info(common->dev, "MAC address synchronization completed\n");
	} else {
		dev_info(common->dev, "All MAC addresses have consistent 44-bit prefix\n");
	}

	/* Program switch frame filter registers.
	 * Use EMAC0 MAC address as the base for the switch filter.
	 * Network port MAC addresses must differ in last LSB nibble only.
	 * This is a hardware pre-requisite - we program the MAC per port basis.
	 */

	ether_addr_copy(base_mac, emac0_ndev->dev_addr);

	/* Program lower 32 bits (bytes 2-5) into TSN_MAC_LSB register */
	mac_lsb = ((u32)base_mac[2] << 24) | ((u32)base_mac[3] << 16) |
		  ((u32)base_mac[4] << 8)  | ((u32)base_mac[5]);
	sw_iow(sw, TSN_SW_MAC_LSB_OFFSET, mac_lsb);

	/* Program upper 16 bits (bytes 0-1) and 4-bit filter mask (0xF)
	 * into TSN_MAC_MSB register
	 */
	mac_msb = (0xF << TSN_SW_MAC_MSB_FF_MASK_SHIFT) |
		  ((u32)base_mac[0] << 8) | ((u32)base_mac[1]);
	sw_iow(sw, TSN_SW_MAC_MSB_OFFSET, mac_msb);

	dev_info(common->dev, "Switch frame filter programmed with MAC: %pM (LSB=0x%08x, MSB=0x%08x)\n",
		 base_mac, mac_lsb, mac_msb);
}

/**
 * tsn_switch_set_state - Set hardware port state for a TSN switch
 * @sw: Pointer to TSN switch structure
 * @port: Pointer to port status structure containing port number and desired state
 *
 * This function programs the desired state of a TSN switch port by writing
 * to the port state control register. It supports all switch ports
 * (endpoint, MAC1, MAC2) and updates the corresponding port state bits.
 * After writing, it waits for the hardware to acknowledge the state change.
 *
 * Return: 0 on success, -ETIMEDOUT if the hardware does not acknowledge
 *         the change within the timeout period.
 */
static int tsn_switch_set_state(struct tsn_switch *sw,
				struct port_status *port)
{
	u32 en_port_sts_chg_bit = 1;
	u32 u_value, reg, err;

	u_value = sw_ior(sw, TSN_PORT_STATE_CTRL_OFFSET);
	switch (port->port_num) {
	case PORT_EP:
		if (!(u_value & EP_PORT_STATUS_CHG_BIT)) {
			u_value &= ~EP_PORT_STATUS_MASK;
			u_value |= FIELD_PREP(EP_PORT_STATUS_MASK, port->port_status);
			en_port_sts_chg_bit = EP_PORT_STATUS_CHG_BIT;
		}
		break;
	case PORT_MAC1:
		if (!(u_value & MAC1_PORT_STATUS_CHG_BIT)) {
			u_value &= ~MAC1_PORT_STATUS_MASK;
			u_value |= FIELD_PREP(MAC1_PORT_STATUS_MASK, port->port_status);
			en_port_sts_chg_bit = MAC1_PORT_STATUS_CHG_BIT;
		}
		break;
	case PORT_MAC2:
		if (!(u_value & MAC2_PORT_STATUS_CHG_BIT)) {
			u_value &= ~MAC2_PORT_STATUS_MASK;
			u_value |= FIELD_PREP(MAC2_PORT_STATUS_MASK, port->port_status);
			en_port_sts_chg_bit = MAC2_PORT_STATUS_CHG_BIT;
		}
		break;
	}

	u_value |= en_port_sts_chg_bit;
	sw_iow(sw, TSN_PORT_STATE_CTRL_OFFSET, u_value);

	/* wait for write to complete */
	err = readl_poll_timeout(sw->regs + TSN_PORT_STATE_CTRL_OFFSET, reg,
				 (!(reg & en_port_sts_chg_bit)), 10,
				 DELAY_OF_FIVE_MILLISEC);
	if (err) {
		pr_err("CAM write timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * tsn_sw_configure_forwarding - Initialize switch forwarding behavior
 * @sw: TSN switch instance pointer
 *
 * Establishes basic packet forwarding configuration across all switch
 * ports by setting each port to the forwarding state. This enables
 * traffic flow between endpoint and MAC interfaces through the switch
 * fabric.
 */
static void tsn_sw_configure_forwarding(struct tsn_switch *sw)
{
	struct port_status port;
	int ret;

	pr_info("Configuring TSN switch for basic forwarding\n");

	/* Configure PORT_EP to forwarding state */
	port.port_num = PORT_EP;
	port.port_status = TSN_PORT_STATE_FORWARDING;
	ret = tsn_switch_set_state(sw, &port);
	if (ret)
		pr_err("Failed to set PORT_EP state: %d\n", ret);
	else
		pr_info("PORT_EP configured for forwarding\n");

	/* Configure PORT_MAC1 to forwarding state */
	port.port_num = PORT_MAC1;
	port.port_status = TSN_PORT_STATE_FORWARDING;
	ret = tsn_switch_set_state(sw, &port);
	if (ret)
		pr_err("Failed to set PORT_MAC1 state: %d\n", ret);
	else
		pr_info("PORT_MAC1 configured for forwarding\n");

	/* Configure PORT_MAC2 to forwarding state */
	port.port_num = PORT_MAC2;
	port.port_status = TSN_PORT_STATE_FORWARDING;
	ret = tsn_switch_set_state(sw, &port);
	if (ret)
		pr_err("Failed to set PORT_MAC2 state: %d\n", ret);
	else
		pr_info("PORT_MAC2 configured for forwarding\n");

	pr_info("TSN switch forwarding configuration completed\n");
}

/**
 * tsn_switch_init - Initialize TSN switching subsystem
 * @pdev: Platform device for the TSN controller
 *
 * Sets up the TSN switch component by parsing device tree configuration,
 * mapping register regions, allocating switch data structures, and
 * configuring initial forwarding behavior for all ports.
 *
 * Return: 0 on successful initialization, negative error code otherwise
 */
int tsn_switch_init(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device_node *switch_node;
	struct device *dev = &pdev->dev;
	struct tsn_switch *sw;
	struct resource res;
	u32 ret;

	switch_node = of_get_child_by_name(dev->of_node, "switch");
	if (!switch_node)
		return dev_err_probe(dev, -ENODEV, "missing switch node\n");

	ret = of_address_to_resource(switch_node, 0, &res);
	if (ret) {
		of_node_put(switch_node);
		return dev_err_probe(dev, ret, "failed to get switch resource\n");
	}

	sw = devm_kzalloc(&pdev->dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;

	sw->dev = &pdev->dev;
	sw->regs = common->regs + res.start;
	common->sw = sw;

	/* Configure default forwarding */
	tsn_sw_configure_forwarding(sw);

	tsn_sw_frame_filter_config(common);

	return 0;
}

/**
 * tsn_switch_exit - Cleanup TSN switching subsystem
 * @pdev: Platform device for the TSN controller
 *
 * Performs shutdown sequence for the TSN switch by disabling all port
 * forwarding states and cleaning up allocated resources. This ensures
 * proper isolation of switch ports during driver removal.
 */
void tsn_switch_exit(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct tsn_switch *sw = common->sw;
	struct port_status port;

	if (!sw)
		return;

	port.port_status = TSN_PORT_STATE_DISABLED;

	port.port_num = PORT_EP;
	tsn_switch_set_state(sw, &port);

	port.port_num = PORT_MAC1;
	tsn_switch_set_state(sw, &port);

	port.port_num = PORT_MAC2;
	tsn_switch_set_state(sw, &port);

	pr_info("TSN switch exited and all ports set to disabled\n");
}
