// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Loongson2 SoC APB DMA Mux Controller
 *
 * Copyright (C) 2026 Loongson Technology Corporation Limited
 *
 * Device tree bindings:
 *
 *   2K0300:
 *     dma_mux: dma-mux@16000134 {
 *             compatible = "loongson,ls2k0300-dmamux";
 *             reg = <0x0 0x16000134 0x0 0xc>;
 *             #dma-cells = <3>;
 *             dma-masters = <&dma>;
 *             dma-requests = <22>;
 *     };
 *
 * DMA specifier: <request> <channel> <flags>
 *   - request: peripheral request ID (LS2K0300_DMA_* or LS2K1000_DMA_*)
 *   - channel: target physical DMA channel or controller index
 *   - flags:   channel configuration flags forwarded to the parent DMA
 *              controller
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/dma/loongson,ls2k-dmamux.h>

/* Loongson-2K0300 registers definitions */
#define LS2K0300_CHIP_CTRL13		0x00
#define LS2K0300_CHIP_CTRL14		0x04
#define LS2K0300_CHIP_CTRL15		0x08

/* CHIP_CTRL13 bitfields */
#define CTRL13_UART0_DMA_MAP		GENMASK(17, 16)
#define CTRL13_UART1_DMA_MAP		GENMASK(19, 18)
#define CTRL13_UART2_DMA_MAP		GENMASK(21, 20)
#define CTRL13_UART3_DMA_MAP		GENMASK(23, 22)
#define CTRL13_UART4_DMA_MAP		GENMASK(25, 24)
#define CTRL13_UART5_DMA_MAP		GENMASK(27, 26)
#define CTRL13_UART6_DMA_MAP		GENMASK(29, 28)
#define CTRL13_UART7_DMA_MAP		GENMASK(31, 30)

/* CHIP_CTRL14 bitfields */
#define CTRL14_UART8_DMA_MAP		GENMASK(1, 0)
#define CTRL14_UART9_DMA_MAP		GENMASK(3, 2)
#define CTRL14_I2C0_DMA_MAP		GENMASK(5, 4)
#define CTRL14_I2C1_DMA_MAP		GENMASK(7, 6)
#define CTRL14_I2C2_DMA_MAP		GENMASK(9, 8)
#define CTRL14_I2C3_DMA_MAP		GENMASK(11, 10)
#define CTRL14_SPI2_DMA_MAP		GENMASK(13, 12)
#define CTRL14_SPI3_DMA_MAP		GENMASK(15, 14)
#define CTRL14_I2S_DMA_MAP		GENMASK(17, 16)
#define CTRL14_ADC_DMA_MAP		GENMASK(20, 18)
#define CTRL14_CAN0_DMA_MAP		GENMASK(23, 21)
#define CTRL14_CAN1_DMA_MAP		GENMASK(26, 24)
#define CTRL14_CAN2_DMA_MAP		GENMASK(29, 27)
#define CTRL14_CAN3_DMA_MAP		GENMASK(31, 30)

/* CHIP_CTRL15 bitfields */
#define CTRL15_CAN3_DMA_MAP_HI		BIT(0)

#define LS2K0300_DMA_MAX_CHANNEL	7

/**
 * enum loongson2_dmamux_type - Type of the mapping field
 * @LOONGSON_DMAMUX_PAIR: 2-bit field, value selects a channel pair
 *                        (0/1, 2/3, 4/5, 6/7) — used on 2K0300.
 * @LOONGSON_DMAMUX_SINGLE: 3-bit field, value selects a single channel
 *                          or controller index directly.
 */
enum loongson2_dmamux_type {
	LOONGSON_DMAMUX_PAIR,
	LOONGSON_DMAMUX_SINGLE,
};

/**
 * struct loongson2_dmamux_map - DMA request to register bitfield mapping
 * @reg: Register offset from the controller base
 * @mask: Bitfield mask within the register
 * @shift: Starting bit position of the bitfield
 * @type: Mapping type (pair or single)
 * @high_reg: Optional extension register offset (0 if unused)
 * @high_shift: Bit position in the extension register
 */
struct loongson2_dmamux_map {
	u32 reg;
	u32 mask;
	u8 shift;
	enum loongson2_dmamux_type type;
	u32 high_reg;
	u8 high_shift;
};

/**
 * struct loongson2_dmamux_config - SoC-specific DMA mux configuration
 * @maps: Pointer to the request mapping table
 * @num_maps: Number of entries in @maps
 * @max_channel: Highest valid channel/controller index
 * @parent_args_count: Number of cells to write to dma_spec->args[]
 *                     when forwarding to the parent xlate.
 */
struct loongson2_dmamux_config {
	const struct loongson2_dmamux_map *maps;
	u32 num_maps;
	u32 max_channel;
	u32 parent_args_count;
};

/**
 * struct loongson2_dmamux - DMA mux controller state
 * @dev: Back pointer to the platform device
 * @base: I/O-mapped register base
 * @lock: Protects register read-modify-write sequences
 * @dma_np: The parent DMA controller device nodes
 * @dma_router: Router structure passed to of_dma_router_register()
 * @config: SoC-specific configuration
 */
struct loongson2_dmamux {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;	/* Protects register read-modify-write sequences */
	struct device_node *dma_np;
	struct dma_router dma_router;
	const struct loongson2_dmamux_config *config;
};

/* Loongson-2K0300 mapping table */
static const struct loongson2_dmamux_map ls2k0300_dmamux_maps[] = {
	[LS2K0300_DMA_UART0] = { LS2K0300_CHIP_CTRL13, CTRL13_UART0_DMA_MAP, 16,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART1] = { LS2K0300_CHIP_CTRL13, CTRL13_UART1_DMA_MAP, 18,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART2] = { LS2K0300_CHIP_CTRL13, CTRL13_UART2_DMA_MAP, 20,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART3] = { LS2K0300_CHIP_CTRL13, CTRL13_UART3_DMA_MAP, 22,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART4] = { LS2K0300_CHIP_CTRL13, CTRL13_UART4_DMA_MAP, 24,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART5] = { LS2K0300_CHIP_CTRL13, CTRL13_UART5_DMA_MAP, 26,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART6] = { LS2K0300_CHIP_CTRL13, CTRL13_UART6_DMA_MAP, 28,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART7] = { LS2K0300_CHIP_CTRL13, CTRL13_UART7_DMA_MAP, 30,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART8] = { LS2K0300_CHIP_CTRL14, CTRL14_UART8_DMA_MAP,  0,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_UART9] = { LS2K0300_CHIP_CTRL14, CTRL14_UART9_DMA_MAP,  2,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_I2C0]  = { LS2K0300_CHIP_CTRL14, CTRL14_I2C0_DMA_MAP,   4,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_I2C1]  = { LS2K0300_CHIP_CTRL14, CTRL14_I2C1_DMA_MAP,   6,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_I2C2]  = { LS2K0300_CHIP_CTRL14, CTRL14_I2C2_DMA_MAP,   8,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_I2C3]  = { LS2K0300_CHIP_CTRL14, CTRL14_I2C3_DMA_MAP,  10,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_SPI2]  = { LS2K0300_CHIP_CTRL14, CTRL14_SPI2_DMA_MAP,  12,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_SPI3]  = { LS2K0300_CHIP_CTRL14, CTRL14_SPI3_DMA_MAP,  14,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_I2S]   = { LS2K0300_CHIP_CTRL14, CTRL14_I2S_DMA_MAP,   16,
				 LOONGSON_DMAMUX_PAIR,   0, 0 },
	[LS2K0300_DMA_ADC]   = { LS2K0300_CHIP_CTRL14, CTRL14_ADC_DMA_MAP,   18,
				 LOONGSON_DMAMUX_SINGLE, 0, 0 },
	[LS2K0300_DMA_CAN0]  = { LS2K0300_CHIP_CTRL14, CTRL14_CAN0_DMA_MAP,  21,
				 LOONGSON_DMAMUX_SINGLE, 0, 0 },
	[LS2K0300_DMA_CAN1]  = { LS2K0300_CHIP_CTRL14, CTRL14_CAN1_DMA_MAP,  24,
				 LOONGSON_DMAMUX_SINGLE, 0, 0 },
	[LS2K0300_DMA_CAN2]  = { LS2K0300_CHIP_CTRL14, CTRL14_CAN2_DMA_MAP,  27,
				 LOONGSON_DMAMUX_SINGLE, 0, 0 },
	[LS2K0300_DMA_CAN3]  = { LS2K0300_CHIP_CTRL14, CTRL14_CAN3_DMA_MAP,  30,
				 LOONGSON_DMAMUX_SINGLE, LS2K0300_CHIP_CTRL15, 0 },
};

static const struct loongson2_dmamux_config ls2k0300_dmamux_config = {
	.maps			= ls2k0300_dmamux_maps,
	.num_maps		= ARRAY_SIZE(ls2k0300_dmamux_maps),
	.max_channel		= LS2K0300_DMA_MAX_CHANNEL,
	.parent_args_count	= 2,
};

static inline u32 loongson2_dmamux_read(struct loongson2_dmamux *mux, u32 reg)
{
	return readl(mux->base + reg);
}

static inline void loongson2_dmamux_write(struct loongson2_dmamux *mux, u32 reg, u32 val)
{
	writel(val, mux->base + reg);
}

/**
 * loongson2_dmamux_set_route() - Program the DMA request route
 * @mux:     DMA mux controller
 * @request: Peripheral DMA request ID
 * @channel: Target physical DMA channel or controller index
 *
 * Computes the register value according to the mapping type:
 *   - PAIR   : channel / 2 selects the pair index
 *   - SINGLE : channel is written directly
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int loongson2_dmamux_set_route(struct loongson2_dmamux *mux, u32 request, u32 channel)
{
	const struct loongson2_dmamux_map *map;
	u32 reg_val, route_val, high_val = 0;

	if (request >= mux->config->num_maps)
		return -EINVAL;

	if (channel > mux->config->max_channel)
		return -EINVAL;

	map = &mux->config->maps[request];

	switch (map->type) {
	case LOONGSON_DMAMUX_PAIR:
		route_val = channel / 2;
		break;
	case LOONGSON_DMAMUX_SINGLE:
		route_val = channel;
		if (map->high_reg)
			high_val = (channel >> 2) & 0x1;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&mux->lock);

	/* Write the primary register field */
	reg_val = loongson2_dmamux_read(mux, map->reg);
	reg_val &= ~map->mask;
	reg_val |= field_prep(map->mask, route_val);
	loongson2_dmamux_write(mux, map->reg, reg_val);

	/* Write the extension register field if present */
	if (map->high_reg) {
		reg_val = loongson2_dmamux_read(mux, map->high_reg);
		reg_val &= ~BIT(map->high_shift);
		reg_val |= high_val << map->high_shift;
		loongson2_dmamux_write(mux, map->high_reg, reg_val);
	}

	mutex_unlock(&mux->lock);

	return 0;
}

/**
 * loongson2_dmamux_route_allocate() - DMA router allocation callback
 * @dma_spec: OF DMA specifier <request> <channel> <flags>
 * @ofdma:    Router's of_dma structure; ofdma->dma_router points to our
 *            struct dma_router embedded in struct loongson2_dmamux.
 *
 * Return: NULL as route_data (nothing to free), or an ERR_PTR on failure.
 */
static void *loongson2_dmamux_route_allocate(struct of_phandle_args *dma_spec,
					     struct of_dma *ofdma)
{
	struct loongson2_dmamux *mux;
	u32 request, channel, flags;
	int ret;

	if (!ofdma->dma_router)
		return ERR_PTR(-EINVAL);

	mux = container_of(ofdma->dma_router, struct loongson2_dmamux, dma_router);

	if (dma_spec->args_count < 3)
		return ERR_PTR(-EINVAL);

	request = dma_spec->args[0];
	channel = dma_spec->args[1];
	flags = dma_spec->args[2];

	if (request >= mux->config->num_maps ||
	    channel > mux->config->max_channel)
		return ERR_PTR(-EINVAL);

	/* Program the SoC-specific route register */
	ret = loongson2_dmamux_set_route(mux, request, channel);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * Rewrite dma_spec so the kernel router framework uses it to look
	 * up the parent DMA controller and invoke its xlate.
	 */
	dma_spec->np = mux->dma_np;
	dma_spec->args[0] = channel;
	dma_spec->args[1] = flags;
	dma_spec->args_count = mux->config->parent_args_count;

	return NULL;
}

static int loongson2_dmamux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct loongson2_dmamux *mux;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->dev = dev;
	mux->config = &ls2k0300_dmamux_config;
	mutex_init(&mux->lock);

	mux->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mux->base))
		return PTR_ERR(mux->base);

	/* Parse parent DMA controller nodes */
	mux->dma_np = of_parse_phandle(dev->of_node, "dma-masters", 0);
	if (!mux->dma_np)
		return dev_err_probe(dev, -ENODEV,
				     "Failed to find parent DMA controller\n");

	mux->dma_router.dev = dev;
	mux->dma_router.route_free = NULL;

	platform_set_drvdata(pdev, mux);

	ret = of_dma_router_register(dev->of_node, loongson2_dmamux_route_allocate,
				     &mux->dma_router);
	if (ret) {
		of_node_put(mux->dma_np);
		return dev_err_probe(dev, ret,
				     "Failed to register DMA router\n");
	}

	dev_info(dev,
		 "Loongson2 DMA Mux Controller registered (max_channel=%u, requests=%u)\n",
		 mux->config->max_channel, mux->config->num_maps);

	return 0;
}

static void loongson2_dmamux_remove(struct platform_device *pdev)
{
	struct loongson2_dmamux *mux = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	of_node_put(mux->dma_np);
}

static const struct of_device_id loongson2_dmamux_of_match[] = {
	{ .compatible = "loongson,ls2k0300-dmamux" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, loongson2_dmamux_of_match);

static struct platform_driver loongson2_dmamux_driver = {
	.driver = {
		.name = "loongson2-dmamux",
		.of_match_table = loongson2_dmamux_of_match,
	},
	.probe = loongson2_dmamux_probe,
	.remove = loongson2_dmamux_remove,
};
module_platform_driver(loongson2_dmamux_driver);

MODULE_DESCRIPTION("Loongson SoC DMA Mux Controller driver");
MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_LICENSE("GPL");
