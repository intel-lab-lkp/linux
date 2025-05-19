/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _FPGA_REGION_H
#define _FPGA_REGION_H

#include <linux/configfs.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga-region.h>

struct fpga_region;

/**
 * struct fpga_region_ops - ops for low level FPGA region ops for device
 * enumeration/removal
 * @region_status: Check current status of the FPGA region.
 * @region_pre_config: Called before starting FPGA configuration.
 * @region_post_config: Called after FPGA configuration is done.
 * @region_remove: Called to remove devices added during configuration.
 */
struct fpga_region_ops {
	int (*region_status)(struct fpga_region *region);
	int (*region_pre_config)(struct fpga_region *region);
	int (*region_post_config)(struct fpga_region *region);
	int (*region_remove)(struct fpga_region *region);
};

/**
 * struct fpga_region_info - collection of parameters an FPGA Region
 * @mgr: fpga region manager
 * @compat_id: FPGA region id for compatibility check.
 * @priv: fpga region private data
 * @get_bridges: optional function to get bridges to a list
 * @fpga_region_ops: ops for low level FPGA region ops for device
 * enumeration/removal
 *
 * fpga_region_info contains parameters for the register_full function.
 * These are separated into an info structure because they some are optional
 * others could be added to in the future. The info structure facilitates
 * maintaining a stable API.
 */
struct fpga_region_info {
	struct fpga_manager *mgr;
	struct fpga_compat_id *compat_id;
	void *priv;
	int (*get_bridges)(struct fpga_region *region);
	const struct fpga_region_ops *region_ops;
};

/**
 * struct fpga_region - FPGA Region structure
 * @dev: FPGA Region device
 * @mutex: enforces exclusive reference to region
 * @bridge_list: list of FPGA bridges specified in region
 * @mgr: FPGA manager
 * @info: FPGA image info
 * @compat_id: FPGA region id for compatibility check.
 * @ops_owner: module containing the get_bridges function
 * @priv: private data
 * @get_bridges: optional function to get bridges to a list
 * @fpga_region_ops: ops for low level FPGA region ops for device
 * enumeration/removal.
 * @subsys: Configfs subsystem for exposing region configuration.
 * @firmware_name: Firmware name set via configfs for image loading.
 */
struct fpga_region {
	struct device dev;
	struct mutex mutex; /* for exclusive reference to region */
	struct list_head bridge_list;
	struct fpga_manager *mgr;
	struct fpga_image_info *info;
	struct fpga_compat_id *compat_id;
	struct module *ops_owner;
	void *priv;
	int (*get_bridges)(struct fpga_region *region);
	const struct fpga_region_ops *region_ops;
	struct configfs_subsystem subsys;
	char *firmware_name;
};

#define to_fpga_region(d) container_of(d, struct fpga_region, dev)

struct fpga_region *
fpga_region_class_find(struct device *start, const void *data,
		       int (*match)(struct device *, const void *));

int fpga_region_program_fpga(struct fpga_region *region);

#define fpga_region_register_full(parent, info) \
	__fpga_region_register_full(parent, info, THIS_MODULE)
struct fpga_region *
__fpga_region_register_full(struct device *parent, const struct fpga_region_info *info,
			    struct module *owner);

#define fpga_region_register(parent, mgr, get_bridges) \
	__fpga_region_register(parent, mgr, get_bridges, THIS_MODULE)
struct fpga_region *
__fpga_region_register(struct device *parent, struct fpga_manager *mgr,
		       int (*get_bridges)(struct fpga_region *), struct module *owner);
#define fpga_region_register_with_ops(parent, mgr, region_ops, priv, get_bridges) \
	__fpga_region_register_with_ops(parent, mgr, region_ops, priv, get_bridges, THIS_MODULE)
struct fpga_region *
__fpga_region_register_with_ops(struct device *parent, struct fpga_manager *mgr,
				const struct fpga_region_ops *region_ops, void *priv,
				int (*get_bridges)(struct fpga_region *),
				struct module *owner);
void fpga_region_unregister(struct fpga_region *region);

#endif /* _FPGA_REGION_H */
