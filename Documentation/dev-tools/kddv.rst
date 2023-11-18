.. SPDX-License-Identifier: GPL-2.0
.. Copyright(c) 2022 Huawei Technologies Co., Ltd.

Kernel Device Driver Verification (KDDV)
========================================

The kernel code contains a large amount of driver code which
depends on specific hardware. As a result, the test of this part
of code is difficult. Due to resource limitations, common detection
methods such as KASAN are difficult to deploy. Therefore, a test
method independent of physical hardware is required.

Generally, the driver reads related information from the physical
hardware by calling a specific function. Then the driver code can
be tested by mocking the specific function to simulate physical
hardware behavior by returning virtual values.

With the development of eBPF, it is possible to mock a specific
function based on the eBPF TracePoint mechanism, and then use
eBPF to simulate physical hardware.

Based on the python unittests framework, KDDV mounts devices to
virtual buses and simulates hardware behaviors through the ebpf
program to test specific driver functions. In addition, KDDV supports
fault injection to simulate exceptions, verify that the driver
behaves properly.


Design principles
-----------------

The driver can register these device to the mock bus, it can be interacts
with bpf program, then the physical hardware can be mocked by bpf.

::

    __________________________________       _______________________________
    |User Program                    |       | Linux kernel                |
    |                                |       |                             |
    |                                |       |                             |
    |            _________________   | load  |    _____________      _____ |
    |llvm -----> | eBPF bytecode |  -|-------|--> | Verifier  |      | M | |
    |            ¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯   |       |    ¯¯¯¯¯¯¯¯¯¯¯¯¯      | O | |
    |                                |       |                       | C | |
    |                                |       |    _____________      | K | |
    |                                |       |    |   eBPF    | <--> |   | |
    |                                |       |    ¯¯¯¯¯¯¯¯¯¯¯¯¯      | B | |
    |            _______________     | read  |    _____________      | U | |
    |bpftool --> | Access Regs | ----|-------|--> | eBPF Maps |      | S | |
    |            ¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯     | write |    ¯¯¯¯¯¯¯¯¯¯¯¯¯      ¯¯¯¯¯ |
    ¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯       ¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯¯



Running the KDDV
----------------

The kddv depends on the mock bus, should configure kernel as below::

    CONFIG_SPI_MOCK=y
    ...

To build the tests::

  $ make -C tools/testing/kddv/

To install kddv in a user specified location::

   $ make -C tools/testing/selftests install INSTALL_PATH=/some/other/path

To run the tests::

  $ cd /some/other/path
  $ python3 -m kddv.cmds.test

More useful info can be got from the help::

  $ python3 -m kddv.cmds.test --help


Contributing new tests
----------------------

Test file placement
~~~~~~~~~~~~~~~~~~~

The directory structure is as below::

        tools/testing/kddv/kddv
        ├── cmds  # Python unittests command line
        ├── core  # General data structure of the kddv
        ├── data  # Extra data, eg: bpf program
        │   ├── bpf
        │   │   ├── include
        │   │   │   └── bpf-xfer-conf.h
        │   │   ├── Makefile
        │   │   ├── mtd
        │   │   │   └── mtd-mchp23k256.c
        │   │   └── spi
        │   └── Makefile
        ├── tests  # KDDV testcases
        │   ├── __init__.py
        │   ├── hwmon
        │   │   ├── __init__.py
        │   │   └── test_max31722.py
        │   └── mtd
        │       ├── __init__.py
        │       └── test_mchp23k256.py
        ├── __init__.py
        └── Makefile

If you want to add a new testcase to kddv, you should:
  1. add the python unittests to the **tests** directory.
  2. add the bpf program to the **data/bpf** directory.

Basic example
~~~~~~~~~~~~~

The kddv based on python unittests, it provides a set of tools for write
the testcase, here is a basic example for test some feature of mchp23k256
mtd device::

  from kddv.core import SPIDriverTest
  from . import MTDDriver

  MCHP23K256_TEST_DATA = [0x78] * 16

  class TestMCHP23K256(SPIDriverTest, MTDDriver):
      name = "mchp23k256"

      @property
      def bpf(self):
          return f"mtd-{self.name}"

      def test_device_probe(self):
          with self.assertRaisesFault():
              with self.device() as _:
                  pass

      def test_device_size(self):
          with self.device() as dev:
              size = self.mtd_read_attr(dev, 'size')
              self.assertEqual(size, '32768')

      def test_read_data(self):
          with self.device() as dev:
              self.write_regs(0x00, MCHP23K256_TEST_DATA)
              data = self.mtd_read_bytes(dev, 16)
              self.assertEqual(data, bytes(MCHP23K256_TEST_DATA))

      def test_write_data(self):
          with self.device() as dev:
              self.write_regs(0x00, [0] * 16)
              self.mtd_write_bytes(dev, bytes(MCHP23K256_TEST_DATA))
              self.assertRegsEqual(0x00, MCHP23K256_TEST_DATA)

**SPIDriverTest** provides the basic functions for SPI driver, such as
loading the driver, read/write SPI registers.

**MTDDriver** provides some basic function for MTD driver, such as get the
mtd device info, read/write data from mtd device.

Since the mchp23k256 is a mtd driver based on SPI bus, so **TestMCHP23K256**
inherited from **SPIDriverTest** and **MTDDriver**.

There are 4 testcases for mchp23k256 basic function:
  1. driver and device can be loading successfully;
  2. the device size should be 32k as the default;
  3. read data from device, the data should be equal with the regs;
  4. write data to device, the regs should be equal with the data;

The above script produces an output that looks like this::

  $ python3 -m kddv.cmds.test kddv.tests.mtd.test_mchp23k256
  test_device_probe (kddv.tests.mtd.test_mchp23k256.TestMCHP23K256) ... ok
  test_device_size (kddv.tests.mtd.test_mchp23k256.TestMCHP23K256) ... ok
  test_read_data (kddv.tests.mtd.test_mchp23k256.TestMCHP23K256) ... ok
  test_write_data (kddv.tests.mtd.test_mchp23k256.TestMCHP23K256) ... ok

  ----------------------------------------------------------------------
  Ran 4 tests in 36.100s

  OK
