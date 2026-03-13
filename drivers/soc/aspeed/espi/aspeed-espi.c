// SPDX-License-Identifier: GPL-2.0+
/*
 * Unified Aspeed eSPI driver framework for different generation SoCs
 */

#include <linux/clk.h>
#include <linux/device/devres.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "aspeed-espi.h"
#include "aspeed-espi-comm.h"
#include "ast2600-espi.h"
#include "espi_storage.h"


struct aspeed_espi_ops {
	void (*espi_pre_init)(struct aspeed_espi *espi);
	void (*espi_post_init)(struct aspeed_espi *espi);
	void (*espi_deinit)(struct aspeed_espi *espi);
	int (*espi_perif_probe)(struct aspeed_espi *espi);
	int (*espi_perif_remove)(struct aspeed_espi *espi);
	int (*espi_flash_probe)(struct aspeed_espi *espi);
	int (*espi_flash_remove)(struct aspeed_espi *espi);
	int (*espi_flash_get_hdr)(struct aspeed_espi *espi,
				  struct espi_comm_hdr *hdr);
	int (*espi_flash_get_pkt)(struct aspeed_espi *espi, void *pkt_buf,
				  size_t pkt_size);
	int (*espi_flash_put_pkt)(struct aspeed_espi *espi,
				  struct espi_flash_cmplt hdr, void *pkt_buf,
				  size_t pkt_size);
	void (*espi_flash_clr_pkt)(struct aspeed_espi *espi);
	irqreturn_t (*espi_isr)(int irq, void *espi);
};

static const struct aspeed_espi_ops aspeed_espi_ast2600_ops = {
	.espi_pre_init = ast2600_espi_pre_init,
	.espi_post_init = ast2600_espi_post_init,
	.espi_deinit = ast2600_espi_deinit,
	.espi_perif_probe = ast2600_espi_perif_probe,
	.espi_perif_remove = ast2600_espi_perif_remove,
	.espi_flash_probe = ast2600_espi_flash_probe,
	.espi_flash_remove = ast2600_espi_flash_remove,
	.espi_flash_get_hdr = ast2600_espi_flash_get_hdr,
	.espi_flash_get_pkt = ast2600_espi_flash_get_pkt,
	.espi_flash_put_pkt = ast2600_espi_flash_put_pkt,
	.espi_flash_clr_pkt = ast2600_espi_flash_clr_pkt,
	.espi_isr = ast2600_espi_isr,
};

static const struct of_device_id aspeed_espi_of_matches[] = {
	{ .compatible = "aspeed,ast2600-espi", .data = &aspeed_espi_ast2600_ops },
	{ }
};
MODULE_DEVICE_TABLE(of, aspeed_espi_of_matches);

static void aspeed_espi_flash_handle_lun(struct aspeed_espi *espi)
{
	u32 cyc, len, tag, pkt_len, addr, offset;
	struct espi_flash_cmplt resp_pkt;
	struct aspeed_espi_flash *flash;
	struct espi_flash_rwe *req_pkt;
	struct espi_comm_hdr hdr;
	u8 *payload;
	u8 *buf;
	int rc;

	payload = NULL;
	buf = NULL;

	flash = &espi->flash;
	if (!flash->lun || !flash->lun->filp)
		return;

	rc = espi->ops->espi_flash_get_hdr(espi, &hdr);
	if (rc) {
		dev_err(espi->dev, "espi_flash_handle_lun: get_hdr failed rc=%d\n", rc);
		return;
	}

	if (hdr.cyc != ESPI_FLASH_WRITE && hdr.cyc != ESPI_FLASH_READ &&
	    hdr.cyc != ESPI_FLASH_ERASE) {
		dev_err(espi->dev, "espi_flash_handle_lun: invalid cyc=0x%x\n",
			hdr.cyc);
		return;
	}

	cyc = hdr.cyc;
	len = (hdr.len_h << 8) | hdr.len_l;
	tag = hdr.tag;

	len = len ? len : ESPI_MAX_PLD_LEN;
	pkt_len = len + sizeof(struct espi_flash_rwe);

	payload = kzalloc(pkt_len, GFP_KERNEL);
	if (!payload)
		return;

	rc = espi->ops->espi_flash_get_pkt(espi, payload + sizeof(hdr), pkt_len - sizeof(hdr));
	if (rc) {
		dev_err(espi->dev, "espi_flash_handle_lun: get_pkt failed rc=%d\n", rc);
		goto out_free;
	}

	req_pkt = (struct espi_flash_rwe *)payload;
	req_pkt->cyc = hdr.cyc;
	req_pkt->len_h = hdr.len_h;
	req_pkt->len_l = hdr.len_l;
	req_pkt->tag = hdr.tag;

	addr = be32_to_cpu(req_pkt->addr_be);

	switch (cyc) {
	case ESPI_FLASH_ERASE:
		rc = aspeed_espi_lun_erase_bytes(flash->lun, addr, len);
		resp_pkt.cyc = (rc) ? ESPI_FLASH_UNSUC_CMPLT : ESPI_FLASH_SUC_CMPLT;
		resp_pkt.len_h = 0;
		resp_pkt.len_l = 0;
		resp_pkt.tag = tag;
		espi->ops->espi_flash_put_pkt(espi, resp_pkt, NULL, 0);
		break;
	case ESPI_FLASH_WRITE:
		rc = aspeed_espi_lun_rw_bytes(flash->lun, true, addr, len,
					      &payload[sizeof(struct espi_flash_rwe)]);

		resp_pkt.cyc = (rc) ? ESPI_FLASH_UNSUC_CMPLT : ESPI_FLASH_SUC_CMPLT;
		resp_pkt.len_h = 0;
		resp_pkt.len_l = 0;
		resp_pkt.tag = tag;
		espi->ops->espi_flash_put_pkt(espi, resp_pkt, NULL, 0);
		break;
	case ESPI_FLASH_READ:
		buf = kzalloc(len, GFP_KERNEL);
		if (!buf)
			goto out_free;

		rc = aspeed_espi_lun_rw_bytes(flash->lun, false, addr, len, buf);
		if (rc) {
			resp_pkt.cyc = ESPI_FLASH_UNSUC_CMPLT;
			resp_pkt.len_h = 0;
			resp_pkt.len_l = 0;
			resp_pkt.tag = tag;
			espi->ops->espi_flash_put_pkt(espi, resp_pkt, NULL, 0);
		} else {
			if (len <= ESPI_PLD_LEN_MIN) {
				resp_pkt.cyc = ESPI_FLASH_SUC_CMPLT_D_ONLY;
				resp_pkt.tag = tag;
				resp_pkt.len_h = (len >> 8) & 0xff;
				resp_pkt.len_l = len & 0xff;
				espi->ops->espi_flash_put_pkt(espi, resp_pkt, buf, len);
			} else {
				resp_pkt.cyc = ESPI_FLASH_SUC_CMPLT_D_FIRST;
				resp_pkt.tag = tag;
				resp_pkt.len_h = (ESPI_PLD_LEN_MIN >> 8) & 0xff;
				resp_pkt.len_l = ESPI_PLD_LEN_MIN & 0xff;
				espi->ops->espi_flash_put_pkt(espi, resp_pkt, buf,
							      ESPI_PLD_LEN_MIN);
				offset = ESPI_PLD_LEN_MIN;
				len -= ESPI_PLD_LEN_MIN;

				while (len > ESPI_PLD_LEN_MIN) {
					resp_pkt.cyc = ESPI_FLASH_SUC_CMPLT_D_MIDDLE;
					espi->ops->espi_flash_put_pkt(espi, resp_pkt,
								     &buf[offset],
								     ESPI_PLD_LEN_MIN);
					offset += ESPI_PLD_LEN_MIN;
					len -= ESPI_PLD_LEN_MIN;
				}

				resp_pkt.cyc = ESPI_FLASH_SUC_CMPLT_D_LAST;
				resp_pkt.len_h = (len >> 8) & 0xff;
				resp_pkt.len_l = len & 0xff;
				espi->ops->espi_flash_put_pkt(espi, resp_pkt,
							     &buf[offset], len);
			}
		}
		break;
	default:
		dev_err(espi->dev, "espi_flash_handle_lun: unsupported cyc=0x%x\n", cyc);
		break;
	}
	espi->ops->espi_flash_clr_pkt(espi);
out_free:
	kfree(buf);
	kfree(payload);
}

static void aspeed_espi_flash_rx_work(struct work_struct *work)
{
	struct aspeed_espi_flash *flash = container_of(work, struct aspeed_espi_flash, rx_work);
	struct aspeed_espi *espi = container_of(flash, struct aspeed_espi, flash);

	mutex_lock(&flash->tx_mtx);
	aspeed_espi_flash_handle_lun(espi);
	mutex_unlock(&flash->tx_mtx);
}

static int aspeed_espi_flash_probe(struct aspeed_espi *espi)
{
	struct aspeed_espi_flash *flash;
	struct device *dev;

	flash = &espi->flash;
	dev = espi->dev;

	flash->dma.enable = of_property_read_bool(dev->of_node, "aspeed,flash-dma-mode");
	if (flash->dma.enable) {
		flash->dma.tx_virt = dmam_alloc_coherent(dev, PAGE_SIZE, &flash->dma.tx_addr,
							 GFP_KERNEL);
		if (!flash->dma.tx_virt) {
			dev_err(dev, "cannot allocate DMA TX buffer\n");
			return -ENOMEM;
		}

		flash->dma.rx_virt = dmam_alloc_coherent(dev, PAGE_SIZE, &flash->dma.rx_addr,
							 GFP_KERNEL);
		if (!flash->dma.rx_virt) {
			dev_err(dev, "cannot allocate DMA RX buffer\n");
			return -ENOMEM;
		}
	}

	mutex_init(&flash->tx_mtx);
	INIT_WORK(&flash->rx_work, aspeed_espi_flash_rx_work);

	mutex_init(&espi->flash.lun_mtx);
	espi->flash.lun = NULL;
	espi->flash.lun_path[0] = '\0';
	espi->flash.lun_ro = false;

	return espi->ops->espi_flash_probe(espi);
}

static void aspeed_espi_flash_remove(struct aspeed_espi *espi)
{
	struct aspeed_espi_flash *flash;

	flash = &espi->flash;

	if (espi->ops->espi_flash_remove)
		espi->ops->espi_flash_remove(espi);

	cancel_work_sync(&flash->rx_work);

	if (flash->dma.enable) {
		dmam_free_coherent(espi->dev, PAGE_SIZE, flash->dma.tx_virt, flash->dma.tx_addr);
		dmam_free_coherent(espi->dev, PAGE_SIZE, flash->dma.rx_virt, flash->dma.rx_addr);
	}

	mutex_destroy(&flash->lun_mtx);
	mutex_destroy(&flash->tx_mtx);

	flash->lun = NULL;
	flash->lun_path[0] = '\0';
	flash->lun_ro = false;
}

static int aspeed_espi_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct aspeed_espi *espi;
	struct resource *res;
	struct device *dev;
	int rc;

	dev = &pdev->dev;
	espi = devm_kzalloc(dev, sizeof(*espi), GFP_KERNEL);
	if (!espi)
		return -ENOMEM;

	espi->dev = dev;
	match = of_match_device(aspeed_espi_of_matches, dev);
	if (!match)
		return -ENODEV;

	espi->pdev = pdev;
	espi->ops = match->data;
	if (!espi->ops || !espi->ops->espi_isr)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "cannot get resource\n");
		return -ENODEV;
	}

	espi->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(espi->regs)) {
		dev_err(dev, "cannot map registers\n");
		return PTR_ERR(espi->regs);
	}

	espi->irq = platform_get_irq(pdev, 0);
	if (espi->irq < 0) {
		dev_err(dev, "cannot get IRQ number\n");
		return espi->irq;
	}

	espi->rst = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(espi->rst)) {
		dev_err(dev, "cannot get reset control\n");
		return PTR_ERR(espi->rst);
	}

	espi->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(espi->clk)) {
		dev_err(dev, "cannot get clock control\n");
		return PTR_ERR(espi->clk);
	}

	rc = clk_prepare_enable(espi->clk);
	if (rc) {
		dev_err(dev, "cannot enable clocks\n");
		return rc;
	}

	if (espi->ops->espi_pre_init)
		espi->ops->espi_pre_init(espi);

	if (espi->ops->espi_perif_probe) {
		rc = espi->ops->espi_perif_probe(espi);
		if (rc) {
			dev_err(dev, "cannot init peripheral channel, rc=%d\n", rc);
			goto err_deinit;
		}
	}

	rc = aspeed_espi_flash_probe(espi);
	if (rc) {
		dev_err(dev, "cannot init flash channel, rc=%d\n", rc);
		goto err_remove_perif;
	}

	rc = devm_request_irq(dev, espi->irq, espi->ops->espi_isr, 0,
			      dev_name(dev), espi);
	if (rc) {
		dev_err(dev, "cannot request IRQ\n");
		goto err_remove_flash;
	}

	if (espi->ops->espi_post_init)
		espi->ops->espi_post_init(espi);

	platform_set_drvdata(pdev, espi);

	dev_info(dev, "module loaded\n");

	return 0;

err_remove_flash:
	aspeed_espi_flash_remove(espi);
err_remove_perif:
	if (espi->ops->espi_perif_remove)
		espi->ops->espi_perif_remove(espi);
err_deinit:
	if (espi->ops->espi_deinit)
		espi->ops->espi_deinit(espi);
	clk_disable_unprepare(espi->clk);
	return dev_err_probe(dev, rc, "%s failed\n", __func__);
}

static void aspeed_espi_remove(struct platform_device *pdev)
{
	struct aspeed_espi *espi;

	espi = platform_get_drvdata(pdev);

	if (!espi)
		return;

	aspeed_espi_flash_remove(espi);

	if (espi->ops->espi_perif_remove)
		espi->ops->espi_perif_remove(espi);

	if (espi->ops->espi_deinit)
		espi->ops->espi_deinit(espi);

	clk_disable_unprepare(espi->clk);
}

static struct platform_driver aspeed_espi_driver = {
	.driver = {
		.name = "aspeed-espi",
		.of_match_table = aspeed_espi_of_matches,
	},
	.probe = aspeed_espi_probe,
	.remove = aspeed_espi_remove,
};

module_platform_driver(aspeed_espi_driver);

MODULE_AUTHOR("Aspeed Technology Inc.");
MODULE_DESCRIPTION("Aspeed eSPI controller");
MODULE_LICENSE("GPL");
