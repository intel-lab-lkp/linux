.. SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
============================
Kernel driver i2c-mux-regmap
============================

Author: Vadim Pasternak <vadimp@nvidia.com>

Description
-----------

i2c-mux-regmap is an i2c mux driver providing access to I2C bus segments
from a master I2C bus and a hardware MUX controlled through FPGA device
with indirect access to register space.

For example, Lattice FPGA LFD2NX-40 device, being connected through PCIe
bus provides SPI or LPC logic through PCIe-to-SPI or PCIe-to-LPC
bridging.
Thus, FPGA operates as host controller and some slave devices can be
connected to it. For example:
- CPU (PCIe) -> FPGA (PCIe-to-SPI bridge) -> CPLD or another FPGA
- CPU (PCIe) -> FPGA (PCIe-to-LPC bridge) -> CPLD or another FPGA
where 1-st FPGA connected to PCIe is located on carrier board, while 2-nd
programming logic device is located on some switch board and cannot be
connected to CPU PCIe root complex.

E.G.::
 ------------------------    ---------------------------------------
|  COME board            |  |  Switch board                         |
|                        |  |                                       |
|  -----        ------   |  |   -------     Bus channel 1           |
| |     |      |      |  |  |  |       |  *-------------->          |
| | CPU |      | FPGA |------->| CPLD  |  |                         |
| |     | PCIe |      |  LPC   |  ---  |  | Bus channel 2           |
| |     |------|      |  |  |  | |MUX|--->*-------------->  Devices |
| |     |      |      |  |  |  | |REG| |  |                         |
| |     |      |      |  |  |  |  ---  |  | Bus channel n           |
| |     |      |      |  |  |  |       |  *-------------->          |
|  -----        ------   |  |   -------                             |
|                        |  |                                       |
 ------------------------    ---------------------------------------

SCL/SDA of the master I2C bus is multiplexed to bus segment 1..n
according to the settings of the MUX REG or REGS.

Access to MUX selector registers is performed through the 'regmap' object,
which provides read and write methods, implementing protocol for indirect
access.

Usage
-----

i2c-mux-regmap uses the platform bus, so it is necessary to provide a struct
platform_device with the platform_data pointing to a struct
i2c_mux_regmap_platform_data with:
- The I2C adapter number of the master bus.
- Channels array and the number of bus channels to create.
- MUX select register offset in programming logic device space for bus
  selection/deselection control.
- Optional callback to notify caller when all the adapters are created and
  handle to be passed to callback.
See include/linux/platform_data/i2c-mux-regmap.h for details.

Device Registration example
---------------------------

   /* Channels vector for regmap mux. */
   static int regmap_mux_chan[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

   /* Platform regmap mux data */
   static struct i2c_mux_regmap_platform_data regmap_mux_data[] = {
	{
		.parent = 1,
		.chan_ids = regmap_mux_chan,
		.num_adaps = ARRAY_SIZE(regmap_mux_chan),
		.sel_reg_addr = 0xdb,
	},
	{
		.parent = 1,
		.chan_ids = regmap_mux_chan,
		.num_adaps = ARRAY_SIZE(regmap_mux_chan),
		.sel_reg_addr = 0xda,
	},
   };

  Create regmap object.

  struct caller_regmap_context {
	void __iomem *base;
  };

  /* Read callback for indirect register map access */
  static int fpga_reg_read(void *context, unsigned int reg, unsigned int *val)
  {
	/* Verify there is no pending transactions */
	/* Set address in register space */
	/* Activate read operation */
	/* Verify transaction completion */
	/* Read data */
  }

  /* Write callback for indirect register map access */
  static int reg_write(void *context, unsigned int reg, unsigned int val)
  {
	/* Verify there is no pending transactions */
	/* Set address in register space */
	/* Set data to be written */
	/* Activate write operation */
	/* Verify transaction completion */
  }

  static struct caller_regmap_context caller_regmap_ctx;

  static const struct regmap_config fpga_regmap_config = {
	.reg_bits = 9,
	.val_bits = 8,
	.max_register = 511,
	.cache_type = REGCACHE_FLAT,
	.writeable_reg = caller_writeable_reg,
	.readable_reg = caller_readable_reg,
	.volatile_reg = caller_volatile_reg,
	.reg_defaults = caller_regmap_default,
	.num_reg_defaults = ARRAY_SIZE(caller_regmap_default),
	/* Methods implementing ptotocol to access PCI-LPC bridge. */
	.reg_read = fpga_reg_read,
	.reg_write = fpga_reg_write,
  };

  regmap = devm_regmap_init(&dev, NULL, &caller_regmap_ctx,
			    fpga_regmap_config);

  Remap FPGA base address.

  caller_regmap_ctx.base = devm_ioremap(&fpga_pci_dev->dev,
					pci_resource_start(pci_dev, 0),
					pci_resource_len(pci_dev, 0));

  For each entry in 'regmap_mux_data' array.

  mux_regmap_data[i].handle = caller_handle;
  mux_regmap_data[i].regmap = regmap;
  mux_regmap_data[i].completion_notify = caller_complition_notify;

  pdev[i] =
  platform_device_register_resndata(dev, "i2c-mux-regmap", i, NULL, 0,
				    &regmap_mux_data[i],
				    sizeof(regmap_mux_data[i]));

  In the above examples two instances of "i2c-mux-regmap" will be created.
  For each the array of the created adapter will be passed to
  caller_complition_notify(), if this callback was provided.

SYSFS example
=============
  In 'sysfs' the channels will be exposed through path:
  /sys/devices/platform/<caller-driver>/<i2c-parent-driver.parent-bus>
  Following the above example it will contain:
  i2c-mux-regmap.0/channel-1
  ...
  i2c-mux-regmap.0/channel-8
  i2c-mux-regmap.1/channel-1
  ...
  i2c-mux-regmap.1/channel-8

  However, MUX number of each 'i2c-mux-regmap' instance is limited by
  the size of selector register.
  Thus, if its size is 1 byte - up to 255 MUX channels can be created,
  for 2 bytes respectively up to 65535.

