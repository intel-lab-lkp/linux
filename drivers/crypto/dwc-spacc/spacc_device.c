// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <crypto/engine.h>
#include "spacc_device.h"

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
	void __iomem *baseaddr;
	struct pdu_info   info;
	struct spacc_priv *priv;
	int err = 0;
	int ret = 0;
	int oldmode;
	int irq_num;
	int irq_ret;
	const u64 oldtimer = SPACC_OLD_TIMER;

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

	/* default to little-endian */
	priv->spacc.config.big_endian    = false;
	priv->spacc.config.little_endian = true;

	priv->spacc.config.oldtimer = oldtimer;

	/* Set the SPAcc internal counter value from kernel config */
	priv->spacc.config.timer =
		(u64)CONFIG_CRYPTO_DEV_SPACC_INTERNAL_COUNTER;
	dev_dbg(&pdev->dev, "SPAcc internal counter set to: %llu\n",
		priv->spacc.config.timer);

	baseaddr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(baseaddr)) {
		dev_err(&pdev->dev, "Unable to map iomem\n");
		err = PTR_ERR(baseaddr);
		goto free_ddt_mem_pool;
	}

	pdu_get_version(baseaddr, &info);

	ret = spacc_init(baseaddr, &priv->spacc, &info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to initialize SPAcc device\n");
		err = ret;
		goto free_ddt_mem_pool;
	}

	/* Set the priority from kernel config */
	priv->spacc.config.priority = CONFIG_CRYPTO_DEV_SPACC_PRIORITY;
	dev_dbg(&pdev->dev, "VSPACC priority set from config: %u\n",
		priv->spacc.config.priority);

	/* Set the priority for this virtual SPAcc instance */
	spacc_set_priority(&priv->spacc, priv->spacc.config.priority);

	/* Initialize crypto engine */
	priv->engine = crypto_engine_alloc_init(&pdev->dev, true);
	if (!priv->engine) {
		dev_err(&pdev->dev, "Could not allocate crypto engine\n");
		err = -ENOMEM;
		goto free_spacc_ctx;
	}

	err = crypto_engine_start(priv->engine);
	if (err) {
		dev_err(&pdev->dev, "Could not start crypto engine\n");
		goto free_engine;
	}

	priv->spacc_wq = alloc_workqueue("spacc_workqueue", WQ_UNBOUND, 0);
	if (!priv->spacc_wq) {
		err = -ENOMEM;
		goto free_engine;
	}

	INIT_WORK(&priv->pop_jobs, spacc_pop_jobs);
	spacc_irq_glbl_disable(&priv->spacc);

	priv->spacc.dptr = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	irq_num = platform_get_irq(pdev, 0);
	if (irq_num < 0) {
		err = irq_num;
		goto free_spacc_workq;
	}

	/* determine configured maximum message length */
	priv->max_msg_len = priv->spacc.config.max_msg_size;

	irq_ret = devm_request_irq(&pdev->dev, irq_num, spacc_irq_handler,
			     IRQF_SHARED, dev_name(&pdev->dev),
			     &pdev->dev);
	if (irq_ret) {
		dev_err(&pdev->dev, "Failed to request IRQ : %d\n", irq_ret);
		err = irq_ret;
		goto free_spacc_workq;
	}

	priv->spacc.irq_cb_stat	= spacc_stat_process;
	priv->spacc.irq_cb_cmdx	= spacc_cmd_process;
	oldmode			= priv->spacc.op_mode;
	priv->spacc.op_mode	= SPACC_OP_MODE_IRQ;

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
		priv->spacc.config.wd_timer = priv->spacc.config.timer;
		spacc_set_wd_count(&priv->spacc, priv->spacc.config.wd_timer);
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

	return 0;

free_spacc_workq:
	destroy_workqueue(priv->spacc_wq);

free_engine:
	crypto_engine_exit(priv->engine);
free_spacc_ctx:
	spacc_fini(&priv->spacc);

free_ddt_mem_pool:
	pdu_mem_deinit(&pdev->dev);

	return err;
}

static void spacc_unregister_algs(struct spacc_priv *priv)
{
#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_HASH)
	spacc_unregister_hash_algs(priv);
#endif
}

static void spacc_crypto_remove(struct platform_device *pdev)
{
	struct spacc_priv *priv = platform_get_drvdata(pdev);

	spacc_irq_glbl_disable(&priv->spacc);
	spacc_unregister_algs(priv);

	if (priv->engine)
		crypto_engine_exit(priv->engine);

	if (priv->spacc_wq)
		destroy_workqueue(priv->spacc_wq);

	spacc_remove(pdev);
}

static int spacc_crypto_probe(struct platform_device *pdev)
{
	int rc = 0;

	rc = spacc_init_device(pdev);
	if (rc < 0)
		return rc;

#if IS_ENABLED(CONFIG_CRYPTO_DEV_SPACC_HASH)
	rc = spacc_probe_hashes(pdev);
	if (rc < 0)
		goto err;
#endif

	return 0;
err:
	spacc_crypto_remove(pdev);

	return rc;
}

static const struct of_device_id snps_spacc_id[] = {
	{.compatible = "snps,nsimosci-hs-spacc" },
	{ /* sentinel */        }
};

MODULE_DEVICE_TABLE(of, snps_spacc_id);

static struct platform_driver spacc_driver = {
	.probe  = spacc_crypto_probe,
	.remove = spacc_crypto_remove,
	.driver = {
		.name  = "spacc",
		.of_match_table = snps_spacc_id,
	},
};

module_platform_driver(spacc_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Synopsys, Inc.");
MODULE_DESCRIPTION("SPAcc Crypto Accelerator Driver");
