.. SPDX-License-Identifier: GPL-2.0

==========
spi-mockup
==========

Description
===========

This module is a very simple fake SPI controller driver. It implements
a BPF based interface to mockup SPI device.

No hardware is needed nor associated with this module. It will respond
spi message by BPF program attached to spi_transfer_writeable tracepoint
by reading from or writing BPF maps.

The typical use-case is like this:
        1. create the spi mockup controller;
        2. load EBPF program as device's backend;
        3. create target chip device to the spi mockup controller.

Example
=======

This example show how to mock a MTD device by using spi-mockup driver.

Compile your copy of the kernel source. Make sure to configure the spi-mockup
and the target chip driver as a module.

Register the spi mockup controller.

::

  $ mkdir /sys/kernel/config/spi-mockup/spi0

  # configure the spi mockup controller attribute
  $ echo 40000 > /sys/kernel/config/spi-mockup/spi0/min_speed
  $ echo 25000000 > /sys/kernel/config/spi-mockup/spi0/max_speed
  $ echo 0 > /sys/kernel/config/spi-mockup/spi0/flags
  $ echo 8 > /sys/kernel/config/spi-mockup/spi0/num_cs

  # enable the spi mockup controller
  $ echo 1 > /sys/kernel/config/spi-mockup/spi0/live

Write a BPF program as device's backup.

::

  #define MCHP23K256_CMD_WRITE_STATUS   0x01
  #define MCHP23K256_CMD_WRITE          0x02
  #define MCHP23K256_CMD_READ           0x03

  #define CHIP_REGS_SIZE		0x20000

  #define MAX_CMD_SIZE		        4

  struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, CHIP_REGS_SIZE);
	__type(key, __u32);
	__type(value, __u8);
  } regs_mchp23k256 SEC(".maps");

  static unsigned int chip_reg = 0;

  static int spi_transfer_read(struct spi_msg_ctx *msg, unsigned int len)
  {
	int i, key;
	u8 *reg;

	for (i = 0; i < len && i < sizeof(msg->data); i++) {
		key = i + chip_reg;

		reg = bpf_map_lookup_elem(&regs_mchp23k256, &key);
		if (!reg) {
			bpf_printk("key %d not exists", key);
			return -EINVAL;
		}

		msg->data[i] = *reg;
	}

	return 0;
  }

  static int spi_transfer_write(struct spi_msg_ctx *msg, unsigned int len)
  {
	u8 opcode = msg->data[0], value;
	int i, key;

	switch (opcode) {
	case MCHP23K256_CMD_READ:
	case MCHP23K256_CMD_WRITE:
		if (len < 2)
			return -EINVAL;

		chip_reg = 0;
		for (i = 0; i < MAX_CMD_SIZE && i < len - 1; i++)
			chip_reg = (chip_reg << 8) + msg->data[1 + i];

		return 0;
	case MCHP23K256_CMD_WRITE_STATUS:
		// ignore write status
		return 0;
	default:
		break;
	}

	for (i = 0; i < len && i < sizeof(msg->data); i++) {
		value = msg->data[i];
		key = chip_reg + i;

		if (bpf_map_update_elem(&regs_mchp23k256, &key, &value,
					BPF_EXIST)) {
			bpf_printk("key %d not exists", key);
			return -EINVAL;
		}
	}

	return 0;
  }

  SEC("raw_tp.w/spi_transfer_writeable")
  int BPF_PROG(mtd_mchp23k256, struct spi_msg_ctx *msg, u8 chip, unsigned int len)
  {
	int ret = 0;

	if (msg->tx_nbits)
		ret = spi_transfer_write(msg, len);
	else if (msg->rx_nbits)
		ret = spi_transfer_read(msg, len);

	return ret;
  }

  char LICENSE[] SEC("license") = "GPL";

Use bpftool to load the BPF program.

::

  $ bpftool prog load mtd-mchp23k256.o /sys/fs/bpf/mtd_mchp23k256 autoattach

Create the mchp23k256 device, this is accomplished by executing the
following command:

::

  $ mkdir /sys/kernel/config/spi-mockup/spi0/targets/mchp23k256
  $ echo -n mchp23k256 > /sys/kernel/config/spi-mockup/spi0/targets/mchp23k256/device_id
  $ echo 1 > /sys/kernel/config/spi-mockup/spi0/targets/mchp23k256/live


Now, the mchp23k256 MTD device named /dev/mtd0 has been created successfully.

::

  $ ls /sys/bus/spi/devices/spi0.0/mtd/
  mtd0  mtd0ro

  $ cat /sys/class/mtd/mtd0/name
  spi0.0

  $ hexdump /dev/mtd0
  0000000 0000 0000 0000 0000 0000 0000 0000 0000
  *
  0008000

  $ echo aaaa > /dev/mtd0

  $ hexdump /dev/mtd0
  00000000  61 61 61 61 0a 00 00 00  00 00 00 00 00 00 00 00  |aaaa............|
  00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
  *
  00008000

  $ bpftool map update name regs_mchp23k256 key 0 0 0 0 value 0

  $ hexdump /dev/mtd0
  00000000  00 61 61 61 0a 00 00 00  00 00 00 00 00 00 00 00  |.aaa............|
  00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
  *
  00008000


Remove the mchp23k256 device by executing the following command:

::

  $ echo 0 > /sys/kernel/config/spi-mockup/spi0/targets/mchp23k256/live
  $ rmdir /sys/kernel/config/spi-mockup/spi0/targets/mchp23k256

Remove the spi mockup controller by executing the following command:

::

  $ echo 0 > /sys/kernel/config/spi-mockup/spi0/live
  $ rmdir /sys/kernel/config/spi-mockup/spi0
