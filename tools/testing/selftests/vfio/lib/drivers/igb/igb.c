// SPDX-License-Identifier: GPL-2.0-only
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <linux/io.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <libvfio/vfio_pci_device.h>

#include "registers.h"

#define PCI_DEVICE_ID_INTEL_82576 0x10C9
#define IGB_MAX_CHUNK_SIZE 1024
#define MSIX_VECTOR 0
#define RING_SIZE 4096 /* Number of descriptors in ring */

struct igb_tx_desc {
	union {
		struct {
			u64 buffer_addr; /* Address of descriptor's data buffer */
			u32 cmd_type_len; /* Command/Type/Length */
			u32 olinfo_status; /* Context/Buffer info */
		} read;

		struct {
			u64 rsvd;        /* Reserved */
			u32 nxtseq_seed; /* Next sequence seed */
			u32 status;      /* Descriptor status */
		} wb;
	};
};

struct igb_rx_desc {
	union {
		struct {
			u64 pkt_addr; /* Packet buffer address */
			u64 hdr_addr; /* Header buffer address */
		} read;
		struct {
			u16 pkt_info;     /* RSS type, Packet type */
			u16 hdr_info;     /* Split Head, buf len */
			u32 rss;          /* RSS Hash */
			u32 status_error; /* ext status/error */
			u16 length;       /* Packet length */
			u16 vlan;         /* VLAN tag */
		} wb; /* writeback */
	};
};

struct igb {
	void *bar0;
	u32 tx_tail;
	u32 rx_tail;
	struct igb_tx_desc tx_ring[RING_SIZE] __attribute__((aligned(128)));
	struct igb_rx_desc rx_ring[RING_SIZE] __attribute__((aligned(128)));
};

static inline struct igb *to_igb_state(struct vfio_pci_device *device)
{
	return (struct igb *)device->driver.region.vaddr;
}

static inline void igb_write32(struct igb *igb, u32 reg, u32 val)
{
	writel(val, igb->bar0 + reg);
}

static inline u32 igb_read32(struct igb *igb, u32 reg)
{
	return readl(igb->bar0 + reg);
}

static int igb_write_phy(struct igb *igb, u32 offset, u16 data)
{
	u32 mdic;
	int i;

	mdic = (((u32)data) |
		(offset << IGB_MDIC_REG_SHIFT) |
		(1 << IGB_MDIC_PHY_SHIFT) |
		IGB_MDIC_OP_WRITE);

	igb_write32(igb, IGB_MDIC, mdic);

	for (i = 0; i < 1000; i++) {
		usleep(50);
		mdic = igb_read32(igb, IGB_MDIC);
		if (mdic & IGB_MDIC_READY)
			break;
	}

	if (!(mdic & IGB_MDIC_READY))
		return -1;

	if (mdic & IGB_MDIC_ERROR)
		return -1;

	return 0;
}

static int igb_read_phy(struct igb *igb, u32 offset, u16 *data)
{
	u32 mdic;
	int i;

	mdic = ((offset << IGB_MDIC_REG_SHIFT) |
		(1 << IGB_MDIC_PHY_SHIFT) |
		IGB_MDIC_OP_READ);

	igb_write32(igb, IGB_MDIC, mdic);

	for (i = 0; i < 1000; i++) {
		usleep(50);
		mdic = igb_read32(igb, IGB_MDIC);
		if (mdic & IGB_MDIC_READY)
			break;
	}

	if (!(mdic & IGB_MDIC_READY))
		return -1;

	if (mdic & IGB_MDIC_ERROR)
		return -1;

	*data = (u16)mdic;
	return 0;
}

static int igb_phy_setup_autoneg(struct igb *igb)
{
	int timeout_ms = 1000;
	bool success = false;
	u16 phy_status;
	int ret;
	int i;

	/* Trigger auto-negotiation */
	ret = igb_write_phy(igb, IGB_PHY_CTRL_REG_OFFSET,
			    IGB_PHY_CTRL_AN_ENABLE | IGB_PHY_CTRL_RESTART_AN);
	if (ret)
		return ret;

	for (i = 0; i < timeout_ms; i++) {
		if (igb_read_phy(igb, IGB_PHY_STATUS_REG_OFFSET, &phy_status) == 0) {
			success = !!(phy_status & IGB_PHY_STATUS_AN_COMP);
			if (success)
				break;
		}
		usleep(1000);
	}

	if (!success) {
		printf("igb: Auto-negotiation did not complete in time\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int igb_probe(struct vfio_pci_device *device)
{
	if (!vfio_pci_device_match(device, PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82576))
		return -EINVAL;

	return 0;
}

static void igb_init(struct vfio_pci_device *device)
{
	struct igb *igb = to_igb_state(device);
	u64 iova_tx, iova_rx;
	u32 ctrl, rctl;
	u16 cmd_reg;
	int retries;

	VFIO_ASSERT_GE(device->driver.region.size, sizeof(struct igb));

	/* Set up rings and calculate IOVAs */
	igb->bar0 = device->bars[0].vaddr;

	iova_tx = to_iova(device, igb->tx_ring);
	iova_rx = to_iova(device, igb->rx_ring);

	/* Reset device and disable all interrupts */
	igb_write32(igb, IGB_CTRL, igb_read32(igb, IGB_CTRL) | IGB_CTRL_RST);
	usleep(20000);
	igb_write32(igb, IGB_IMC, 0xFFFFFFFF);

	/* Signal that the driver is loaded */
	ctrl = igb_read32(igb, IGB_CTRL_EXT);
	ctrl |= IGB_CTRL_EXT_DRV_LOAD;
	ctrl &= ~IGB_CTRL_EXT_LINK_MODE_MASK;
	igb_write32(igb, IGB_CTRL_EXT, ctrl);

	/* Enable PCI Bus Master. */
	cmd_reg = vfio_pci_config_readw(device, PCI_COMMAND);
	if (!(cmd_reg & PCI_COMMAND_MASTER)) {
		cmd_reg |= (PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);
		vfio_pci_config_writew(device, PCI_COMMAND, cmd_reg);
	}

	/* Trigger autonegotiation. This enables IGB to transmit data. */
	if (igb_phy_setup_autoneg(igb))
		return;

	/* Configure TX and RX descriptor rings */
	igb_write32(igb, IGB_TDBAL0, (u32)iova_tx);
	igb_write32(igb, IGB_TDBAH0, (u32)(iova_tx >> 32));
	igb_write32(igb, IGB_TDLEN0, RING_SIZE * sizeof(struct igb_tx_desc));
	igb_write32(igb, IGB_TDH0, 0);
	igb_write32(igb, IGB_TDT0, 0);
	igb_write32(igb, IGB_TXDCTL0, IGB_TXDCTL0_Q_EN);

	igb_write32(igb, IGB_RDBAL0, (u32)iova_rx);
	igb_write32(igb, IGB_RDBAH0, (u32)(iova_rx >> 32));
	igb_write32(igb, IGB_RDLEN0, RING_SIZE * sizeof(struct igb_rx_desc));
	igb_write32(igb, IGB_RDH0, 0);
	igb_write32(igb, IGB_RDT0, 0);
	igb_write32(igb, IGB_RXDCTL0, IGB_RXDCTL0_Q_EN);

	/* Wait for TX and RX queues to be enabled */
	retries = 2000;
	while (retries-- > 0) {
		if ((igb_read32(igb, IGB_TXDCTL0) & IGB_TXDCTL0_Q_EN) &&
		    (igb_read32(igb, IGB_RXDCTL0) & IGB_RXDCTL0_Q_EN))
			break;
		usleep(10);
	}

	/* Enable Receiver and Transmitter */
	rctl = IGB_RCTL_EN |       /* Receiver Enable */
	       IGB_RCTL_UPE |      /* Unicast Promiscuous (for dummy MAC) */
	       IGB_RCTL_LBM_MAC |  /* MAC Loopback Mode */
	       IGB_RCTL_SECRC;     /* Strip CRC (needed for memcmp) */
	igb_write32(igb, IGB_RCTL, rctl);
	igb_write32(igb, IGB_TCTL, IGB_TCTL_EN);

	/* Enable MSI-X with 1 vector for the test */
	vfio_pci_msix_enable(device, MSIX_VECTOR, 1);

	/* Enable auto-masking of interrupts to avoid storms without a real ISR */
	igb_write32(igb, IGB_GPIE, IGB_GPIE_EIAME);

	/* Enable interrupts on vector 0 */
	igb_write32(igb, IGB_EIMS, 1);

	/* Map vector 0 to interrupt cause 0 and mark it valid */
	igb_write32(igb, IGB_IVAR0, IGB_IVAR_VALID);

	/* Initialize driver state and capability limits */
	igb->tx_tail = 0;
	igb->rx_tail = 0;

	device->driver.max_memcpy_size = IGB_MAX_CHUNK_SIZE;
	device->driver.max_memcpy_count = RING_SIZE - 1;
	device->driver.msi = MSIX_VECTOR;
}


static void igb_remove(struct vfio_pci_device *device)
{
	struct igb *igb = to_igb_state(device);

	vfio_pci_msix_disable(device);
	igb_write32(igb, IGB_RCTL, 0);
	igb_write32(igb, IGB_TCTL, 0);
	igb_write32(igb, IGB_CTRL, igb_read32(igb, IGB_CTRL) | IGB_CTRL_RST);
}

static void igb_irq_disable(struct igb *igb)
{
	igb_write32(igb, IGB_EIMC, 1);
}

static void igb_irq_enable(struct igb *igb)
{
	igb_write32(igb, IGB_EIMS, 1);
}

static void igb_irq_clear(struct igb *igb)
{
	igb_read32(igb, IGB_EICR);
}

static void igb_memcpy_start(struct vfio_pci_device *device, iova_t src,
			     iova_t dst, u64 size, u64 count)
{
	struct igb *igb = to_igb_state(device);
	struct igb_rx_desc *rx;
	struct igb_tx_desc *tx;
	u32 i;

	igb_irq_disable(igb);

	for (i = 0; i < count; i++) {
		tx = &igb->tx_ring[igb->tx_tail];
		rx = &igb->rx_ring[igb->rx_tail];

		memset(tx, 0, sizeof(struct igb_tx_desc));
		memset(rx, 0, sizeof(struct igb_rx_desc));

		rx->read.pkt_addr = dst;
		tx->read.buffer_addr = src;
		tx->read.cmd_type_len = (u32)size;
		tx->read.cmd_type_len |= (u32)(IGB_TXD_CMD_EOP) << IGB_TXD_CMD_SHIFT;

		/* Set to 0 to disable offloads and avoid needing a context descriptor */
		tx->read.olinfo_status = 0;

		igb->tx_tail = (igb->tx_tail + 1) % RING_SIZE;
		igb->rx_tail = (igb->rx_tail + 1) % RING_SIZE;
	}

	igb_write32(igb, IGB_RDT0, igb->rx_tail);
	igb_write32(igb, IGB_TDT0, igb->tx_tail);
}

static int igb_memcpy_wait(struct vfio_pci_device *device)
{
	struct igb *igb = to_igb_state(device);
	struct igb_rx_desc *rx;
	u32 prev_tail;
	int retries;

	prev_tail = (igb->rx_tail + RING_SIZE - 1) % RING_SIZE;
	rx = &igb->rx_ring[prev_tail];

	retries = 100;
	while (retries-- > 0) {
		if (rx->wb.status_error & 1)
			break;
		usleep(10);
	}

	igb_irq_clear(igb);

	igb_irq_enable(igb);

	if (rx->wb.status_error & 1)
		return 0;

	return -ETIMEDOUT;
}

static void igb_send_msi(struct vfio_pci_device *device)
{
	struct igb *igb = to_igb_state(device);

	igb_write32(igb, IGB_EICS, 1);
}

const struct vfio_pci_driver_ops igb_ops = {
	.name = "igb",
	.probe = igb_probe,
	.init = igb_init,
	.remove = igb_remove,
	.memcpy_start = igb_memcpy_start,
	.memcpy_wait = igb_memcpy_wait,
	.send_msi = igb_send_msi,
};
