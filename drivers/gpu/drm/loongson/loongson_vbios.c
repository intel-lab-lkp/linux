// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Loongson Technology Corporation Limited
 */

#include <drm/drm_device.h>
#include <drm/drm_managed.h>

#include "loongson_vbios.h"
#include "lsdc_drv.h"

#define LOONGSON_VBIOS_HEADER_STR       "Loongson-VBIOS"
/* Legacy VBIOS is stored at offset 0 */
#define LOONGSON_VBIOS_LEGACY_OFFSET     0
/* The size of legacy VBIOS is 1 KiB */
#define LOONGSON_VBIOS_LEGACY_SIZE       0x000400

/* Data Control Block of Newer version of the VBIOS started at here */
#define LOONGSON_VBIOS_DCB_OFFSET        0x006000
/* The last 1 MiB of the VRAM contains the raw VBIOS data */
#define LOONGSON_VBIOS_BLOCK_OFFSET      0x100000
/* Only 256KB of the 1 MiB are used for now */
#define LOONGSON_VBIOS_BLOCK_SIZE        0x040000

struct loongson_vbios __loongson_vbios;

/*
 * vbios data control block is a kind of metadata, which is used to index
 * real hardware device data block.
 */
struct loongson_vbios_dcb {
	u16 type;    /* what is it */
	u8 version;  /* version of it, useless */
	u8 id;       /* index (usually same with the display pipe) of the hardware */
	u32 offset;  /* the offset of the real data */
	u32 size;    /* the size of the real data */
	u64 ext0;    /* for extension purpose */
	u64 ext1;    /* extra space reserved for future use */
} __packed;

/*
 * Loongson-VBIOS Data Block Layout
 *
 *
 *         _____________________   0x00000
 *        |_____________________|
 *        |                     |  [0x0000, 0x0400) : legacy vbios storage
 *        |    Not Used Yet     |
 *        |                     |
 *        |---------------------|<------- 0x6000
 *   +----|        DCB 0        |
 *   |    |---------------------|
 *   |    |        DCB 1        |
 *   |    |---------------------|      Format of Data Control Blocks
 *   |    | One by one, packed  |            +------------+
 *   |    |---------------------|            |  u16 type  |
 *   |    |        DCB N        |----+       |            |
 *   |    |---------------------|    |       +------------+
 *   |    |          .          |    |       | u8 version |
 *   |    |          .          |    |       |  u8 index  |
 *   |    |          .          |    |       +------------+
 *   |    |---------------------|    |       |            |
 *   |    |        DCB end      |    |       | u32 offset |
 *   |    |---------------------|    |   +-------         |
 *   |    |                     |    |   |   |            |
 *   |    |_____________________|    |   |   +------------+
 *   |    |_____________________|    |   |   |            |
 *   |    |                     |    |   |   |  u32 size  |
 *   +--->|  vbios header info  |    |   |   |         -------+
 *        |_____________________|    |   |   |            |   |
 *        |          .          |    |   |   +------------+   |
 *        |          .          |    |   |   |  useless   |   |
 *        |          .          |    |   |   |  members   |   |
 *        |_____________________|    |   |   +------------+   |
 *        |                     |    |   |                    |
 *        |    encoders info    |<---+   |                    |
 *        |_____________________|        |                    |
 *        |                     |     ___|                    |
 *        |_____________________|____/                        |
 *        |                     |                             |
 *        |      Something      |                             |
 *        |_____________________|_________________            |
 *        |                     |             |               |
 *        |        EDID         |             |<--------------+
 *        |_____________________|_____________|___
 *        |                     |
 *        |                     |    Contents of those device specific data
 *        |  GPU specific info  |    block are implement-defined and version
 *        |                     |    dependent :0
 *        |_____________________|
 *        /          .          /
 *        /          .          /
 *        /          .          /
 *        |_____________________|  0x040000
 *
 */

enum loongson_vbios_dcb_type {
	LV_DCB_HEADER = 0,
	LV_DCB_CRTC = 1,
	LV_DCB_ENCODER = 2,
	LV_DCB_CONNECTOR = 3,
	LV_DCB_I2C = 4,
	LV_DCB_PWM = 5,
	LV_DCB_GPIO = 6,
	LV_DCB_BACKLIGHT = 7,
	LV_DCB_FAN = 8,
	LV_DCB_IRQ = 9,
	LV_DCB_ENCODER_CFG = 10,
	LV_DCB_ENCODER_RES = 11,
	LV_DCB_GPU = 12,
	LV_DCB_UNKNOWN = 13,
	LV_DCB_END = 0xffff,
};

struct loongson_vbios_header {
	char header[16];
	u32 version_major;
	u32 version_minor;
	char information[20];
	u32 num_crtc;
	u32 crtc_offset;
	u32 num_connector;
	u32 connector_offset;
	u32 num_encoder;
	u32 encoder_offset;
} __packed;

struct loongson_vbios_encoder {
	u32 feature;
	u32 i2c_id;
	u32 connector_id;
	u32 type;
	u32 config_method;
	u32 chip_id;
	u8 chip_addr;
} __packed;

struct loongson_vbios_connector {
	u32 feature;
	u32 i2c_id;
	u8  edid[256];
	u32 type;
	u32 hotplug_method;
	u32 edid_method;
	u32 hpd_int_gpio;
	u32 gpio_place;
} __packed;

/*
 * A list node which contains the information about the device specific data
 * block, the device here refer to the property or topology of hardware
 * configuration, such as external display bridges, HDP GPIO, connectors etc.
 */
struct loongson_vbios_node {
	struct list_head head;

	/* @type: the type of the data. For search */
	u32 type;
	/* @id: the index(display pipe) of the data belong to. For search */
	u32 id;
	/*
	 * @data: point to the device specific data block, such as external
	 * encoders name and it's i2c device address, hpd gpio resource etc.
	 */
	const void *data;
	/*
	 * The size of the data.
	 */
	u32 size;
};

/*
 * The returned pointer is actually point to &__loongson_vbios, but this
 * function is only intended to provide READ-ONLY access. As our vbios is
 * only be able to pass(provide) parameters, it is not executable and outside
 * should not modify it.
 */
const struct loongson_vbios *to_loongson_vbios(struct drm_device *ddev)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	const struct loongson_gfx_desc *gfx = to_loongson_gfx(ldev->descp);

	return gfx->vbios;
}

static bool loongson_vbios_is_valid(const struct loongson_vbios *vbios)
{
	char header[32];

	memcpy(header, vbios->raw_data, sizeof(header));

	if (strcmp(header, LOONGSON_VBIOS_HEADER_STR))
		return false;

	return true;
}

/*
 * The VBIOS blob is stored at the last 1 MiB of the VRAM, no SPI flush or
 * EEPROM is needed. Platform BIOS is responsible for store this VBIOS blob
 * data at right position on per boot time.
 */
static int loongson_vbios_construct(struct drm_device *ddev,
				    struct loongson_vbios *vbios)
{
	struct lsdc_device *ldev = to_lsdc(ddev);
	u64 vram_end = ldev->vram_base + ldev->vram_size;
	u64 vbios_start = vram_end - LOONGSON_VBIOS_BLOCK_OFFSET;
	void __iomem *ptr;

	vbios->raw_data = kzalloc(LOONGSON_VBIOS_BLOCK_SIZE, GFP_KERNEL);
	if (!vbios->raw_data)
		return -ENOMEM;

	ptr = ioremap(vbios_start, LOONGSON_VBIOS_BLOCK_SIZE);
	if (!ptr) {
		drm_err(ddev, "Map VBIOS region failed\n");
		return -ENOMEM;
	}

	memcpy_fromio(vbios->raw_data, ptr, LOONGSON_VBIOS_BLOCK_SIZE);

	iounmap(ptr);

	INIT_LIST_HEAD(&vbios->list);
	vbios->ddev = ddev;

	return 0;
}

static void loongson_vbios_destruct(struct drm_device *ddev, void *data)
{
	struct loongson_vbios *vbios = (struct loongson_vbios *)data;
	struct loongson_vbios_node *node;
	struct loongson_vbios_node *tmp;

	list_for_each_entry_safe(node, tmp, &vbios->list, head) {
		list_del(&node->head);
		kfree(node);
	}

	kfree(vbios->raw_data);
	vbios->raw_data = NULL;
}

static void loongson_vbios_print_dcb(struct drm_device *ddev,
				     struct loongson_vbios_dcb *dcb)
{
	drm_info(ddev, "type: %u, Offset: %u, Size: %u, version: %u, ID: %u\n",
		 dcb->type, dcb->offset, dcb->size, dcb->version, dcb->id);
}

/*
 * Process the data control block, establish a list for later searching.
 * returns the number of data control block. Generally, loongson vbios
 * has only 10 DCB or so.
 */
static int loongson_vbios_process_dcb(struct loongson_vbios *vbios,
				      bool verbose)
{
	struct drm_device *ddev = vbios->ddev;
	void *base = vbios->raw_data;
	int count = 0;
	struct loongson_vbios_dcb *dcb;

	dcb = (struct loongson_vbios_dcb *)(base + LOONGSON_VBIOS_DCB_OFFSET);

	while (dcb->type != LV_DCB_END) {
		struct loongson_vbios_node *node;

		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (!node)
			return -ENOMEM;

		node->type = dcb->type;
		node->id = dcb->id;
		node->data = base + dcb->offset;
		node->size = dcb->size;

		list_add_tail(&node->head, &vbios->list);

		if (verbose)
			loongson_vbios_print_dcb(ddev, dcb);

		++dcb;

		if (++count > 1024) {
			drm_err(ddev, "Unlikely, DCB is too much\n");
			break;
		}
	}

	return count;
}

static const struct loongson_vbios_node *
loongson_vbios_get_node(struct drm_device *ddev, u32 type, u32 id)
{
	const struct loongson_vbios *vbios = to_loongson_vbios(ddev);
	struct loongson_vbios_node *np;

	if (!vbios)
		return NULL;

	list_for_each_entry(np, &vbios->list, head) {
		if (np->type == type && np->id == id)
			return np;
	}

	return NULL;
}

bool loongson_vbios_query_encoder_info(struct drm_device *ddev,
				       u32 pipe,
				       u32 *type,
				       enum loongson_vbios_encoder_name *name,
				       u8 *i2c_addr)
{
	const struct loongson_vbios_encoder *vencoder;
	const struct loongson_vbios_node *np;

	np = loongson_vbios_get_node(ddev, LV_DCB_ENCODER, pipe);
	if (!np)
		return false;

	if (np->size != sizeof(*vencoder))
		WARN_ON(1);

	vencoder = (const struct loongson_vbios_encoder *)np->data;

	if (type)
		*type = vencoder->type;

	if (name)
		*name = vencoder->chip_id;

	/* i2c address, as a slave device */
	if (i2c_addr)
		*i2c_addr = vencoder->chip_addr;

	return true;
}

bool loongson_vbios_query_connector_info(struct drm_device *ddev,
					 u32 pipe,
					 u32 *connector_type,
					 u32 *hpd_method,
					 u32 *int_gpio,
					 u8 *edid_blob)
{
	const struct loongson_vbios_connector *vconnector;
	const struct loongson_vbios_node *np;

	np = loongson_vbios_get_node(ddev, LV_DCB_CONNECTOR, pipe);
	if (!np)
		return false;

	if (np->size != sizeof(*vconnector))
		WARN_ON(1);

	vconnector = (const struct loongson_vbios_connector *)np->data;

	if (connector_type)
		*connector_type = vconnector->type;

	if (edid_blob)
		memcpy(edid_blob, vconnector->edid, 256);

	if (int_gpio)
		*int_gpio = vconnector->hpd_int_gpio;

	return true;
}

static void loongson_vbios_acquire_version(struct drm_device *ddev,
					   struct loongson_vbios *vbios)
{
	struct loongson_vbios_header *vh;

	vh = (struct loongson_vbios_header *)vbios->raw_data;

	vbios->version_major = vh->version_major;
	vbios->version_minor = vh->version_minor;

	drm_info(ddev, "Loongson VBIOS version: %u.%u\n",
		 vh->version_major, vh->version_minor);
}

int loongson_vbios_init(struct drm_device *ddev)
{
	struct loongson_vbios *vbios = &__loongson_vbios;
	int ret;
	int num;

	ret = loongson_vbios_construct(ddev, vbios);
	if (ret)
		return ret;

	ret = drmm_add_action_or_reset(ddev, loongson_vbios_destruct, vbios);
	if (ret)
		return ret;

	if (!loongson_vbios_is_valid(vbios)) {
		drm_err(ddev, "Loongson VBIOS: header is invalid\n");
		return -EINVAL;
	}

	loongson_vbios_acquire_version(ddev, vbios);

	num = loongson_vbios_process_dcb(vbios, false);
	if (num <= 0) {
		drm_err(ddev, "Loongson VBIOS: Process DCB failed\n");
		return -EINVAL;
	}

	drm_info(ddev, "Loongson VBIOS: has %d DCBs\n", num);

	return 0;
}
