// SPDX-License-Identifier: GPL-2.0
/*
 * OTP Memory controller
 *
 * Copyright (C) 2022 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Claudiu Beznea <claudiu.beznea@microchip.com>
 */

#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define MCHP_OTPC_CR			(0x0)
#define MCHP_OTPC_CR_READ		BIT(6)
#define MCHP_OTPC_MR			(0x4)
#define MCHP_OTPC_MR_ADDR		GENMASK(31, 16)
#define MCHP_OTPC_MR_EMUL		BIT(7)
#define MCHP_OTPC_AR			(0x8)
#define MCHP_OTPC_SR			(0xc)
#define MCHP_OTPC_SR_READ		BIT(6)
#define MCHP_OTPC_HR			(0x20)
#define MCHP_OTPC_HR_SIZE		GENMASK(15, 8)
#define MCHP_OTPC_HR_PACKET_TYPE	GENMASK(2, 0)
#define MCHP_OTPC_DR			(0x24)

#define MCHP_OTPC_NAME			"mchp-otpc"
#define MCHP_OTPC_SIZE			(11 * 1024)

#define PACKET_TYPE_REGULAR		1

/**
 * struct mchp_otpc - OTPC private data structure
 * @base: base address
 * @dev: struct device pointer
 * @packets: list of packets in OTP memory
 * @npackets: number of packets in OTP memory
 */
struct mchp_otpc {
	void __iomem *base;
	struct device *dev;
	struct list_head packets;
	u32 npackets;
};

/**
 * struct mchp_otpc_packet - OTPC packet data structure
 * @list: list head
 * @id: packet ID
 * @offset: packet offset (in words) in OTP memory
 * @type: type of the packet
 * @tag: 4-byte ASCII tag of the packet
 */
struct mchp_otpc_packet {
	struct list_head list;
	u32 id;
	u32 offset;
	u32 type;
	u32 tag;
};

static struct mchp_otpc_packet *mchp_otpc_id_to_packet(struct mchp_otpc *otpc,
						       u32 id)
{
	struct mchp_otpc_packet *packet;

	if (id >= otpc->npackets)
		return NULL;

	list_for_each_entry(packet, &otpc->packets, list) {
		if (packet->id == id)
			return packet;
	}

	return NULL;
}

/**
 * mchp_otpc_tag_to_packet() - find packet by tag
 * @otpc: OTPC private data
 * @tag: 4-byte ASCII tag to search for
 *
 * Return: pointer to packet if found, NULL otherwise
 */
static struct mchp_otpc_packet *mchp_otpc_tag_to_packet(struct mchp_otpc *otpc,
							u32 tag)
{
	struct mchp_otpc_packet *packet;

	list_for_each_entry(packet, &otpc->packets, list) {
		if (packet->tag == tag)
			return packet;
	}

	return NULL;
}

/**
 * mchp_otpc_resolve_packet() - resolve offset to packet
 * @otpc: OTPC private data
 * @off: NVMEM offset (legacy ID-based or TAG-based)
 *
 * Legacy offsets (multiples of 4 within valid ID range) are resolved
 * through ID lookup. Other offsets are treated as 4-byte ASCII tags.
 *
 * Return: pointer to packet if found, NULL otherwise
 */
static struct mchp_otpc_packet *mchp_otpc_resolve_packet(struct mchp_otpc *otpc,
							 u32 off)
{
	/*
	 * Legacy id based packet access: offset = id * 4
	 * Inside the driver we use continuous unsigned integer numbers
	 * for packet id, thus divide off by 4 before passing it to
	 * mchp_otpc_id_to_packet().
	 */
	u32 id = off / 4;

	if (!(off % 4) && id < otpc->npackets)
		return mchp_otpc_id_to_packet(otpc, id);

	/*
	 * TAG-based packet access: offset is a 4-byte ASCII tag
	 */
	return mchp_otpc_tag_to_packet(otpc, off);
}

static int mchp_otpc_prepare_read(struct mchp_otpc *otpc,
				  unsigned int offset)
{
	u32 tmp;

	/* Set address. */
	tmp = readl_relaxed(otpc->base + MCHP_OTPC_MR);
	tmp &= ~MCHP_OTPC_MR_ADDR;
	tmp |= FIELD_PREP(MCHP_OTPC_MR_ADDR, offset);
	writel_relaxed(tmp, otpc->base + MCHP_OTPC_MR);

	/* Set read. */
	tmp = readl_relaxed(otpc->base + MCHP_OTPC_CR);
	tmp |= MCHP_OTPC_CR_READ;
	writel_relaxed(tmp, otpc->base + MCHP_OTPC_CR);

	/* Wait for packet to be transferred into temporary buffers. */
	return read_poll_timeout(readl_relaxed, tmp, !(tmp & MCHP_OTPC_SR_READ),
				 10000, 2000, false, otpc->base + MCHP_OTPC_SR);
}

/*
 * OTPC memory is organized into packets. Each packets contains a header and
 * a payload. Header is 4 bytes long and contains the size of the payload.
 * Payload size varies. The memory footprint is something as follows:
 *
 * Memory offset  Memory footprint     Packet ID
 * -------------  ----------------     ---------
 *
 * 0x0            +------------+   <-- packet 0
 *                | header  0  |
 * 0x4            +------------+
 *                | payload 0  |
 *                .            .
 *                .    ...     .
 *                .            .
 * offset1        +------------+   <-- packet 1
 *                | header  1  |
 * offset1 + 0x4  +------------+
 *                | payload 1  |
 *                .            .
 *                .    ...     .
 *                .            .
 * offset2        +------------+   <-- packet 2
 *                .            .
 *                .    ...     .
 *                .            .
 * offsetN        +------------+   <-- packet N
 *                | header  N  |
 * offsetN + 0x4  +------------+
 *                | payload N  |
 *                .            .
 *                .    ...     .
 *                .            .
 *                +------------+
 *
 * where offset1, offset2, offsetN depends on the size of payload 0, payload 1,
 * payload N-1.
 *
 * The access to memory is done on a per packet basis: the control registers
 * need to be updated with an offset address (within a packet range) and the
 * data registers will be update by controller with information contained by
 * that packet. E.g. if control registers are updated with any address within
 * the range [offset1, offset2) the data registers are updated by controller
 * with packet 1. Header data is accessible though MCHP_OTPC_HR register.
 * Payload data is accessible though MCHP_OTPC_DR and MCHP_OTPC_AR registers.
 * There is no direct mapping b/w the offset requested by software and the
 * offset returned by hardware.
 *
 * For this, the read function will return the first requested bytes in the
 * packet.
 *
 * Two offset encoding are supported:
 *
 * 1. Legacy ID-based: offset = OTP_PKT(id) = id * 4
 *    Used in DT as: reg = <OTP_PKT(1) 76>;
 * 2. TAG-based: offset = 4-byte ASCII packet tag
 *    Used in DT as: reg = <0x41435354 0x4c>; (tag "ACST")
 *
 * To use the legacy ID based packet lookup the user will have to be aware of
 * the memory footprint before doing the read request.
 *
 * But by using the TAG based packet lookup, the user won't have to be aware
 * of the memory footprint before doing the read request since this driver has
 * it abstracted and taken care of.
 *
 * Practically, there is no way of knowing the mapping of the OTP memory table
 * in advance for every device. But by using the packet tag - the identifier
 * ASCII value, the packets can be recognized without being aware of the
 * flashed OTP memory map table and the payload can be acquired reliably.
 *
 * While the legacy ID based lookup is still supported, TAG based approach is
 * recommended.
 */
static int mchp_otpc_read(void *priv, unsigned int off, void *val,
			  size_t bytes)
{
	struct mchp_otpc *otpc = priv;
	struct mchp_otpc_packet *packet;
	u32 *buf = val;
	u32 offset;
	size_t len = 0;
	int ret, payload_size;

	/*
	 * From this point the offset has to be translated into the actual
	 * packet. For this we traverse the table of contents stored in a list
	 * "packet" based on the access type - packet id or tag.
	 */
	packet = mchp_otpc_resolve_packet(otpc, off);
	if (!packet)
		return -EINVAL;
	offset = packet->offset;

	while (len < bytes) {
		ret = mchp_otpc_prepare_read(otpc, offset);
		if (ret)
			return ret;

		/* Read and save header content. */
		*buf++ = readl_relaxed(otpc->base + MCHP_OTPC_HR);
		len += sizeof(*buf);
		offset++;
		if (len >= bytes)
			break;

		/* Read and save payload content. */
		payload_size = FIELD_GET(MCHP_OTPC_HR_SIZE, *(buf - 1));
		writel_relaxed(0UL, otpc->base + MCHP_OTPC_AR);
		do {
			*buf++ = readl_relaxed(otpc->base + MCHP_OTPC_DR);
			len += sizeof(*buf);
			offset++;
			payload_size--;
		} while (payload_size >= 0 && len < bytes);
	}

	return 0;
}

/**
 * mchp_otpc_read_packet_tag() - read tag from packet payload
 * @otpc: OTPC private data
 * @offset: packet offset in OTP memory
 * @val: pointer to store the tag value
 *
 * Return: 0 on success, negative errno on failure
 */
static int mchp_otpc_read_packet_tag(struct mchp_otpc *otpc, unsigned int offset,
				     unsigned int *val)
{
	int ret;

	ret = mchp_otpc_prepare_read(otpc, offset);
	if (ret)
		return ret;

	writel_relaxed(0, otpc->base + MCHP_OTPC_AR);
	*val = readl_relaxed(otpc->base + MCHP_OTPC_DR);

	return 0;
}

static int mchp_otpc_init_packets_list(struct mchp_otpc *otpc, u32 *size)
{
	struct mchp_otpc_packet *packet;
	u32 word, word_pos = 0, id = 0, npackets = 0, payload_size;
	int ret;

	INIT_LIST_HEAD(&otpc->packets);
	*size = 0;

	while (*size < MCHP_OTPC_SIZE) {
		ret = mchp_otpc_prepare_read(otpc, word_pos);
		if (ret)
			return ret;

		word = readl_relaxed(otpc->base + MCHP_OTPC_HR);
		payload_size = FIELD_GET(MCHP_OTPC_HR_SIZE, word);
		if (!payload_size)
			break;

		packet = devm_kzalloc(otpc->dev, sizeof(*packet), GFP_KERNEL);
		if (!packet)
			return -ENOMEM;

		packet->id = id++;
		packet->offset = word_pos;
		packet->type = FIELD_GET(MCHP_OTPC_HR_PACKET_TYPE, word);

		if (packet->type == PACKET_TYPE_REGULAR) {
			ret = mchp_otpc_read_packet_tag(otpc, packet->offset,
							&packet->tag);
			if (ret)
				return ret;
		} else {
			packet->tag = 0;
		}

		INIT_LIST_HEAD(&packet->list);
		list_add_tail(&packet->list, &otpc->packets);

		/* Count size by adding header and paload sizes. */
		*size += 4 * (payload_size + 1);
		/* Next word: this packet (header, payload) position + 1. */
		word_pos += payload_size + 2;

		npackets++;
	}

	otpc->npackets = npackets;

	return 0;
}

static struct nvmem_config mchp_nvmem_config = {
	.name = MCHP_OTPC_NAME,
	.type = NVMEM_TYPE_OTP,
	.read_only = true,
	.word_size = 4,
	.stride = 1,
	.reg_read = mchp_otpc_read,
};

static int mchp_otpc_probe(struct platform_device *pdev)
{
	struct nvmem_device *nvmem;
	struct mchp_otpc *otpc;
	bool emul_enable;
	u32 size, tmp;
	int ret;

	otpc = devm_kzalloc(&pdev->dev, sizeof(*otpc), GFP_KERNEL);
	if (!otpc)
		return -ENOMEM;

	otpc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(otpc->base))
		return PTR_ERR(otpc->base);

	otpc->dev = &pdev->dev;

	tmp = readl_relaxed(otpc->base + MCHP_OTPC_MR);
	emul_enable = tmp & MCHP_OTPC_MR_EMUL;
	if (emul_enable)
		dev_info(otpc->dev, "Emulation mode enabled\n");

	ret = mchp_otpc_init_packets_list(otpc, &size);
	if (ret)
		return ret;

	if (!size) {
		dev_warn(otpc->dev, "Cannot access OTP memory\n");
		if (!emul_enable)
			dev_info(otpc->dev, "Boot packet not programmed and emulation mode disabled\n");
	}

	mchp_nvmem_config.dev = otpc->dev;
	mchp_nvmem_config.add_legacy_fixed_of_cells = true;
	mchp_nvmem_config.size = size;
	mchp_nvmem_config.priv = otpc;
	nvmem = devm_nvmem_register(&pdev->dev, &mchp_nvmem_config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id __maybe_unused mchp_otpc_ids[] = {
	{ .compatible = "microchip,sama7g5-otpc", },
	{ },
};
MODULE_DEVICE_TABLE(of, mchp_otpc_ids);

static struct platform_driver mchp_otpc_driver = {
	.probe = mchp_otpc_probe,
	.driver = {
		.name = MCHP_OTPC_NAME,
		.of_match_table = of_match_ptr(mchp_otpc_ids),
	},
};
module_platform_driver(mchp_otpc_driver);

MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea@microchip.com>");
MODULE_DESCRIPTION("Microchip SAMA7G5 OTPC driver");
MODULE_LICENSE("GPL");
