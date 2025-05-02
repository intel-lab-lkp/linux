// SPDX-License-Identifier: GPL-2.0+
/*
 * AMD ISP platform driver for sensor i2-client instantiation
 *
 * Copyright 2025 Advanced Micro Devices, Inc.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/units.h>

#define AMDISP_OV05C10_I2C_ADDR		0x10
#define AMDISP_OV05C10_PLAT_NAME	"amdisp_ov05c10_platform"
#define AMDISP_OV05C10_HID		"OMNI5C10"
#define AMDISP_OV05C10_REMOTE_EP_NAME	"ov05c10_isp_4_1_1"
#define AMD_ISP_PLAT_DRV_NAME		"amd-isp4"

/*
 * AMD ISP platform definition to configure the device properties
 * missing in the ACPI table.
 */
struct amdisp_platform {
	struct i2c_board_info board_info;
	struct notifier_block i2c_nb;
	struct i2c_client *i2c_dev;
	struct mutex lock; /* protects i2c client creation */
};

/* Top-level OV05C10 camera node property table */
static const struct property_entry ov05c10_camera_props[] = {
	PROPERTY_ENTRY_U32("clock-frequency", 24 * HZ_PER_MHZ),
	{ }
};

/* Root AMD ISP OV05C10 camera node definition */
static const struct software_node camera_node = {
	.name = AMDISP_OV05C10_HID,
	.properties = ov05c10_camera_props,
};

/*
 * AMD ISP OV05C10 Ports node definition. No properties defined for
 * ports node for OV05C10.
 */
static const struct software_node ports = {
	.name = "ports",
	.parent = &camera_node,
};

/*
 * AMD ISP OV05C10 Port node definition. No properties defined for
 * port node for OV05C10.
 */
static const struct software_node port_node = {
	.name = "port@",
	.parent = &ports,
};

/*
 * Remote endpoint AMD ISP node definition. No properties defined for
 * remote endpoint node for OV05C10.
 */
static const struct software_node remote_ep_isp_node = {
	.name = AMDISP_OV05C10_REMOTE_EP_NAME,
};

/*
 * Remote endpoint reference for isp node included in the
 * OV05C10 endpoint.
 */
static const struct software_node_ref_args ov05c10_refs[] = {
	SOFTWARE_NODE_REFERENCE(&remote_ep_isp_node),
};

/* OV05C supports one single link frequency */
static const u64 ov05c10_link_freqs[] = {
	925 * HZ_PER_MHZ,
};

/* OV05C supports only 2-lane configuration */
static const u32 ov05c10_data_lanes[] = {
	1,
	2,
};

/* OV05C10 endpoint node properties table */
static const struct property_entry ov05c10_endpoint_props[] = {
	PROPERTY_ENTRY_U32("bus-type", 4),
	PROPERTY_ENTRY_U32_ARRAY_LEN("data-lanes", ov05c10_data_lanes,
				     ARRAY_SIZE(ov05c10_data_lanes)),
	PROPERTY_ENTRY_U64_ARRAY_LEN("link-frequencies", ov05c10_link_freqs,
				     ARRAY_SIZE(ov05c10_link_freqs)),
	PROPERTY_ENTRY_REF_ARRAY("remote-endpoint", ov05c10_refs),
	{ }
};

/* AMD ISP endpoint node definition */
static const struct software_node endpoint_node = {
	.name = "endpoint",
	.parent = &port_node,
	.properties = ov05c10_endpoint_props,
};

/*
 * AMD ISP swnode graph uses 5 nodes and also its relationship is
 * fixed to align with the structure that v4l2 expects for successful
 * endpoint fwnode parsing.
 *
 * It is only the node property_entries that will vary for each platform
 * supporting different sensor modules.
 */
#define NUM_SW_NODES 5

static const struct software_node *ov05c10_nodes[NUM_SW_NODES + 1] = {
	&camera_node,
	&ports,
	&port_node,
	&endpoint_node,
	&remote_ep_isp_node,
	NULL
};

static const struct acpi_device_id amdisp_sensor_ids[] = {
	{ AMDISP_OV05C10_HID },
	{ }
};
MODULE_DEVICE_TABLE(acpi, amdisp_sensor_ids);

static inline bool is_isp_i2c_adapter(struct i2c_adapter *adap)
{
	return !strcmp(adap->owner->name, "i2c_designware_amdisp");
}

static void instantiate_isp_i2c_client(struct amdisp_platform *ov05c10, struct i2c_adapter *adap)
{
	struct i2c_board_info *info = &ov05c10->board_info;
	struct i2c_client *i2c_dev;

	if (ov05c10->i2c_dev)
		return;

	if (!info->addr) {
		dev_err(&adap->dev, "invalid i2c_addr 0x%x detected\n", info->addr);
		return;
	}

	guard(mutex)(&ov05c10->lock);

	i2c_dev = i2c_new_client_device(adap, info);
	if (IS_ERR(i2c_dev)) {
		dev_err(&adap->dev, "error %pe registering isp i2c_client\n", i2c_dev);
		return;
	}
	ov05c10->i2c_dev = i2c_dev;
}

static int isp_i2c_bus_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	struct amdisp_platform *ov05c10 = container_of(nb, struct amdisp_platform, i2c_nb);
	struct device *dev = data;
	struct i2c_client *client;
	struct i2c_adapter *adap;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		adap = i2c_verify_adapter(dev);
		if (!adap)
			break;
		if (is_isp_i2c_adapter(adap))
			instantiate_isp_i2c_client(ov05c10, adap);
		break;
	case BUS_NOTIFY_REMOVED_DEVICE:
		client = i2c_verify_client(dev);
		if (!client)
			break;
		if (ov05c10->i2c_dev == client) {
			dev_dbg(&client->adapter->dev, "amdisp i2c_client removed\n");
			ov05c10->i2c_dev = NULL;
		}
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct amdisp_platform *prepare_amdisp_platform(struct device *dev)
{
	struct amdisp_platform *isp_ov05c10;
	int ret;

	isp_ov05c10 = devm_kzalloc(dev, sizeof(*isp_ov05c10), GFP_KERNEL);
	if (!isp_ov05c10)
		return ERR_PTR(-ENOMEM);

	mutex_init(&isp_ov05c10->lock);
	isp_ov05c10->board_info.dev_name = "ov05c10";
	strscpy(isp_ov05c10->board_info.type, "ov05c10", I2C_NAME_SIZE);
	isp_ov05c10->board_info.addr = AMDISP_OV05C10_I2C_ADDR;

	ret = software_node_register_node_group(ov05c10_nodes);
	if (ret) {
		mutex_destroy(&isp_ov05c10->lock);
		return ERR_PTR(ret);
	}

	isp_ov05c10->board_info.swnode = ov05c10_nodes[0];

	return isp_ov05c10;
}

static int try_to_instantiate_i2c_client(struct device *dev, void *data)
{
	struct amdisp_platform *ov05c10 = (struct amdisp_platform *)data;
	struct i2c_adapter *adap = i2c_verify_adapter(dev);

	if (!ov05c10 || !adap)
		return 0;
	if (!adap->owner)
		return 0;

	if (is_isp_i2c_adapter(adap))
		instantiate_isp_i2c_client(ov05c10, adap);

	return 0;
}

static int amd_isp_probe(struct platform_device *pdev)
{
	struct amdisp_platform *ov05c10;
	int ret;

	ov05c10 = prepare_amdisp_platform(&pdev->dev);
	if (IS_ERR(ov05c10))
		return dev_err_probe(&pdev->dev, PTR_ERR(ov05c10),
				     "failed to prepare AMD ISP platform fwnode\n");

	ov05c10->i2c_nb.notifier_call = isp_i2c_bus_notify;
	ret = bus_register_notifier(&i2c_bus_type, &ov05c10->i2c_nb);
	if (ret)
		goto error_unregister_sw_node;

	/* check if adapter is already registered and create i2c client instance */
	i2c_for_each_dev((void *)ov05c10, try_to_instantiate_i2c_client);

	platform_set_drvdata(pdev, ov05c10);
	return 0;

error_unregister_sw_node:
	software_node_unregister_node_group(ov05c10_nodes);
	mutex_destroy(&ov05c10->lock);
	return ret;
}

static void amd_isp_remove(struct platform_device *pdev)
{
	struct amdisp_platform *ov05c10 = platform_get_drvdata(pdev);

	bus_unregister_notifier(&i2c_bus_type, &ov05c10->i2c_nb);
	i2c_unregister_device(ov05c10->i2c_dev);
	software_node_unregister_node_group(ov05c10_nodes);
	mutex_destroy(&ov05c10->lock);
}

static struct platform_driver amd_isp_platform_driver = {
	.driver	= {
		.name			= AMD_ISP_PLAT_DRV_NAME,
		.acpi_match_table	= amdisp_sensor_ids,
	},
	.probe	= amd_isp_probe,
	.remove	= amd_isp_remove,
};

module_platform_driver(amd_isp_platform_driver);

MODULE_AUTHOR("Benjamin Chan <benjamin.chan@amd.com>");
MODULE_AUTHOR("Pratap Nirujogi <pratap.nirujogi@amd.com>");
MODULE_DESCRIPTION("AMD ISP4 Platform Driver");
MODULE_LICENSE("GPL");
