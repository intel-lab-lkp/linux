// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include "spacc_device.h"

static struct platform_device *spacc_pdev[MAX_DEVICES];

#define VSPACC_PRIORITY_MAX 15

void spacc_cmd_process(struct spacc_device *spacc, int x)
{
	struct spacc_priv *priv = container_of(spacc, struct spacc_priv, spacc);

	/* run tasklet to pop jobs off fifo */
	tasklet_schedule(&priv->pop_jobs);
}
void spacc_stat_process(struct spacc_device *spacc)
{
	struct spacc_priv *priv = container_of(spacc, struct spacc_priv, spacc);

	/* run tasklet to pop jobs off fifo */
	tasklet_schedule(&priv->pop_jobs);
}

static const struct of_device_id snps_spacc_id[] = {
	{.compatible = "snps,dwc-spacc" },
	{ /*sentinel */        }
};

MODULE_DEVICE_TABLE(of, snps_spacc_id);

static int spacc_init_device(struct platform_device *pdev)
{
	int vspacc_idx = -1;
	struct resource *mem;
	void __iomem *baseaddr;
	struct pdu_info   info;
	int vspacc_priority = -1;
	struct spacc_priv *priv;
	int x = 0, err, oldmode, irq_num;
	u64 oldtimer = 100000, timer = 100000;

	/* Initialize DDT DMA pools based on this device's resources */
	if (pdu_mem_init(&pdev->dev)) {
		dev_err(&pdev->dev, "Could not initialize DMA pools\n");
		return -ENOMEM;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "no memory resource for spacc\n");
		err = -ENXIO;
		goto free_ddt_mem_pool;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto free_ddt_mem_pool;
	}

	/* Read spacc priority and index and save inside priv.spacc.config */
	if (of_property_read_u32(pdev->dev.of_node, "vspacc-priority",
				 &vspacc_priority)) {
		dev_err(&pdev->dev, "No virtual spacc priority specified\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	if (vspacc_priority < 0 && vspacc_priority > VSPACC_PRIORITY_MAX) {
		dev_err(&pdev->dev, "Invalid virtual spacc priority\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}
	priv->spacc.config.priority = vspacc_priority;

	if (of_property_read_u32(pdev->dev.of_node, "vspacc-index",
				 &vspacc_idx)) {
		dev_err(&pdev->dev, "No virtual spacc index specified\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}
	priv->spacc.config.idx = vspacc_idx;

	priv->spacc.config.spacc_endian = of_property_read_bool(
				pdev->dev.of_node, "little-endian");

	priv->spacc.config.oldtimer = oldtimer;

	if (of_property_read_u64(pdev->dev.of_node, "spacc-wdtimer", &timer)) {
		dev_dbg(&pdev->dev, "No spacc wdtimer specified\n");
		dev_dbg(&pdev->dev, "Default wdtimer: (100000)\n");
		timer = 100000;
	}
	priv->spacc.config.timer = timer;

	baseaddr = devm_platform_get_and_ioremap_resource(pdev, 0, &mem);
	if (IS_ERR(baseaddr)) {
		dev_err(&pdev->dev, "unable to map iomem\n");
		err = PTR_ERR(baseaddr);
		goto free_ddt_mem_pool;
	}

	pdu_get_version(baseaddr, &info);

	dev_dbg(&pdev->dev, "EPN %04X : virt [%d]\n",
				info.spacc_version.project,
				info.spacc_version.vspacc_idx);

	/* Validate virtual spacc index with vspacc count read from
	 * VERSION_EXT.VSPACC_CNT. Thus vspacc count=3, gives valid index 0,1,2
	 */
	if (vspacc_idx != info.spacc_version.vspacc_idx) {
		dev_err(&pdev->dev, "DTS vspacc_idx mismatch read value\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	if (vspacc_idx < 0 || vspacc_idx > (info.spacc_config.num_vspacc - 1)) {
		dev_err(&pdev->dev, "Invalid vspacc index specified\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	err = spacc_init(baseaddr, &priv->spacc, &info);
	if (err != CRYPTO_OK) {
		dev_err(&pdev->dev, "Failed to initialize device %d\n", x);
		err = -ENXIO;
		goto free_ddt_mem_pool;
	}

	spin_lock_init(&priv->hw_lock);
	spacc_irq_glbl_disable(&priv->spacc);
	tasklet_init(&priv->pop_jobs, spacc_pop_jobs, (unsigned long)priv);

	priv->spacc.dptr = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	irq_num = platform_get_irq(pdev, 0);
	if (irq_num < 0) {
		dev_err(&pdev->dev, "no irq resource for spacc\n");
		err = -ENXIO;
		goto free_ddt_mem_pool;
	}

	/* Determine configured maximum message length. */
	priv->max_msg_len = priv->spacc.config.max_msg_size;

	if (devm_request_irq(&pdev->dev, irq_num, spacc_irq_handler,
			     IRQF_SHARED, dev_name(&pdev->dev),
			     &pdev->dev)) {
		dev_err(&pdev->dev, "failed to request IRQ\n");
		err = -EBUSY;
		goto err_tasklet_kill;
	}

	priv->spacc.irq_cb_stat = spacc_stat_process;
	priv->spacc.irq_cb_cmdx = spacc_cmd_process;
	oldmode			= priv->spacc.op_mode;
	priv->spacc.op_mode     = SPACC_OP_MODE_IRQ;

	spacc_irq_stat_enable(&priv->spacc, 1);
	spacc_irq_cmdx_enable(&priv->spacc, 0, 1);
	spacc_irq_stat_wd_disable(&priv->spacc);
	spacc_irq_glbl_enable(&priv->spacc);


#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_AUTODETECT)
	err = spacc_autodetect(&priv->spacc);
	if (err < 0) {
		spacc_irq_glbl_disable(&priv->spacc);
		goto err_tasklet_kill;
	}
#else
	err = spacc_static_config(&priv->spacc);
	if (err < 0) {
		spacc_irq_glbl_disable(&priv->spacc);
		goto err_tasklet_kill;
	}
#endif

	priv->spacc.op_mode = oldmode;

	if (priv->spacc.op_mode == SPACC_OP_MODE_IRQ) {
		priv->spacc.irq_cb_stat = spacc_stat_process;
		priv->spacc.irq_cb_cmdx = spacc_cmd_process;

		spacc_irq_stat_enable(&priv->spacc, 1);
		spacc_irq_cmdx_enable(&priv->spacc, 0, 1);
		spacc_irq_glbl_enable(&priv->spacc);
	} else {
		priv->spacc.irq_cb_stat = spacc_stat_process;
		priv->spacc.irq_cb_stat_wd = spacc_stat_process;

		spacc_irq_stat_enable(&priv->spacc,
				      priv->spacc.config.ideal_stat_level);

		spacc_irq_cmdx_disable(&priv->spacc, 0);
		spacc_irq_stat_wd_enable(&priv->spacc);
		spacc_irq_glbl_enable(&priv->spacc);

		/* enable the wd by setting the wd_timer = 100000 */
		spacc_set_wd_count(&priv->spacc,
				   priv->spacc.config.wd_timer =
						priv->spacc.config.timer);
	}

	/* unlock normal*/
	if (priv->spacc.config.is_secure_port) {
		u32 t;

		t = readl(baseaddr + SPACC_REG_SECURE_CTRL);
		t &= ~(1UL << 31);
		writel(t, baseaddr + SPACC_REG_SECURE_CTRL);
	}

	/* unlock device by default */
	writel(0, baseaddr + SPACC_REG_SECURE_CTRL);

	return err;

err_tasklet_kill:
	tasklet_kill(&priv->pop_jobs);
	spacc_fini(&priv->spacc);

free_ddt_mem_pool:
	pdu_mem_deinit(&pdev->dev);

	return err;
}

static void spacc_unregister_algs(void)
{
#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_HASH)
	spacc_unregister_hash_algs();
#endif
#if  IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_AEAD)
	spacc_unregister_aead_algs();
#endif
#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_CIPHER)
	spacc_unregister_cipher_algs();
#endif
}


static int spacc_crypto_probe(struct platform_device *pdev)
{
	int rc;

	rc = spacc_init_device(pdev);
	if (rc < 0)
		goto err;

	spacc_pdev[0] = pdev;

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_HASH)
	rc = probe_hashes(pdev);
	if (rc < 0)
		goto err;
#endif

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_CIPHER)
	rc = probe_ciphers(pdev);
	if (rc < 0)
		goto err;
#endif

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_AEAD)
	rc = probe_aeads(pdev);
	if (rc < 0)
		goto err;
#endif

	return 0;
err:
	spacc_unregister_algs();

	return rc;
}

static void spacc_crypto_remove(struct platform_device *pdev)
{
	spacc_unregister_algs();
	spacc_remove(pdev);
}

static struct platform_driver spacc_driver = {
	.probe  = spacc_crypto_probe,
	.remove = spacc_crypto_remove,
	.driver = {
		.name  = "spacc",
		.of_match_table = snps_spacc_id,
		.owner = THIS_MODULE,
	},
};

module_platform_driver(spacc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Synopsys, Inc.");
MODULE_DESCRIPTION("SPAcc Crypto Accelerator Driver");
