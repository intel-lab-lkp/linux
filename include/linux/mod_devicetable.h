/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Device tables which are exported to userspace via
 * scripts/mod/file2alias.c.  You must keep that file in sync with this
 * header.
 */

#ifndef LINUX_MOD_DEVICETABLE_H
#define LINUX_MOD_DEVICETABLE_H

#ifdef __KERNEL__
#include <linux/types.h>
#endif

#include "mod_device_id/acpi.h"
#include "mod_device_id/amba.h"
#include "mod_device_id/ap.h"
#include "mod_device_id/apr.h"
#include "mod_device_id/auxiliary.h"
#include "mod_device_id/bcma.h"
#include "mod_device_id/ccw.h"
#include "mod_device_id/cdx.h"
#include "mod_device_id/coreboot.h"
#include "mod_device_id/css.h"
#include "mod_device_id/dfl.h"
#include "mod_device_id/dmi.h"
#include "mod_device_id/eisa.h"
#include "mod_device_id/fsl_mc.h"
#include "mod_device_id/hda.h"
#include "mod_device_id/hid.h"
#include "mod_device_id/hv_vmbus.h"
#include "mod_device_id/i2c.h"
#include "mod_device_id/i3c.h"
#include "mod_device_id/ieee1394.h"
#include "mod_device_id/input.h"
#include "mod_device_id/ipack.h"
#include "mod_device_id/isapnp.h"
#include "mod_device_id/ishtp.h"
#include "mod_device_id/mcb.h"
#include "mod_device_id/mdio.h"
#include "mod_device_id/mei_cl.h"
#include "mod_device_id/mhi.h"
#include "mod_device_id/mips_cdmm.h"
#include "mod_device_id/of.h"
#include "mod_device_id/parisc.h"
#include "mod_device_id/pci.h"
#include "mod_device_id/pcmcia.h"
#include "mod_device_id/platform.h"
#include "mod_device_id/pnp.h"
#include "mod_device_id/rio.h"
#include "mod_device_id/rpmsg.h"
#include "mod_device_id/sdio.h"
#include "mod_device_id/sdw.h"
#include "mod_device_id/serio.h"
#include "mod_device_id/slim.h"
#include "mod_device_id/spi.h"
#include "mod_device_id/spmi.h"
#include "mod_device_id/ssam.h"
#include "mod_device_id/ssb.h"
#include "mod_device_id/tb.h"
#include "mod_device_id/tee.h"
#include "mod_device_id/typec.h"
#include "mod_device_id/ulpi.h"
#include "mod_device_id/usb.h"
#include "mod_device_id/vchiq.h"
#include "mod_device_id/vio.h"
#include "mod_device_id/virtio.h"
#include "mod_device_id/wmi.h"
#include "mod_device_id/x86_cpu.h"
#include "mod_device_id/zorro.h"

/*
 * Generic table type for matching CPU features.
 * @feature:	the bit number of the feature (0 - 65535)
 */

struct cpu_feature {
	__u16	feature;
};

#endif /* LINUX_MOD_DEVICETABLE_H */
