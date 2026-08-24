// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>	/* udelay (polling) */
#include <linux/of.h>
#include <linux/slab.h>		/* GFP_KERNEL */
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/iosys-map.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>

#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_print.h>

/* Hardware Constants */
#define GLANDA_WIDTH      640
#define GLANDA_HEIGHT     480
#define GLANDA_VRAM_SIZE  (GLANDA_WIDTH * GLANDA_HEIGHT * 4)
#define GLANDA_MMIO_SIZE  32
#define GLANDA_MMIO_OFFSET 0x00200000

/* QEMU test device ID, from the range reserved for experimental use (docs/specs/pci-ids.rst). */
#define PCI_DEVICE_ID_GLANDA_GPU 0x10f0

/* Register Offsets */
#define REG_STATUS  0x00
#define REG_CTRL    0x04
#define REG_COORD0  0x08
#define REG_COORD1  0x0C
#define REG_COLOR   0x10
#define REG_ISR     0x14
#define REG_IER     0x18

/* Bit Masks */
#define INT_DONE    BIT(0)
#define INT_VSYNC   BIT(1)

#define STATUS_BUSY BIT(0)
#define CMD_CLEAR   (0x1)
#define CMD_RECT    (0x2)
#define CMD_LINE    (0x3)
#define CTRL_START  BIT(4)

struct glanda_device {
	struct drm_device drm;

	/* hw */
	void __iomem *mmio_base;
	void __iomem *vram_base;
	phys_addr_t vram_phys;

	int irq;

	/* drm */
	struct drm_plane primary_plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

#define to_glanda(dev) container_of(dev, struct glanda_device, drm)

static const u32 glanda_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static void glanda_plane_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_state = to_drm_shadow_plane_state(new_state);
	struct drm_framebuffer *fb = new_state->fb;
	struct glanda_device *gdev = to_glanda(plane->dev);
	u32 src_pitch, width, height, x, y;
	int idx;

	if (!fb)
		return;

	if (!drm_dev_enter(plane->dev, &idx))
		return;

	src_pitch = fb->pitches[0];
	width = min_t(u32, fb->width, GLANDA_WIDTH);
	height = min_t(u32, fb->height, GLANDA_HEIGHT);

	for (y = 0; y < height; y++) {
		size_t offset = y * GLANDA_WIDTH * sizeof(u32);
		u32 __iomem *dst = (u32 __iomem *)(gdev->vram_base + offset);

		for (x = 0; x < width; x++) {
			u32 pixel = iosys_map_rd(&shadow_state->data[0],
						 y * src_pitch + x * sizeof(u32), u32);
			pixel = le32_to_cpu((__force __le32)pixel);
			u32 packed = ((pixel >> 12) & 0x0F00) |
				((pixel >> 8) & 0x00F0) |
				((pixel >> 4) & 0x000F);

			writel_relaxed(packed, &dst[x]);
		}
	}

	drm_dev_exit(idx);
}

static int glanda_plane_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
		DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
		false,	/* can_position */
		false	/* can_update_disabled */);
}

static const struct drm_plane_helper_funcs glanda_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_update = glanda_plane_atomic_update,
	.atomic_check = glanda_plane_atomic_check,
};

static const struct drm_plane_funcs glanda_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static int glanda_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode) {
		dev_err(connector->dev->dev, "GlandaGPU: failed to create display mode\n");
		return 0;
	}

	/* Standard VGA timing: 640x480 @ 60 Hz. */
	mode->hdisplay = 640;
	mode->hsync_start = 656;
	mode->hsync_end = 752;
	mode->htotal = 800;

	mode->vdisplay = 480;
	mode->vsync_start = 490;
	mode->vsync_end = 492;
	mode->vtotal = 525;

	mode->clock = 25175;	/* 25.175 MHz pixel clock */

	mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	return 1;
}

static enum drm_connector_status glanda_connector_detect(struct drm_connector
							 *connector, bool force)
{
	return connector_status_connected;
}

static int glanda_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct glanda_device *gdev = to_glanda(crtc->dev);
	u32 ier;

	ier = readl(gdev->mmio_base + REG_IER);
	writel(ier | INT_VSYNC, gdev->mmio_base + REG_IER);

	return 0;
}

static void glanda_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct glanda_device *gdev = to_glanda(crtc->dev);
	u32 ier = readl(gdev->mmio_base + REG_IER);

	writel(ier & ~INT_VSYNC, gdev->mmio_base + REG_IER);
}

static void glanda_crtc_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_commit *state)
{
	drm_crtc_vblank_on(crtc);
}

static void glanda_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	drm_crtc_vblank_off(crtc);
}

static void glanda_crtc_atomic_flush(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct glanda_device *gdev = to_glanda(crtc->dev);
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;

	if (new_state->event) {
		event = new_state->event;
		new_state->event = NULL;

		spin_lock_irq(&crtc->dev->event_lock);

		if (gdev->irq > 0 && drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);

		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_crtc_funcs glanda_crtc_funcs = {
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = glanda_crtc_enable_vblank,
	.disable_vblank = glanda_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs glanda_crtc_helper_funcs = {
	.atomic_enable = glanda_crtc_atomic_enable,
	.atomic_disable = glanda_crtc_atomic_disable,
	.atomic_flush = glanda_crtc_atomic_flush,
};

static const struct drm_connector_helper_funcs glanda_connector_helper_funcs = {
	.get_modes = glanda_connector_get_modes,
};

static const struct drm_encoder_funcs glanda_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_funcs glanda_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.detect = glanda_connector_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs glanda_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(glanda_drm_fops);

static const struct drm_driver glanda_drm_driver = {
	.driver_features =
	    DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.name = "glandagpu",
	.desc = "GlandaGPU Hardware Accelerated DRM Driver",
	.major = 1,
	.minor = 0,
	.fops = &glanda_drm_fops,
	.dumb_create = drm_gem_shmem_dumb_create,
};

static irqreturn_t glanda_irq_handler(int irq, void *dev_id)
{
	struct glanda_device *gdev = dev_id;
	u32 isr, ier;

	if (!gdev || !gdev->mmio_base)
		return IRQ_NONE;

	isr = readl(gdev->mmio_base + REG_ISR);
	ier = readl(gdev->mmio_base + REG_IER);

	if (!(isr & ier))
		return IRQ_NONE;

	if (isr & INT_VSYNC)
		drm_crtc_handle_vblank(&gdev->crtc);

	/* Clear interrupt(W1C) */
	writel(isr, gdev->mmio_base + REG_ISR);
	return IRQ_HANDLED;
}

/* Common DRM setup once MMIO/VRAM/IRQ are known(used by both probe paths) */
static int glanda_drm_init(struct glanda_device *gdev, int irq)
{
	int ret;

	gdev->irq = -1;

	writel(0, gdev->mmio_base + REG_IER);
	writel(0xFFFFFFFF, gdev->mmio_base + REG_ISR);	/* clear flags */

	/* DRM mode config */
	ret = drm_mode_config_init(&gdev->drm);
	if (ret)
		return ret;

	gdev->drm.mode_config.min_width = 640;
	gdev->drm.mode_config.min_height = 480;
	gdev->drm.mode_config.max_width = 640;
	gdev->drm.mode_config.max_height = 480;
	gdev->drm.mode_config.funcs = &glanda_mode_config_funcs;

	ret = drm_universal_plane_init(&gdev->drm, &gdev->primary_plane, 1 << 0,
				       &glanda_plane_funcs,
				       glanda_plane_formats,
				       ARRAY_SIZE(glanda_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		drm_err(&gdev->drm, "Failed to initialize primary plane\n");
		return ret;
	}
	drm_plane_helper_add(&gdev->primary_plane, &glanda_plane_helper_funcs);

	/* VBlank init */
	ret = drm_vblank_init(&gdev->drm, 1);
	if (ret) {
		drm_err(&gdev->drm, "Failed to initialize vblank\n");
		return ret;
	}

	/* CRTC init */
	ret = drm_crtc_init_with_planes(&gdev->drm, &gdev->crtc,
					&gdev->primary_plane, NULL,
					&glanda_crtc_funcs, NULL);
	if (ret) {
		drm_err(&gdev->drm, "Failed to initialize CRTC with planes\n");
		return ret;
	}
	drm_crtc_helper_add(&gdev->crtc, &glanda_crtc_helper_funcs);

	ret = drm_encoder_init(&gdev->drm, &gdev->encoder, &glanda_encoder_funcs,
			       DRM_MODE_ENCODER_DAC, NULL);
	if (ret) {
		drm_err(&gdev->drm, "Failed to initialize encoder\n");
		return ret;
	}
	gdev->encoder.possible_crtcs = 1;

	ret = drm_connector_init(&gdev->drm, &gdev->connector,
				 &glanda_connector_funcs, DRM_MODE_CONNECTOR_VGA);
	if (ret) {
		drm_err(&gdev->drm, "Failed to initialize connector\n");
		return ret;
	}
	drm_connector_helper_add(&gdev->connector, &glanda_connector_helper_funcs);

	drm_connector_attach_encoder(&gdev->connector, &gdev->encoder);

	/* Populate connector state early so userspace can enumerate modes. */
	mutex_lock(&gdev->drm.mode_config.mutex);
	drm_helper_probe_single_connector_modes(&gdev->connector, 1024, 768);
	mutex_unlock(&gdev->drm.mode_config.mutex);

	drm_mode_config_reset(&gdev->drm);

	if (irq > 0) {
		gdev->irq = irq;
		ret = devm_request_irq(gdev->drm.dev, gdev->irq, glanda_irq_handler,
				       IRQF_SHARED, "glandagpu", gdev);
		if (ret) {
			drm_err(&gdev->drm, "Failed to request IRQ %d\n",
				gdev->irq);
			return ret;
		}

		writel(INT_VSYNC, gdev->mmio_base + REG_IER);
	} else {
		drm_warn(&gdev->drm, "No IRQ found, falling back to polling\n");
	}

	ret = drm_dev_register(&gdev->drm, 0);
	if (ret) {
		writel(0, gdev->mmio_base + REG_IER);
		return ret;
	}

	return 0;
}

/* Shared teardown, mirrors glanda_drm_init() */
static void glanda_drm_fini(struct glanda_device *gdev)
{
	drm_dev_unplug(&gdev->drm);
	drm_atomic_helper_shutdown(&gdev->drm);

	/* Disable interrupts */
	writel(0, gdev->mmio_base + REG_IER);
}

static int glandagpu_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct glanda_device *gdev;
	int irq;

	gdev = devm_drm_dev_alloc(&pdev->dev, &glanda_drm_driver, struct glanda_device, drm);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	platform_set_drvdata(pdev, gdev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	if (resource_size(res) < GLANDA_MMIO_OFFSET + GLANDA_MMIO_SIZE) {
		dev_err(&pdev->dev, "MMIO region too small: %llu bytes, need at least %u\n",
			(unsigned long long)resource_size(res),
			GLANDA_MMIO_OFFSET + GLANDA_MMIO_SIZE);
		return -EINVAL;
	}

	gdev->vram_phys = res->start;
	gdev->vram_base = devm_ioremap(&pdev->dev, res->start, GLANDA_VRAM_SIZE);
	gdev->mmio_base = devm_ioremap(&pdev->dev, res->start + GLANDA_MMIO_OFFSET,
								   GLANDA_MMIO_SIZE);
	if (!gdev->vram_base || !gdev->mmio_base) {
		drm_err(&gdev->drm, "failed to ioremap\n");
		return -ENOMEM;
	}

	irq = platform_get_irq_optional(pdev, 0);
	if (irq == -ENXIO)
		irq = -1;	/* no IRQ resource, fall back to polling */
	else if (irq < 0)
		return irq;

	return glanda_drm_init(gdev, irq);
}

static void glandagpu_remove(struct platform_device *pdev)
{
	glanda_drm_fini(platform_get_drvdata(pdev));
}

/* Device Tree match table. */
static const struct of_device_id glanda_of_match[] = {
	{ .compatible = "kieweg,gpu-1.0" },
	{ }
};

MODULE_DEVICE_TABLE(of, glanda_of_match);

static struct platform_driver glandagpu_driver = {
	.driver = {
		.name = "glandagpu",
		.of_match_table = glanda_of_match,
	},
	.probe = glandagpu_probe,
	.remove = glandagpu_remove,
};

/* PCI probe path for the QEMU test device, real hardware uses platform_driver */
static int glandagpu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct glanda_device *gdev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);

	if (pci_resource_len(pdev, 0) < GLANDA_MMIO_SIZE ||
		pci_resource_len(pdev, 1) < GLANDA_VRAM_SIZE) {
		dev_err(&pdev->dev, "BAR too small: BAR0=%llu (need %u), BAR1=%llu (need %u)\n",
			(unsigned long long)pci_resource_len(pdev, 0), GLANDA_MMIO_SIZE,
			(unsigned long long)pci_resource_len(pdev, 1), GLANDA_VRAM_SIZE);
		return -EINVAL;
	}

	ret = pcim_iomap_regions(pdev, BIT(0) | BIT(1), "glandagpu");
	if (ret)
		return ret;

	gdev = devm_drm_dev_alloc(&pdev->dev, &glanda_drm_driver, struct glanda_device, drm);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	pci_set_drvdata(pdev, gdev);

	gdev->mmio_base = pcim_iomap_table(pdev)[0];
	gdev->vram_base = pcim_iomap_table(pdev)[1];
	gdev->vram_phys = pci_resource_start(pdev, 1);

	return glanda_drm_init(gdev, pdev->irq);
}

static void glandagpu_pci_remove(struct pci_dev *pdev)
{
	glanda_drm_fini(pci_get_drvdata(pdev));
}

static const struct pci_device_id glanda_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REDHAT_QUMRANET, PCI_DEVICE_ID_GLANDA_GPU) },
	{ }
};

MODULE_DEVICE_TABLE(pci, glanda_pci_ids);

static struct pci_driver glandagpu_pci_driver = {
	.name = "glandagpu-pci",
	.id_table = glanda_pci_ids,
	.probe = glandagpu_pci_probe,
	.remove = glandagpu_pci_remove,
};

static int __init glandagpu_init(void)
{
	int ret;

	ret = platform_driver_register(&glandagpu_driver);
	if (ret) {
		pr_err("GlandaGPU: Failed to register platform driver\n");
		return ret;
	}

	ret = pci_register_driver(&glandagpu_pci_driver);
	if (ret) {
		pr_err("GlandaGPU: Failed to register PCI driver\n");
		platform_driver_unregister(&glandagpu_driver);
		return ret;
	}

	return 0;
}

static void __exit glandagpu_exit(void)
{
	pci_unregister_driver(&glandagpu_pci_driver);
	platform_driver_unregister(&glandagpu_driver);
}

module_init(glandagpu_init);
module_exit(glandagpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leander Kieweg <kieweg.leander@gmail.com>");
MODULE_DESCRIPTION("DRM driver for GlandaGPU, an FPGA-based 2D GPU with VGA output");
