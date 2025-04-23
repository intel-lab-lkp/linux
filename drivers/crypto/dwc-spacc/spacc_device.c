// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include "spacc_device.h"

#define VSPACC_PRIORITY_MAX 15

static void spacc_cmd_process(struct spacc_device *spacc, int x)
{
	struct spacc_priv *priv = container_of(spacc, struct spacc_priv, spacc);

	if (!work_pending(&priv->pop_jobs))
		queue_work(priv->spacc_wq, &priv->pop_jobs);
}

static void spacc_stat_process(struct spacc_device *spacc)
{
	struct spacc_priv *priv = container_of(spacc, struct spacc_priv, spacc);

	if (!work_pending(&priv->pop_jobs))
		queue_work(priv->spacc_wq, &priv->pop_jobs);
}

static int spacc_init_device(struct platform_device *pdev)
{
	int vspacc_id = -1;
	u64 timer = 100000;
	void __iomem *baseaddr;
	struct pdu_info   info;
	int vspacc_priority = -1;
	struct spacc_priv *priv;
	int err = 0;
	int oldmode;
	int irq_num;
	const u64 oldtimer = 100000;

	/* initialize DDT DMA pools based on this device's resources */
	if (pdu_mem_init(&pdev->dev)) {
		dev_err(&pdev->dev, "Could not initialize DMA pools\n");
		return -ENOMEM;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto free_ddt_mem_pool;
	}

	/* read spacc priority and index and save inside priv.spacc.config */
	if (of_property_read_u32(pdev->dev.of_node, "snps,vspacc-priority",
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

	/* default to little-endian */
	priv->spacc.config.big_endian	 = false;
	priv->spacc.config.little_endian = true;

	if (of_property_read_u32(pdev->dev.of_node, "snps,vspacc-id",
				 &vspacc_id)) {
		dev_err(&pdev->dev, "No virtual spacc id specified\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	priv->spacc.config.idx = vspacc_id;
	priv->spacc.config.oldtimer = oldtimer;

	if (of_property_read_u64(pdev->dev.of_node, "spacc-wdtimer", &timer)) {
		dev_dbg(&pdev->dev, "No spacc wdtimer specified\n");
		dev_dbg(&pdev->dev, "Default wdtimer: (100000)\n");
		timer = 100000;
	}
	priv->spacc.config.timer = timer;

	baseaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(baseaddr)) {
		dev_err(&pdev->dev, "Unable to map iomem\n");
		err = PTR_ERR(baseaddr);
		goto free_ddt_mem_pool;
	}

	pdu_get_version(baseaddr, &info);

	dev_dbg(&pdev->dev, "EPN %04X : virt [%d]\n",
		info.spacc_version.project,
		info.spacc_version.vspacc_id);

	/*
	 * Validate virtual spacc index with vspacc count read from
	 * VERSION_EXT.VSPACC_CNT. Thus vspacc count=3, gives valid index 0,1,2
	 */
	if (vspacc_id != info.spacc_version.vspacc_id) {
		dev_err(&pdev->dev, "DTS vspacc_id mismatch read value\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	if (vspacc_id < 0 || vspacc_id > (info.spacc_config.num_vspacc - 1)) {
		dev_err(&pdev->dev, "Invalid vspacc index specified\n");
		err = -EINVAL;
		goto free_ddt_mem_pool;
	}

	err = spacc_init(baseaddr, &priv->spacc, &info);
	if (err != 0) {
		dev_err(&pdev->dev, "Failed to initialize SPAcc device\n");
		err = -ENXIO;
		goto free_ddt_mem_pool;
	}

	priv->spacc_wq = alloc_workqueue("spacc_workqueue", WQ_UNBOUND, 0);
	if (!priv->spacc_wq) {
		dev_err(&pdev->dev, "failed to allocated workqueue\n");
		err = -ENOMEM;
		goto free_spacc_ctx;
	}

	spacc_irq_glbl_disable(&priv->spacc);
	INIT_WORK(&priv->pop_jobs, spacc_pop_jobs);

	priv->spacc.dptr = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	irq_num = platform_get_irq(pdev, 0);
	if (irq_num < 0) {
		dev_err(&pdev->dev, "No irq resource for spacc\n");
		err = -ENXIO;
		goto free_spacc_workq;
	}

	/* determine configured maximum message length */
	priv->max_msg_len = priv->spacc.config.max_msg_size;

	if (devm_request_irq(&pdev->dev, irq_num, spacc_irq_handler,
			     IRQF_SHARED, dev_name(&pdev->dev),
			     &pdev->dev)) {
		dev_err(&pdev->dev, "Failed to request IRQ\n");
		err = -EBUSY;
		goto free_spacc_workq;
	}

	priv->spacc.irq_cb_stat = spacc_stat_process;
	priv->spacc.irq_cb_cmdx = spacc_cmd_process;
	oldmode			= priv->spacc.op_mode;
	priv->spacc.op_mode     = SPACC_OP_MODE_IRQ;

	/* Enable STAT and CMD interrupts */
	spacc_irq_stat_enable(&priv->spacc, 1);
	spacc_irq_cmdx_enable(&priv->spacc, 0, 1);
	spacc_irq_stat_wd_disable(&priv->spacc);
	spacc_irq_glbl_enable(&priv->spacc);

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_AUTODETECT)

	err = spacc_autodetect(&priv->spacc);
	if (err < 0) {
		spacc_irq_glbl_disable(&priv->spacc);
		goto free_spacc_workq;
	}
#else
	err = spacc_static_config(&priv->spacc);
	if (err < 0) {
		spacc_irq_glbl_disable(&priv->spacc);
		goto free_spacc_workq;
	}
#endif

	priv->spacc.op_mode = oldmode;
	if (priv->spacc.op_mode == SPACC_OP_MODE_IRQ) {
		priv->spacc.irq_cb_stat = spacc_stat_process;
		priv->spacc.irq_cb_cmdx = spacc_cmd_process;

		/* Enable STAT and CMD interrupts */
		spacc_irq_stat_enable(&priv->spacc, 1);
		spacc_irq_cmdx_enable(&priv->spacc, 0, 1);
		spacc_irq_glbl_enable(&priv->spacc);
	} else {
		priv->spacc.irq_cb_stat = spacc_stat_process;
		priv->spacc.irq_cb_stat_wd = spacc_stat_process;

		spacc_irq_stat_enable(&priv->spacc,
				      priv->spacc.config.ideal_stat_level);

		/* Enable STAT and WD interrupts */
		spacc_irq_cmdx_disable(&priv->spacc, 0);
		spacc_irq_stat_wd_enable(&priv->spacc);
		spacc_irq_glbl_enable(&priv->spacc);

		/* enable the wd by setting the wd_timer = 100000 */
		spacc_set_wd_count(&priv->spacc,
				   priv->spacc.config.wd_timer =
						priv->spacc.config.timer);
	}

	/* unlock normal */
	if (priv->spacc.config.is_secure_port) {
		u32 t;

		t = readl(baseaddr + SPACC_REG_SECURE_CTRL);
		t &= ~(1UL << 31);
		writel(t, baseaddr + SPACC_REG_SECURE_CTRL);
	}

	/* unlock device by default */
	writel(0, baseaddr + SPACC_REG_SECURE_CTRL);

	return err;

free_spacc_workq:
	flush_workqueue(priv->spacc_wq);
	destroy_workqueue(priv->spacc_wq);

free_spacc_ctx:
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
	int rc = 0;

	rc = spacc_init_device(pdev);
	if (rc < 0)
		goto err;

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_HASH)
	rc = spacc_probe_hashes(pdev);
	if (rc < 0)
		goto err;
#endif

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_CIPHER)
	rc = spacc_probe_ciphers(pdev);
	if (rc < 0)
		goto err;
#endif

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_AEAD)
	rc = spacc_probe_aeads(pdev);
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
	struct spacc_priv *priv = platform_get_drvdata(pdev);

	if (priv->spacc_wq) {
		flush_workqueue(priv->spacc_wq);
		destroy_workqueue(priv->spacc_wq);
	}

	spacc_unregister_algs();
	spacc_remove(pdev);
}

static const struct of_device_id snps_spacc_id[] = {
	{.compatible = "snps,dwc-spacc" },
	{ /* sentinel */        }
};

MODULE_DEVICE_TABLE(of, snps_spacc_id);

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
