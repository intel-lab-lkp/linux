// SPDX-License-Identifier: GPL-2.0
/*
 * Ethernet MAC driver for the J-Core family of SoCs.
 * Based on SEI MAC driver by Oleksandr G Zhadan / Smart Energy Instruments Inc.
 * Copyright (c) 2025 Artur Rojek <contact@artur-rojek.eu>
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define	JCORE_EMAC_CONTROL		0x0
#define	JCORE_EMAC_TX_LEN		0x4
#define	JCORE_EMAC_MACL			0x8
#define	JCORE_EMAC_MACH			0xc
#define	JCORE_EMAC_MCAST_MASK(n)	(0x60 + ((n) * 4))
#define	JCORE_EMAC_RX_BUF		0x1000
#define	JCORE_EMAC_TX_BUF		0x1800

#define	JCORE_EMAC_ENABLE_RX		BIT(1)
#define	JCORE_EMAC_BUSY			BIT(2)
#define	JCORE_EMAC_MCAST		BIT(3)
#define	JCORE_EMAC_READ			BIT(4)
#define	JCORE_EMAC_ENABLE_INT_RX	BIT(5)
#define	JCORE_EMAC_ENABLE_INT_TX	BIT(6)
#define	JCORE_EMAC_PROMISC		BIT(7)
#define	JCORE_EMAC_COMPLETE		BIT(8)
#define	JCORE_EMAC_CRC_ERR		BIT(9)
#define	JCORE_EMAC_PKT_LEN		GENMASK(26, 16)

#define	JCORE_EMAC_RX_BUFFERS		4
#define	JCORE_EMAC_TX_TIMEOUT		(2 * USEC_PER_SEC)
#define	JCORE_EMAC_MCAST_ADDRS		4

struct jcore_emac {
	void __iomem *base;
	struct regmap *map;
	struct net_device *ndev;
	struct {
		struct u64_stats_sync syncp;
		u64 rx_packets;
		u64 tx_packets;
		u64 rx_bytes;
		u64 tx_bytes;
		u64 rx_dropped;
		u64 rx_errors;
		u64 rx_crc_errors;
	} stats;
};

static irqreturn_t jcore_emac_irq(int irq, void *data)
{
	struct jcore_emac *priv = data;
	struct net_device *ndev = priv->ndev;
	struct sk_buff *skb;
	struct {
		int packets;
		int bytes;
		int dropped;
		int crc_errors;
	} stats = {};
	unsigned int status, pkt_len, i;

	for (i = 0; i < JCORE_EMAC_RX_BUFFERS; i++) {
		regmap_read(priv->map, JCORE_EMAC_CONTROL, &status);

		if (!(status & JCORE_EMAC_COMPLETE))
			break;

		/* Handle the next RX ping-pong buffer. */
		if (status & JCORE_EMAC_CRC_ERR) {
			stats.dropped++;
			stats.crc_errors++;
			goto next;
		}

		skb = netdev_alloc_skb_ip_align(ndev, ndev->mtu +
						ETH_HLEN + ETH_FCS_LEN);
		if (!skb) {
			stats.dropped++;
			goto next;
		}

		pkt_len = FIELD_GET(JCORE_EMAC_PKT_LEN, status);
		skb_put(skb, pkt_len);

		memcpy_fromio(skb->data, priv->base + JCORE_EMAC_RX_BUF,
			      pkt_len);
		skb->dev = ndev;
		skb->protocol = eth_type_trans(skb, ndev);

		stats.packets++;
		stats.bytes += pkt_len;

		netif_rx(skb);

next:
		regmap_set_bits(priv->map, JCORE_EMAC_CONTROL, JCORE_EMAC_READ);
	}

	u64_stats_update_begin(&priv->stats.syncp);
	priv->stats.rx_packets += stats.packets;
	priv->stats.rx_bytes += stats.bytes;
	priv->stats.rx_dropped += stats.dropped;
	priv->stats.rx_crc_errors += stats.crc_errors;
	priv->stats.rx_errors += stats.crc_errors;
	u64_stats_update_end(&priv->stats.syncp);

	return IRQ_HANDLED;
}

static int jcore_emac_wait(struct jcore_emac *priv)
{
	unsigned int val;

	return regmap_read_poll_timeout(priv->map, JCORE_EMAC_CONTROL, val,
					!(val & JCORE_EMAC_BUSY),
					100, JCORE_EMAC_TX_TIMEOUT);
}

static void jcore_emac_reset(struct jcore_emac *priv)
{
	regmap_write(priv->map, JCORE_EMAC_CONTROL, 0);
	usleep_range(10, 20);
}

static int jcore_emac_open(struct net_device *ndev)
{
	struct jcore_emac *priv = netdev_priv(ndev);

	if (jcore_emac_wait(priv))
		return -ETIMEDOUT;

	jcore_emac_reset(priv);
	regmap_set_bits(priv->map, JCORE_EMAC_CONTROL,
			JCORE_EMAC_ENABLE_RX | JCORE_EMAC_ENABLE_INT_RX |
			JCORE_EMAC_READ);
	regmap_clear_bits(priv->map, JCORE_EMAC_CONTROL, JCORE_EMAC_BUSY);

	netif_start_queue(ndev);
	netif_carrier_on(ndev);

	return 0;
}

static int jcore_emac_close(struct net_device *ndev)
{
	struct jcore_emac *priv = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);

	if (jcore_emac_wait(priv))
		return -ETIMEDOUT;

	jcore_emac_reset(priv);

	return 0;
}

static int jcore_emac_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct jcore_emac *priv = netdev_priv(ndev);
	unsigned int tx_len;

	if (jcore_emac_wait(priv))
		return NETDEV_TX_BUSY;

	memcpy_toio(priv->base + JCORE_EMAC_TX_BUF, skb->data, skb->len);

	tx_len = max(skb->len, 60);
	regmap_write(priv->map, JCORE_EMAC_TX_LEN, tx_len);
	regmap_set_bits(priv->map, JCORE_EMAC_CONTROL, JCORE_EMAC_BUSY);

	u64_stats_update_begin(&priv->stats.syncp);
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += skb->len;
	u64_stats_update_end(&priv->stats.syncp);

	consume_skb(skb);

	return NETDEV_TX_OK;
}

static void jcore_emac_set_rx_mode(struct net_device *ndev)
{
	struct jcore_emac *priv = netdev_priv(ndev);
	struct netdev_hw_addr *ha;
	unsigned int reg, i, idx = 0, set_mask = 0, clear_mask = 0, addr = 0;

	if (ndev->flags & IFF_PROMISC)
		set_mask |= JCORE_EMAC_PROMISC;
	else
		clear_mask |= JCORE_EMAC_PROMISC;

	if (ndev->flags & IFF_ALLMULTI)
		set_mask |= JCORE_EMAC_MCAST;
	else
		clear_mask |= JCORE_EMAC_MCAST;

	regmap_update_bits(priv->map, JCORE_EMAC_CONTROL, set_mask | clear_mask,
			   set_mask);

	if (!(ndev->flags & IFF_MULTICAST))
		return;

	netdev_for_each_mc_addr(ha, ndev) {
		/* Only the first 3 octets are used in a hardware mcast mask. */
		memcpy(&addr, ha->addr, 3);

		for (i = 0; i < idx; i++) {
			regmap_read(priv->map, JCORE_EMAC_MCAST_MASK(i), &reg);
			if (reg == addr)
				goto next_ha;
		}

		regmap_write(priv->map, JCORE_EMAC_MCAST_MASK(idx), addr);
		if (++idx >= JCORE_EMAC_MCAST_ADDRS) {
			netdev_warn(ndev, "Multicast list limit reached\n");
			break;
		}
next_ha:
	}

	/* Clear the remaining mask entries. */
	for (i = idx; i < JCORE_EMAC_MCAST_ADDRS; i++)
		regmap_write(priv->map, JCORE_EMAC_MCAST_MASK(i), 0);
}

static void jcore_emac_read_hw_addr(struct jcore_emac *priv, u8 *addr)
{
	unsigned int val;

	regmap_read(priv->map, JCORE_EMAC_MACL, &val);
	addr[5] = val;
	addr[4] = val >> 8;
	addr[3] = val >> 16;
	addr[2] = val >> 24;
	regmap_read(priv->map, JCORE_EMAC_MACH, &val);
	addr[1] = val;
	addr[0] = val >> 8;
}

static void jcore_emac_write_hw_addr(struct jcore_emac *priv, u8 *addr)
{
	unsigned int val;

	val = addr[0] << 8 | addr[1];
	regmap_write(priv->map, JCORE_EMAC_MACH, val);

	val = addr[2] << 24 | addr[3] << 16 | addr[4] << 8 | addr[5];
	regmap_write(priv->map, JCORE_EMAC_MACL, val);
}

static int jcore_emac_set_mac_address(struct net_device *ndev, void *addr)
{
	struct jcore_emac *priv = netdev_priv(ndev);
	struct sockaddr *sa = addr;
	int ret;

	ret = eth_prepare_mac_addr_change(ndev, addr);
	if (ret)
		return ret;

	jcore_emac_write_hw_addr(priv, sa->sa_data);
	eth_hw_addr_set(ndev, sa->sa_data);

	return 0;
}

static void jcore_emac_get_stats64(struct net_device *ndev,
				   struct rtnl_link_stats64 *stats)
{
	struct jcore_emac *priv = netdev_priv(ndev);
	unsigned int start;

	do {
		start = u64_stats_fetch_begin(&priv->stats.syncp);
		stats->rx_packets = priv->stats.rx_packets;
		stats->tx_packets = priv->stats.tx_packets;
		stats->rx_bytes = priv->stats.rx_bytes;
		stats->tx_bytes = priv->stats.tx_bytes;
		stats->rx_dropped = priv->stats.rx_dropped;
		stats->rx_errors = priv->stats.rx_errors;
		stats->rx_crc_errors = priv->stats.rx_crc_errors;
	} while (u64_stats_fetch_retry(&priv->stats.syncp, start));
}

static const struct net_device_ops jcore_emac_netdev_ops = {
	.ndo_open = jcore_emac_open,
	.ndo_stop = jcore_emac_close,
	.ndo_start_xmit = jcore_emac_start_xmit,
	.ndo_set_rx_mode = jcore_emac_set_rx_mode,
	.ndo_set_mac_address = jcore_emac_set_mac_address,
	.ndo_get_stats64 = jcore_emac_get_stats64,
};

static const struct regmap_config jcore_emac_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = JCORE_EMAC_MCAST_MASK(3),
	.fast_io = true, /* Force spinlock for JCORE_EMAC_CONTROL ISR access. */
};

static int jcore_emac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jcore_emac *priv;
	struct net_device *ndev;
	u8 mac[ETH_ALEN];
	unsigned int i;
	int irq, ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);

	priv = netdev_priv(ndev);
	priv->ndev = ndev;

	priv->base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->map = devm_regmap_init_mmio(dev, priv->base,
					  &jcore_emac_regmap_cfg);
	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	platform_set_drvdata(pdev, ndev);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, jcore_emac_irq, 0, dev_name(dev),
			       priv);
	if (ret < 0)
		return ret;

	ndev->watchdog_timeo = usecs_to_jiffies(JCORE_EMAC_TX_TIMEOUT);
	ndev->netdev_ops = &jcore_emac_netdev_ops;

	/* Put hardware into a known state. */
	jcore_emac_reset(priv);
	for (i = 0; i < JCORE_EMAC_MCAST_ADDRS; i++)
		regmap_write(priv->map, JCORE_EMAC_MCAST_MASK(i), 0);

	jcore_emac_read_hw_addr(priv, mac);
	if (is_zero_ether_addr(mac)) {
		eth_random_addr(mac);
		jcore_emac_write_hw_addr(priv, mac);
	}
	eth_hw_addr_set(ndev, mac);

	ret = devm_register_netdev(dev, ndev);
	if (ret)
		return dev_err_probe(dev, ret, "Unable to register netdev\n");

	return 0;
}

static const struct of_device_id jcore_emac_of_match[] = {
	{ .compatible = "jcore,emac", },
	{}
};
MODULE_DEVICE_TABLE(of, jcore_emac_of_match);

static struct platform_driver jcore_emac_driver = {
	.driver = {
		.name = "jcore-emac",
		.of_match_table = jcore_emac_of_match,
	},
	.probe = jcore_emac_probe,
};

module_platform_driver(jcore_emac_driver);
MODULE_DESCRIPTION("Ethernet MAC driver for the J-Core family of SoCs");
MODULE_AUTHOR("Artur Rojek <contact@artur-rojek.eu>");
MODULE_LICENSE("GPL");
