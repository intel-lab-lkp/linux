// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Furong Xu <0x1207@gmail.com>
 * stmmac FPE(802.3 Qbu) handling
 */

#include "stmmac.h"
#include "stmmac_fpe.h"
#include "dwmac4.h"

static int __fpe_irq_status(void __iomem *ioaddr, struct net_device *dev)
{
	u32 value;
	int status;

	status = FPE_EVENT_UNKNOWN;

	/* Reads from the MAC_FPE_CTRL_STS register should only be performed
	 * here, since the status flags of MAC_FPE_CTRL_STS are "clear on read"
	 */
	value = readl(ioaddr);

	if (value & FPE_CTRL_STS_TRSP) {
		status |= FPE_EVENT_TRSP;
		netdev_info(dev, "FPE: Respond mPacket is transmitted\n");
	}

	if (value & FPE_CTRL_STS_TVER) {
		status |= FPE_EVENT_TVER;
		netdev_info(dev, "FPE: Verify mPacket is transmitted\n");
	}

	if (value & FPE_CTRL_STS_RRSP) {
		status |= FPE_EVENT_RRSP;
		netdev_info(dev, "FPE: Respond mPacket is received\n");
	}

	if (value & FPE_CTRL_STS_RVER) {
		status |= FPE_EVENT_RVER;
		netdev_info(dev, "FPE: Verify mPacket is received\n");
	}

	return status;
}

static void __fpe_send_mpacket(void __iomem *ioaddr, struct stmmac_fpe_cfg *cfg,
			       enum stmmac_mpacket_type type)
{
	u32 value = cfg->fpe_csr;

	if (type == MPACKET_VERIFY)
		value |= FPE_CTRL_STS_SVER;
	else if (type == MPACKET_RESPONSE)
		value |= FPE_CTRL_STS_SRSP;

	writel(value, ioaddr);
}

static void dwmac4_fpe_configure(void __iomem *ioaddr,
				 struct stmmac_fpe_cfg *cfg,
				 u32 num_txq, u32 num_rxq, bool enable)
{
	u32 value;

	if (enable) {
		cfg->fpe_csr = FPE_CTRL_STS_EFPE;
		value = readl(ioaddr + GMAC_RXQ_CTRL1);
		value &= ~GMAC_RXQCTRL_FPRQ;
		value |= (num_rxq - 1) << GMAC_RXQCTRL_FPRQ_SHIFT;
		writel(value, ioaddr + GMAC_RXQ_CTRL1);
	} else {
		cfg->fpe_csr = 0;
	}

	writel(cfg->fpe_csr, ioaddr + FPE_CTRL_STS_GMAC4_OFFSET);
}

static int dwmac4_fpe_irq_status(void __iomem *ioaddr, struct net_device *dev)
{
	return __fpe_irq_status(ioaddr + FPE_CTRL_STS_GMAC4_OFFSET, dev);
}

static void dwmac4_fpe_send_mpacket(void __iomem *ioaddr,
				    struct stmmac_fpe_cfg *cfg,
				    enum stmmac_mpacket_type type)
{
	__fpe_send_mpacket(ioaddr + FPE_CTRL_STS_GMAC4_OFFSET, cfg, type);
}

const struct stmmac_fpe_ops dwmac4_fpe_ops = {
	.configure = dwmac4_fpe_configure,
	.irq_status = dwmac4_fpe_irq_status,
	.send_mpacket = dwmac4_fpe_send_mpacket,
};
