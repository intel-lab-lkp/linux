// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright (C) 2026 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>

#define NPAC_RPMSG_SERVICE_NAME	"ti.npac"
#define NPAC_ACK_TIMEOUT_MS	1000

/**
 * enum npac_acl_cmd - IOCTL command numbers for NPAC ACL operations.
 */
enum npac_acl_cmd {
	/** @NPAC_ACL_ADD_RULE: Add an ACL rule. */
	NPAC_ACL_ADD_RULE,

	/** @NPAC_ACL_DELETE_RULE: Delete an ACL rule. */
	NPAC_ACL_DELETE_RULE,
};

/**
 * enum npac_acl_match_type - Match field selector for an ACL rule.
 */
enum npac_acl_match_type {
	/** @NPAC_ACL_MATCH_SRC_IPV4: Match on source IPv4 address. */
	NPAC_ACL_MATCH_SRC_IPV4 = 1,

	/** @NPAC_ACL_MATCH_DST_IPV4: Match on destination IPv4 address. */
	NPAC_ACL_MATCH_DST_IPV4,

	/** @NPAC_ACL_MATCH_SRC_MAC: Match on source MAC address. */
	NPAC_ACL_MATCH_SRC_MAC,

	/** @NPAC_ACL_MATCH_DST_MAC: Match on destination MAC address. */
	NPAC_ACL_MATCH_DST_MAC,
};

/**
 * enum npac_acl_verdict - Action to apply when an ACL rule matches.
 */
enum npac_acl_verdict {
	/** @NPAC_ACL_VERDICT_ALLOW: Accept the matched packet. */
	NPAC_ACL_VERDICT_ALLOW = 1,

	/** @NPAC_ACL_VERDICT_DROP: Drop the matched packet. */
	NPAC_ACL_VERDICT_DROP,
};

/* Sized for the largest match field: a 6-byte MAC address. */
#define NPAC_ACL_MATCH_VALUE_MAX	6

struct npac_acl_offload_rule {
	__u32 match_type;
	__u32 verdict;
	__u8  match_value[NPAC_ACL_MATCH_VALUE_MAX];
};

#define DRM_IOCTL_NPAC_ACL_ADD_RULE \
	DRM_IOWR(DRM_COMMAND_BASE + NPAC_ACL_ADD_RULE, struct npac_acl_offload_rule)

#define DRM_IOCTL_NPAC_ACL_DELETE_RULE \
	DRM_IOWR(DRM_COMMAND_BASE + NPAC_ACL_DELETE_RULE, struct npac_acl_offload_rule)

struct npac_device {
	struct drm_device drm;
	struct rpmsg_device *rpdev;
	struct mutex req_lock; /* lock for request */
	struct completion ack;
};

static inline struct npac_device *drm_to_npac_device(struct drm_device *dev)
{
	return container_of(dev, struct npac_device, drm);
}

static int npac_add_rule_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct npac_device *npac = drm_to_npac_device(dev);
	struct npac_acl_offload_rule *rule = data;
	int ret;

	dev_dbg(dev->dev, "add rule ioctl received\n");

	mutex_lock(&npac->req_lock);
	reinit_completion(&npac->ack);
	ret = rpmsg_send(npac->rpdev->ept, rule, sizeof(*rule));
	if (ret) {
		dev_err(dev->dev, "rpmsg_send failed: %d\n", ret);
		mutex_unlock(&npac->req_lock);
		return ret;
	}
	if (!wait_for_completion_timeout(&npac->ack,
					 msecs_to_jiffies(NPAC_ACK_TIMEOUT_MS))) {
		dev_err(dev->dev, "ack timed out\n");
		mutex_unlock(&npac->req_lock);
		return -ETIMEDOUT;
	}
	mutex_unlock(&npac->req_lock);
	return 0;
}

static int npac_delete_rule_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct npac_device *npac = drm_to_npac_device(dev);
	struct npac_acl_offload_rule *rule = data;
	int ret;

	dev_dbg(dev->dev, "delete rule ioctl received\n");

	mutex_lock(&npac->req_lock);
	reinit_completion(&npac->ack);
	ret = rpmsg_send(npac->rpdev->ept, rule, sizeof(*rule));
	if (ret) {
		dev_err(dev->dev, "rpmsg_send failed: %d\n", ret);
		mutex_unlock(&npac->req_lock);
		return ret;
	}
	if (!wait_for_completion_timeout(&npac->ack,
					 msecs_to_jiffies(NPAC_ACK_TIMEOUT_MS))) {
		dev_err(dev->dev, "ack timed out\n");
		mutex_unlock(&npac->req_lock);
		return -ETIMEDOUT;
	}
	mutex_unlock(&npac->req_lock);
	return 0;
}

static const struct drm_ioctl_desc npac_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NPAC_ACL_ADD_RULE,    npac_add_rule_ioctl,    DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(NPAC_ACL_DELETE_RULE, npac_delete_rule_ioctl, DRM_ROOT_ONLY),
};

DEFINE_DRM_ACCEL_FOPS(npac_fops);

static const struct drm_driver npac_drm_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL,
	.ioctls			= npac_drm_ioctls,
	.num_ioctls		= ARRAY_SIZE(npac_drm_ioctls),
	.fops			= &npac_fops,
	.name			= "npac",
	.desc			= "NPAC Client Driver",
};

static int npac_cb(struct rpmsg_device *rpdev, void *data, int len,
		   void *priv, u32 src)
{
	struct npac_device *npac = dev_get_drvdata(&rpdev->dev);

	dev_dbg(&rpdev->dev, "ack received\n");
	complete(&npac->ack);
	return 0;
}

static int npac_probe(struct rpmsg_device *rpdev)
{
	struct npac_device *npac;
	int ret;

	npac = devm_drm_dev_alloc(&rpdev->dev, &npac_drm_driver,
				  struct npac_device, drm);
	if (IS_ERR(npac))
		return PTR_ERR(npac);

	npac->rpdev = rpdev;
	mutex_init(&npac->req_lock);
	init_completion(&npac->ack);
	dev_set_drvdata(&rpdev->dev, npac);

	ret = drm_dev_register(&npac->drm, 0);
	if (ret) {
		dev_err(&rpdev->dev, "Failed to register DRM accel device: %d\n", ret);
		return ret;
	}

	dev_info(&rpdev->dev, "NPAC Client driver probed\n");
	return 0;
}

static void npac_remove(struct rpmsg_device *rpdev)
{
	struct npac_device *npac = dev_get_drvdata(&rpdev->dev);

	drm_dev_unplug(&npac->drm);

	dev_info(&rpdev->dev, "NPAC Client Driver removed\n");
}

static struct rpmsg_device_id npac_id_table[] = {
	{ .name = NPAC_RPMSG_SERVICE_NAME },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, npac_id_table);

static struct rpmsg_driver npac_driver = {
	.drv.name	= KBUILD_MODNAME,
	.id_table	= npac_id_table,
	.probe		= npac_probe,
	.callback	= npac_cb,
	.remove		= npac_remove,
};

module_rpmsg_driver(npac_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NPAC Client Driver");
MODULE_AUTHOR("Jaspinder Budhal <j-budhal@ti.com>");
