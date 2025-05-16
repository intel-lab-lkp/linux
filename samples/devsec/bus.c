// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2024 - 2025 Intel Corporation. All rights reserved.

#include <linux/platform_device.h>
#include <linux/genalloc.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/module.h>
#include <linux/range.h>
#include <uapi/linux/pci_regs.h>
#include <linux/pci.h>

#include "../../drivers/pci/pci-bridge-emul.h"
#include "devsec.h"

#define NR_DEVSEC_BUSES 1
#define NR_DEVSEC_ROOT_PORTS 1
#define NR_PLATFORM_STREAMS 4
#define NR_PORT_STREAMS 1
#define NR_ADDR_ASSOC 1
#define NR_DEVSEC_DEVS 1

struct devsec {
	struct pci_host_bridge hb;
	struct devsec_sysdata sysdata;
	struct gen_pool *iomem_pool;
	struct resource resource[2];
	struct pci_bus *bus;
	struct device *dev;
	struct devsec_port {
		union {
			struct devsec_ide {
				u32 cap;
				u32 ctl;
				struct devsec_stream {
					u32 cap;
					u32 ctl;
					u32 status;
					u32 rid1;
					u32 rid2;
					struct devsec_addr_assoc {
						u32 assoc1;
						u32 assoc2;
						u32 assoc3;
					} assoc[NR_ADDR_ASSOC];
				} stream[NR_PORT_STREAMS];
			} ide __packed;
			char ide_regs[sizeof(struct devsec_ide)];
		};
		struct pci_bridge_emul bridge;
	} *devsec_ports[NR_DEVSEC_ROOT_PORTS];
	struct devsec_dev {
		struct devsec *devsec;
		struct range mmio_range;
		u8 __cfg[SZ_4K];
		struct devsec_dev_doe {
			int cap;
			u32 req[SZ_4K / sizeof(u32)];
			u32 rsp[SZ_4K / sizeof(u32)];
			int write, read, read_ttl;
		} doe;
		u16 ide_pos;
		union {
			struct devsec_ide ide __packed;
			char ide_regs[sizeof(struct devsec_ide)];
		};
	} *devsec_devs[NR_DEVSEC_DEVS];
};

#define devsec_base(x) ((void __force __iomem *) &(x)->__cfg[0])

static struct devsec *bus_to_devsec(struct pci_bus *bus)
{
	return container_of(bus->sysdata, struct devsec, sysdata);
}

static int devsec_dev_config_read(struct devsec *devsec, struct pci_bus *bus,
				  unsigned int devfn, int pos, int size,
				  u32 *val)
{
	struct devsec_dev *devsec_dev;
	struct devsec_dev_doe *doe;
	void __iomem *base;

	if (PCI_FUNC(devfn) != 0 ||
	    PCI_SLOT(devfn) >= ARRAY_SIZE(devsec->devsec_devs))
		return PCIBIOS_DEVICE_NOT_FOUND;

	devsec_dev = devsec->devsec_devs[PCI_SLOT(devfn)];
	base = devsec_base(devsec_dev);
	doe = &devsec_dev->doe;

	if (pos == doe->cap + PCI_DOE_READ) {
		if (doe->read_ttl > 0) {
			*val = doe->rsp[doe->read];
			dev_dbg(&bus->dev, "devfn: %#x doe read[%d]\n", devfn,
				doe->read);
		} else {
			*val = 0;
			dev_dbg(&bus->dev, "devfn: %#x doe no data\n", devfn);
		}
		return PCIBIOS_SUCCESSFUL;
	} else if (pos == doe->cap + PCI_DOE_STATUS) {
		if (doe->read_ttl > 0) {
			*val = PCI_DOE_STATUS_DATA_OBJECT_READY;
			dev_dbg(&bus->dev, "devfn: %#x object ready\n", devfn);
		} else if (doe->read_ttl < 0) {
			*val = PCI_DOE_STATUS_ERROR;
			dev_dbg(&bus->dev, "devfn: %#x error\n", devfn);
		} else {
			*val = 0;
			dev_dbg(&bus->dev, "devfn: %#x idle\n", devfn);
		}
		return PCIBIOS_SUCCESSFUL;
	} else if (pos >= devsec_dev->ide_pos &&
		   pos < devsec_dev->ide_pos + sizeof(struct devsec_ide)) {
		*val = *(u32 *) &devsec_dev->ide_regs[pos - devsec_dev->ide_pos];
		return PCIBIOS_SUCCESSFUL;
	}

	switch (size) {
	case 1:
		*val = readb(base + pos);
		break;
	case 2:
		*val = readw(base + pos);
		break;
	case 4:
		*val = readl(base + pos);
		break;
	default:
		PCI_SET_ERROR_RESPONSE(val);
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	return PCIBIOS_SUCCESSFUL;
}

static int devsec_port_config_read(struct devsec *devsec, unsigned int devfn,
				   int pos, int size, u32 *val)
{
	struct devsec_port *devsec_port;

	if (PCI_FUNC(devfn) != 0 ||
	    PCI_SLOT(devfn) >= ARRAY_SIZE(devsec->devsec_ports))
		return PCIBIOS_DEVICE_NOT_FOUND;

	devsec_port = devsec->devsec_ports[PCI_SLOT(devfn)];
	return pci_bridge_emul_conf_read(&devsec_port->bridge, pos, size, val);
}

static int devsec_pci_read(struct pci_bus *bus, unsigned int devfn, int pos,
			   int size, u32 *val)
{
	struct devsec *devsec = bus_to_devsec(bus);

	dev_vdbg(&bus->dev, "devfn: %#x pos: %#x size: %d\n", devfn, pos, size);

	if (bus == devsec->hb.bus)
		return devsec_port_config_read(devsec, devfn, pos, size, val);
	else if (bus->parent == devsec->hb.bus)
		return devsec_dev_config_read(devsec, bus, devfn, pos, size,
					      val);

	return PCIBIOS_DEVICE_NOT_FOUND;
}

#ifndef PCI_DOE_PROTOCOL_DISCOVERY
#define PCI_DOE_PROTOCOL_DISCOVERY 0
#define PCI_DOE_FEATURE_CMA 1
#endif

/* just indicate support for CMA */
static void doe_process(struct devsec_dev_doe *doe)
{
	u8 type;
	u16 vid;

	vid = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, doe->req[0]);
	type = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, doe->req[0]);

	if (vid != PCI_VENDOR_ID_PCI_SIG) {
		doe->read_ttl = -1;
		return;
	}

	if (type != PCI_DOE_PROTOCOL_DISCOVERY) {
		doe->read_ttl = -1;
		return;
	}

	doe->rsp[0] = doe->req[0];
	doe->rsp[1] = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, 3);
	doe->read_ttl = 3;
	doe->rsp[2] = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_VID,
				 PCI_VENDOR_ID_PCI_SIG) |
		      FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_PROTOCOL,
				 PCI_DOE_FEATURE_CMA) |
		      FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_RSP_3_NEXT_INDEX, 0);
}

static int devsec_dev_config_write(struct devsec *devsec, struct pci_bus *bus,
				   unsigned int devfn, int pos, int size,
				   u32 val)
{
	struct devsec_dev *devsec_dev;
	struct devsec_dev_doe *doe;
	struct devsec_ide *ide;
	void __iomem *base;

	dev_vdbg(&bus->dev, "devfn: %#x pos: %#x size: %d\n", devfn, pos, size);

	if (PCI_FUNC(devfn) != 0 ||
	    PCI_SLOT(devfn) >= ARRAY_SIZE(devsec->devsec_devs))
		return PCIBIOS_DEVICE_NOT_FOUND;

	devsec_dev = devsec->devsec_devs[PCI_SLOT(devfn)];
	base = devsec_base(devsec_dev);
	doe = &devsec_dev->doe;
	ide = &devsec_dev->ide;

	if (pos >= PCI_BASE_ADDRESS_0 && pos <= PCI_BASE_ADDRESS_5) {
		if (size != 4)
			return PCIBIOS_BAD_REGISTER_NUMBER;
		/* only one 64-bit mmio bar emulated for now */
		if (pos == PCI_BASE_ADDRESS_0)
			val &= ~lower_32_bits(range_len(&devsec_dev->mmio_range) - 1);
		else if (pos == PCI_BASE_ADDRESS_1)
			val &= ~upper_32_bits(range_len(&devsec_dev->mmio_range) - 1);
		else
			val = 0;
	} else if (pos == PCI_ROM_ADDRESS) {
		val = 0;
	} else if (pos == doe->cap + PCI_DOE_CTRL) {
		if (val & PCI_DOE_CTRL_GO) {
			dev_dbg(&bus->dev, "devfn: %#x doe go\n", devfn);
			doe_process(doe);
		}
		if (val & PCI_DOE_CTRL_ABORT) {
			dev_dbg(&bus->dev, "devfn: %#x doe abort\n", devfn);
			doe->write = 0;
			doe->read = 0;
			doe->read_ttl = 0;
		}
		return PCIBIOS_SUCCESSFUL;
	} else if (pos == doe->cap + PCI_DOE_WRITE) {
		if (doe->write < ARRAY_SIZE(doe->req))
			doe->req[doe->write++] = val;
		dev_dbg(&bus->dev, "devfn: %#x doe write[%d]\n", devfn,
			doe->write - 1);
		return PCIBIOS_SUCCESSFUL;
	} else if (pos == doe->cap + PCI_DOE_READ) {
		if (doe->read_ttl > 0) {
			doe->read_ttl--;
			doe->read++;
			dev_dbg(&bus->dev, "devfn: %#x doe ack[%d]\n", devfn,
				doe->read - 1);
		}
		return PCIBIOS_SUCCESSFUL;
	} else if (pos >= devsec_dev->ide_pos &&
		   pos < devsec_dev->ide_pos + sizeof(struct devsec_ide)) {
		u16 ide_off = pos - devsec_dev->ide_pos;

		for (int i = 0; i < NR_PORT_STREAMS; i++) {
			struct devsec_stream *stream = &ide->stream[i];

			if (ide_off != offsetof(typeof(*ide), stream[i].ctl))
				continue;

			stream->ctl = val;
			stream->status &= ~PCI_IDE_SEL_STS_STATE_MASK;
			if (val & PCI_IDE_SEL_CTL_EN)
				stream->status |= FIELD_PREP(
					PCI_IDE_SEL_STS_STATE_MASK,
					PCI_IDE_SEL_STS_STATE_SECURE);
			else
				stream->status |= FIELD_PREP(
					PCI_IDE_SEL_STS_STATE_MASK,
					PCI_IDE_SEL_STS_STATE_INSECURE);
			return PCIBIOS_SUCCESSFUL;
		}
	}

	switch (size) {
	case 1:
		writeb(val, base + pos);
		break;
	case 2:
		writew(val, base + pos);
		break;
	case 4:
		writel(val, base + pos);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}
	return PCIBIOS_SUCCESSFUL;
}

static int devsec_port_config_write(struct devsec *devsec, struct pci_bus *bus,
				    unsigned int devfn, int pos, int size,
				    u32 val)
{
	struct devsec_port *devsec_port;

	dev_vdbg(&bus->dev, "devfn: %#x pos: %#x size: %d\n", devfn, pos, size);

	if (PCI_FUNC(devfn) != 0 ||
	    PCI_SLOT(devfn) >= ARRAY_SIZE(devsec->devsec_ports))
		return PCIBIOS_DEVICE_NOT_FOUND;

	devsec_port = devsec->devsec_ports[PCI_SLOT(devfn)];
	return pci_bridge_emul_conf_write(&devsec_port->bridge, pos, size, val);
}

static int devsec_pci_write(struct pci_bus *bus, unsigned int devfn, int pos,
			    int size, u32 val)
{
	struct devsec *devsec = bus_to_devsec(bus);

	dev_vdbg(&bus->dev, "devfn: %#x pos: %#x size: %d\n", devfn, pos, size);

	if (bus == devsec->hb.bus)
		return devsec_port_config_write(devsec, bus, devfn, pos, size,
						val);
	else if (bus->parent == devsec->hb.bus)
		return devsec_dev_config_write(devsec, bus, devfn, pos, size,
					       val);
	return PCIBIOS_DEVICE_NOT_FOUND;
}

static struct pci_ops devsec_ops = {
	.read = devsec_pci_read,
	.write = devsec_pci_write,
};

static void destroy_bus(void *data)
{
	struct pci_host_bridge *hb = data;

	pci_stop_root_bus(hb->bus);
	pci_remove_root_bus(hb->bus);
}

static u32 build_ext_cap_header(u32 id, u32 ver, u32 next)
{
	return FIELD_PREP(GENMASK(15, 0), id) |
	       FIELD_PREP(GENMASK(19, 16), ver) |
	       FIELD_PREP(GENMASK(31, 20), next);
}

static void init_ide(struct devsec_ide *ide)
{
	ide->cap = PCI_IDE_CAP_SELECTIVE | PCI_IDE_CAP_IDE_KM |
		   PCI_IDE_CAP_TEE_LIMITED |
		   FIELD_PREP(PCI_IDE_CAP_SEL_NUM_MASK, NR_PORT_STREAMS - 1);

	for (int i = 0; i < NR_PORT_STREAMS; i++)
		ide->stream[i].cap =
			FIELD_PREP(PCI_IDE_SEL_CAP_ASSOC_NUM_MASK, NR_ADDR_ASSOC);
}

static void init_dev_cfg(struct devsec_dev *devsec_dev)
{
	void __iomem *base = devsec_base(devsec_dev), *cap_base;
	int pos, next;

	/* BAR space */
	writew(0x8086, base + PCI_VENDOR_ID);
	writew(0xffff, base + PCI_DEVICE_ID);
	writel(lower_32_bits(devsec_dev->mmio_range.start) |
		       PCI_BASE_ADDRESS_MEM_TYPE_64 |
		       PCI_BASE_ADDRESS_MEM_PREFETCH,
	       base + PCI_BASE_ADDRESS_0);
	writel(upper_32_bits(devsec_dev->mmio_range.start),
	       base + PCI_BASE_ADDRESS_1);

	/* Capability init */
	writeb(PCI_HEADER_TYPE_NORMAL, base + PCI_HEADER_TYPE);
	writew(PCI_STATUS_CAP_LIST, base + PCI_STATUS);
	pos = 0x40;
	writew(pos, base + PCI_CAPABILITY_LIST);

	/* PCI-E Capability */
	cap_base = base + pos;
	writeb(PCI_CAP_ID_EXP, cap_base);
	writew(PCI_EXP_TYPE_ENDPOINT, cap_base + PCI_EXP_FLAGS);
	writew(PCI_EXP_LNKSTA_CLS_2_5GB | PCI_EXP_LNKSTA_NLW_X1, cap_base + PCI_EXP_LNKSTA);
	writel(PCI_EXP_DEVCAP_FLR | PCI_EXP_DEVCAP_TEE, cap_base + PCI_EXP_DEVCAP);

	/* DOE Extended Capability */
	pos = PCI_CFG_SPACE_SIZE;
	next = pos + PCI_DOE_CAP_SIZEOF;
	cap_base = base + pos;
	devsec_dev->doe.cap = pos;
	writel(build_ext_cap_header(PCI_EXT_CAP_ID_DOE, 2, next), cap_base);

	/* IDE Extended Capability */
	pos = next;
	cap_base = base + pos;
	writel(build_ext_cap_header(PCI_EXT_CAP_ID_IDE, 1, 0), cap_base);
	devsec_dev->ide_pos = pos + 4;
	init_ide(&devsec_dev->ide);
}

#define MMIO_SIZE SZ_2M

static void destroy_devsec_dev(void *data)
{
	struct devsec_dev *devsec_dev = data;
	struct devsec *devsec = devsec_dev->devsec;

	gen_pool_free(devsec->iomem_pool, devsec_dev->mmio_range.start,
		      range_len(&devsec_dev->mmio_range));
	kfree(devsec_dev);
}

static struct devsec_dev *devsec_dev_alloc(struct devsec *devsec)
{
	struct devsec_dev *devsec_dev __free(kfree) =
		kzalloc(sizeof(*devsec_dev), GFP_KERNEL);
	struct genpool_data_align data = {
		.align = MMIO_SIZE,
	};
	u64 phys;

	if (!devsec_dev)
		return ERR_PTR(-ENOMEM);

	phys = gen_pool_alloc_algo(devsec->iomem_pool, MMIO_SIZE,
				   gen_pool_first_fit_align, &data);
	if (!phys)
		return ERR_PTR(-ENOMEM);

	*devsec_dev = (struct devsec_dev) {
		.mmio_range = {
			.start = phys,
			.end = phys + MMIO_SIZE - 1,
		},
		.devsec = devsec,
	};
	init_dev_cfg(devsec_dev);

	return_ptr(devsec_dev);
}

static int alloc_devs(struct devsec *devsec)
{
	struct device *dev = devsec->dev;

	for (int i = 0; i < ARRAY_SIZE(devsec->devsec_devs); i++) {
		struct devsec_dev *devsec_dev = devsec_dev_alloc(devsec);
		int rc;

		if (IS_ERR(devsec_dev))
			return PTR_ERR(devsec_dev);
		rc = devm_add_action_or_reset(dev, destroy_devsec_dev,
					      devsec_dev);
		if (rc)
			return rc;
		devsec->devsec_devs[i] = devsec_dev;
	}

	return 0;
}

static pci_bridge_emul_read_status_t
devsec_bridge_read_base(struct pci_bridge_emul *bridge, int pos, u32 *val)
{
	return PCI_BRIDGE_EMUL_NOT_HANDLED;
}

static pci_bridge_emul_read_status_t
devsec_bridge_read_pcie(struct pci_bridge_emul *bridge, int pos, u32 *val)
{
	return PCI_BRIDGE_EMUL_NOT_HANDLED;
}

static pci_bridge_emul_read_status_t
devsec_bridge_read_ext(struct pci_bridge_emul *bridge, int pos, u32 *val)
{
	struct devsec_port *devsec_port = bridge->data;

	/* only one extended capability, IDE... */
	if (pos == 0) {
		*val = build_ext_cap_header(PCI_EXT_CAP_ID_IDE, 1, 0);
		return PCI_BRIDGE_EMUL_HANDLED;
	}

	if (pos < 4)
		return PCI_BRIDGE_EMUL_NOT_HANDLED;

	pos -= 4;
	if (pos < sizeof(struct devsec_ide)) {
		*val = *(u32 *)(&devsec_port->ide_regs[pos]);
		return PCI_BRIDGE_EMUL_HANDLED;
	}

	return PCI_BRIDGE_EMUL_NOT_HANDLED;
}

static void devsec_bridge_write_base(struct pci_bridge_emul *bridge, int pos,
				     u32 old, u32 new, u32 mask)
{
}

static void devsec_bridge_write_pcie(struct pci_bridge_emul *bridge, int pos,
				     u32 old, u32 new, u32 mask)
{
}

static void devsec_bridge_write_ext(struct pci_bridge_emul *bridge, int pos,
				    u32 old, u32 new, u32 mask)
{
	struct devsec_port *devsec_port = bridge->data;

	if (pos < sizeof(struct devsec_ide))
		*(u32 *)(&devsec_port->ide_regs[pos]) = new;
}

static const struct pci_bridge_emul_ops devsec_bridge_ops = {
	.read_base = devsec_bridge_read_base,
	.write_base = devsec_bridge_write_base,
	.read_pcie = devsec_bridge_read_pcie,
	.write_pcie = devsec_bridge_write_pcie,
	.read_ext = devsec_bridge_read_ext,
	.write_ext = devsec_bridge_write_ext,
};

static int init_port(struct devsec_port *devsec_port)
{
	struct pci_bridge_emul *bridge = &devsec_port->bridge;

	*bridge = (struct pci_bridge_emul) {
		.conf = {
			.vendor = cpu_to_le16(0x8086),
			.device = cpu_to_le16(0x7075),
			.class_revision = cpu_to_le32(0x1),
			.pref_mem_base = cpu_to_le16(PCI_PREF_RANGE_TYPE_64),
			.pref_mem_limit = cpu_to_le16(PCI_PREF_RANGE_TYPE_64),
		},
		.pcie_conf = {
			.devcap = cpu_to_le16(PCI_EXP_DEVCAP_FLR),
			.lnksta = cpu_to_le16(PCI_EXP_LNKSTA_CLS_2_5GB),
		},
		.subsystem_vendor_id = cpu_to_le16(0x8086),
		.has_pcie = true,
		.data = devsec_port,
		.ops = &devsec_bridge_ops,
	};

	init_ide(&devsec_port->ide);

	return pci_bridge_emul_init(bridge, 0);
}

static void destroy_port(void *data)
{
	struct devsec_port *devsec_port = data;

	pci_bridge_emul_cleanup(&devsec_port->bridge);
	kfree(devsec_port);
}

static struct devsec_port *devsec_port_alloc(void)
{
	int rc;

	struct devsec_port *devsec_port __free(kfree) =
		kzalloc(sizeof(*devsec_port), GFP_KERNEL);

	if (!devsec_port)
		return ERR_PTR(-ENOMEM);

	rc = init_port(devsec_port);
	if (rc)
		return ERR_PTR(rc);

	return_ptr(devsec_port);
}

static int alloc_ports(struct devsec *devsec)
{
	struct device *dev = devsec->dev;

	for (int i = 0; i < ARRAY_SIZE(devsec->devsec_ports); i++) {
		struct devsec_port *devsec_port = devsec_port_alloc();
		int rc;

		if (IS_ERR(devsec_port))
			return PTR_ERR(devsec_port);
		rc = devm_add_action_or_reset(dev, destroy_port, devsec_port);
		if (rc)
			return rc;
		devsec->devsec_ports[i] = devsec_port;
	}

	return 0;
}

static int __init devsec_bus_probe(struct platform_device *pdev)
{
	int rc;
	struct devsec *devsec;
	u64 mmio_size = SZ_64G;
	struct devsec_sysdata *sd;
	struct pci_host_bridge *hb;
	struct device *dev = &pdev->dev;
	u64 mmio_start = iomem_resource.end + 1 - SZ_64G;

	hb = devm_pci_alloc_host_bridge(
		dev, sizeof(*devsec) - sizeof(struct pci_host_bridge));
	if (!hb)
		return -ENOMEM;

	devsec = container_of(hb, struct devsec, hb);
	devsec->dev = dev;
	devsec->iomem_pool = devm_gen_pool_create(dev, ilog2(SZ_2M),
						  NUMA_NO_NODE, "devsec iomem");
	if (!devsec->iomem_pool)
		return -ENOMEM;

	rc = gen_pool_add(devsec->iomem_pool, mmio_start, mmio_size,
			  NUMA_NO_NODE);
	if (rc)
		return rc;

	rc = alloc_ports(devsec);
	if (rc)
		return rc;

	rc = alloc_devs(devsec);
	if (rc)
		return rc;

	devsec->resource[0] = (struct resource) {
		.name = "DEVSEC BUSES",
		.start = 0,
		.end = NR_DEVSEC_BUSES + NR_DEVSEC_ROOT_PORTS - 1,
		.flags = IORESOURCE_BUS | IORESOURCE_PCI_FIXED,
	};
	pci_add_resource(&hb->windows, &devsec->resource[0]);

	devsec->resource[1] = (struct resource) {
		.name = "DEVSEC MMIO",
		.start = mmio_start,
		.end = mmio_start + mmio_size - 1,
		.flags = IORESOURCE_MEM | IORESOURCE_MEM_64,
	};
	pci_add_resource(&hb->windows, &devsec->resource[1]);

	sd = &devsec->sysdata;
	devsec_sysdata = sd;
	hb->domain_nr = pci_bus_find_emul_domain_nr(PCI_DOMAIN_NR_NOT_SET);
	if (hb->domain_nr < 0)
		return hb->domain_nr;

	/*
	 * Note, domain_nr is set in devsec_sysdata for
	 * !CONFIG_PCI_DOMAINS_GENERIC platforms
	 */
	devsec_set_domain_nr(sd, hb->domain_nr);

	hb->dev.parent = dev;
	hb->sysdata = sd;
	hb->ops = &devsec_ops;
	pci_ide_init_nr_streams(hb, NR_PLATFORM_STREAMS);

	rc = pci_scan_root_bus_bridge(hb);
	if (rc)
		return rc;

	return devm_add_action_or_reset(dev, destroy_bus, no_free_ptr(hb));
}

static struct platform_driver devsec_bus_driver = {
	.driver = {
		.name = "devsec_bus",
	},
};

static struct platform_device *devsec_bus;

static int __init devsec_bus_init(void)
{
	struct platform_device_info devsec_bus_info = {
		.name = "devsec_bus",
		.id = -1,
	};
	int rc;

	devsec_bus = platform_device_register_full(&devsec_bus_info);
	if (IS_ERR(devsec_bus))
		return PTR_ERR(devsec_bus);

	rc = platform_driver_probe(&devsec_bus_driver, devsec_bus_probe);
	if (rc)
		platform_device_unregister(devsec_bus);
	return 0;
}
module_init(devsec_bus_init);

static void __exit devsec_bus_exit(void)
{
	platform_driver_unregister(&devsec_bus_driver);
	platform_device_unregister(devsec_bus);
}
module_exit(devsec_bus_exit);

MODULE_IMPORT_NS("PCI_IDE");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Device Security Sample Infrastructure: TDISP Device Emulation");
