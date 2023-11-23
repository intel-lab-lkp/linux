=================
SPI NOR framework
=================

How to propose a new flash addition
-----------------------------------

Most SPI NOR flashes comply with the JEDEC JESD216
Serial Flash Discoverable Parameter (SFDP) standard. SFDP describes
the functional and feature capabilities of serial flash devices in a
standard set of internal read-only parameter tables.

The SPI NOR driver queries the SFDP tables in order to determine the
flash's parameters and settings. If the flash defines the SFDP tables
it's likely that you won't need a flash entry at all, and instead
rely on the generic flash driver which probes the flash solely based
on its SFDP data. All one has to do is to specify the "jedec,spi-nor"
compatible in the device tree.

There are cases however where you need to define an explicit flash
entry. This typically happens when the flash has settings or support
that is not covered by the SFDP tables (e.g. Block Protection), or
when the flash contains mangled SFDP data. If the later, one needs
to implement the ``spi_nor_fixups`` hooks in order to amend the SFDP
parameters with the correct values.

Minimum testing requirements
-----------------------------

Do all the tests from below and paste them in the commit's comments
section, after the ``---`` marker.

1/ Specify the controller that you used to test the flash and specify
the frequency at which the flash was operated::

    E.g. This flash is populated on the X board and was tested at Y
    frequency using the Z (put compatible) SPI controller.

2/ Dump the sysfs entries and print the md5/sha1/sha256 SFDP checksum::

    root@1:~#  cat /sys/bus/spi/devices/spi1.0/spi-nor/partname
    sst26vf064b
    root@1:~#  cat /sys/bus/spi/devices/spi1.0/spi-nor/jedec_id
    bf2643
    root@1:~#  cat /sys/bus/spi/devices/spi1.0/spi-nor/manufacturer
    sst
    root@1:~# hexdump -C  /sys/bus/spi/devices/spi1.0/spi-nor/sfdp
    00000000  53 46 44 50 06 01 02 ff  00 06 01 10 30 00 00 ff  |SFDP........0...|
    00000010  81 00 01 06 00 01 00 ff  bf 00 01 18 00 02 00 01  |................|
    00000020  ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
    00000030  fd 20 f1 ff ff ff ff 03  44 eb 08 6b 08 3b 80 bb  |. ......D..k.;..|
    00000040  fe ff ff ff ff ff 00 ff  ff ff 44 0b 0c 20 0d d8  |..........D.. ..|
    00000050  0f d8 10 d8 20 91 48 24  80 6f 1d 81 ed 0f 77 38  |.... .H$.o....w8|
    00000060  30 b0 30 b0 f7 ff ff ff  29 c2 5c ff f0 30 c0 80  |0.0.....).\..0..|
    00000070  ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
    *
    00000100  ff 00 04 ff f3 7f 00 00  f5 7f 00 00 f9 ff 7d 00  |..............}.|
    00000110  f5 7f 00 00 f3 7f 00 00  ff ff ff ff ff ff ff ff  |................|
    00000120  ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
    *
    00000200  bf 26 43 ff b9 5f fd ff  30 f2 60 f3 32 ff 0a 12  |.&C.._..0.`.2...|
    00000210  23 46 ff 0f 19 32 0f 19  19 ff ff ff ff ff ff ff  |#F...2..........|
    00000220  00 66 99 38 ff 05 01 35  06 04 02 32 b0 30 72 42  |.f.8...5...2.0rB|
    00000230  8d e8 98 88 a5 85 c0 9f  af 5a ff ff 06 ec 06 0c  |.........Z......|
    00000240  00 03 08 0b ff ff ff ff  ff 07 ff ff 02 02 ff 06  |................|
    00000250  03 00 fd fd 04 07 00 fc  03 00 fe fe 02 02 07 0e  |................|
    00000260
    root@1:~# sha256sum /sys/bus/spi/devices/spi1.0/spi-nor/sfdp
    428f34d0461876f189ac97f93e68a05fa6428c6650b3b7baf736a921e5898ed1  /sys/bus/spi/devices/spi1.0/spi-nor/sfdp

3/ Dump debugfs data::

    root@1:~# cat /sys/kernel/debug/spi-nor/spi1.0/capabilities
    Supported read modes by the flash
     1S-1S-1S
      opcode		0x03
      mode cycles	0
      dummy cycles	0
     1S-1S-1S (fast read)
      opcode		0x0b
      mode cycles	0
      dummy cycles	8
     1S-1S-2S
      opcode		0x3b
      mode cycles	0
      dummy cycles	8
     1S-2S-2S
      opcode		0xbb
      mode cycles	4
      dummy cycles	0
     1S-1S-4S
      opcode		0x6b
      mode cycles	0
      dummy cycles	8
     1S-4S-4S
      opcode		0xeb
      mode cycles	2
      dummy cycles	4
     4S-4S-4S
      opcode		0x0b
      mode cycles	2
      dummy cycles	4
    
    Supported page program modes by the flash
     1S-1S-1S
      opcode	0x02
    
    root@1:~# cat /sys/kernel/debug/spi-nor/spi1.0/params
    name		sst26vf064b
    id			bf 26 43 bf 26 43
    size		8.00 MiB
    write size		1
    page size		256
    address nbytes	3
    flags		HAS_LOCK | HAS_16BIT_SR | SOFT_RESET | SWP_IS_VOLATILE
    
    opcodes
     read		0xeb
      dummy cycles	6
     erase		0x20
     program		0x02
     8D extension	none
    
    protocols
     read		1S-4S-4S
     write		1S-1S-1S
     register		1S-1S-1S
    
    erase commands
     20 (4.00 KiB) [0]
     d8 (8.00 KiB) [1]
     d8 (32.0 KiB) [2]
     d8 (64.0 KiB) [3]
     c7 (8.00 MiB)
    
    sector map
     region (in hex)   | erase mask | flags
     ------------------+------------+----------
     00000000-00007fff |     [01  ] |
     00008000-0000ffff |     [0 2 ] |
     00010000-007effff |     [0  3] |
     007f0000-007f7fff |     [0 2 ] |
     007f8000-007fffff |     [01  ] |

4/ Use `mtd-utils <https://git.infradead.org/mtd-utils.git>`__
and verify that erase, read and page program operations work fine.

a/ Generate a 2 MB file::

    root@1:~# dd if=/dev/urandom of=./spi_test bs=1M count=2
    2+0 records in
    2+0 records out
    2097152 bytes (2.1 MB, 2.0 MiB) copied, 0.848566 s, 2.5 MB/s

b/ Verify erase::

    root@1:~# mtd_debug write /dev/mtd1 0 2097152 spi_test
    Copied 2097152 bytes from spi_test to address 0x00000000 in flash

    root@1:~# mtd_debug erase /dev/mtd1 0 2097152
    Erased 2097152 bytes from address 0x00000000 in flash

    root@1:~# mtd_debug read /dev/mtd1 0 2097152 spi_read
    Copied 2097152 bytes from address 0x00000000 in flash to spi_read

    root@1:~# hexdump -C spi_read
    00000000  ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
    *
    00200000

c/ Write, read back and compare::

    root@1:~# mtd_debug write /dev/mtd1 0 2097152 spi_test
    Copied 2097152 bytes from spi_test to address 0x00000000 in flash

    root@1:~# mtd_debug read /dev/mtd1 0 2097152 spi_read
    Copied 2097152 bytes from address 0x00000000 in flash to spi_read

    root@1:~# sha1sum spi_*
    51eeddc316a4121cf96e76e526d94d638283ec22  spi_read
    51eeddc316a4121cf96e76e526d94d638283ec22  spi_test

d/ Dump mtd_info data::

    root@1:~# mtd_debug info /dev/mtd1
    mtd.type = MTD_NORFLASH
    mtd.flags = MTD_CAP_NORFLASH
    mtd.size = 8388608 (8M)
    mtd.erasesize = 4096 (4K)
    mtd.writesize = 1
    mtd.oobsize = 0
    regions = 0
