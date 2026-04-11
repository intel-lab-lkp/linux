==================
VMUFAT FILESYSTEM
==================

VMUFAT is the simple file allocation table (FAT) based filesystem used in Sega
Dreamcast Visual Memory Units (VMUs) and various Dreamcast emulators and
Android apps etc.

It is not recommended for general use, but does not require a Dreamcast.

All the physical VMU devices that were made were of the same size - 256 blocks
of 512 octets (8 bit bytes) - 128KB in total. But the specification supports a 
wider range of sizes and the filesystem in the Linux kernel is capable of
handling volumes of a size between 256 blocks and 65536 blocks.

The standard 256 block VMU is described below:

+----------+---------------------------------------------+
| BLOCK NO |   CONTENT                                   |
+==========+=============================================+
| 0-199    |   Space used by Dreamcast to save files     |
+----------+---------------------------------------------+
| 200-240  |   Space used by the Dreamcast system        |
+----------+---------------------------------------------+
| 241-253  |   Directory (can hold 208 file records)     |
+----------+---------------------------------------------+
| 254      |   File Allocation Table (FAT)               |
+----------+---------------------------------------------+
| 255      |   Root Block                                |
+----------+---------------------------------------------+

The standard VMU filesystem has 241 blocks which can be used for file storage
but a Dreamcast will only use 200 of these. The other 41 are for Dreamcast system
use, though the filesystem can access them if set up as a general file store.
If a device is formatted as strictly compatible with the Dreamcast those blocks
will not be accessible. Most use cases will want that strict compatibility.

An executable file (generally a game written in the native machine code of
the VMU's microcontroller) must begin at block 0 and be stored linearly in
the volume. The filesystem driver seeks to match that policy but it is not
absolutely constrained by it.

=========
Directory
=========

The directory contains records 32 octets long (format detail below from Marcus
Comstedt's website - http://mc.pp.se/dc/vms/flashmem.html)

0x00      : file type (0x00 = no file, 0x33 = data, 0xcc = game)

0x01      : copy protect (0x00 = copy ok, 0xff = copy protected)

0x02-0x03 : 16 bits (little endian) : location of first block

0x04-0x0f : ASCII string : filename (12 characters)

0x10-0x17 : Binary Coded Decimal timestamp: file creation time

0x18-0x19 : 16 bits (little endian) : file size (in blocks)

0x1a-0x1b : 16 bits (little endian) : offset of header (in blocks)


Header positioning is a matter for executable files written in native
code for a physical VMU (an 8 bit Sanyo microcontroller).

BCD dates are encoded as follows:

Century prefix (eg., 20)

Year (eg., 12)

Month (eg., 02)

Day in month (eg., 29)

Hour (eg., 20)

Minute (eg., 49)

Second (eg., 22)

Day of week (Monday is 00, Sunday is 06)

=====================
File allocation table
=====================

Every block in the volume is mapped to the FAT eg., block 0 of the VMU maps to
the first 16 bits of the FAT and so on. Blocks marked 0xFFFC are empty, blocks
allocated to a file are either numbered with with next block or, if the final
block are marked 0xFFFA.

==========
Root block
==========

The first 16 octets are marked as 0x55 - we use this to provide the *magic
number* for this filesystem. The octets at 0x10 - 0x14 are used to determine
the colour the representation of the VMU is displayed in by a Dreamcast and our
filesystem does not touch these.

Octets 0x30 - 0x37 contain the BCD timestamp of the VMU.

Octets 0x46 - 0x47 contain the location (little endian format) of the FAT

Octets 0x48 - 0x49 contain the size (little endian) of the FAT

Octets 0x4A - 0x4B contain the location (little endian) of the Directory

Octets 0x4C - 0x4D contain the size (little endian) of the Directory

Octets 0x4E - 0x4F is Dreamcast specific (icon shape)

Octets 0x50 - 0x51 contain the number (little endian) of user blocks - NB this
is marked as 200 in a physical VMU. A filesystem formatted for strict
compatibility will respect this boundary, one formatted as a general
file store will not and will instead seek to use all writeable blocks.

====================
Formatting tool
====================

A formatting tool is available at https://github.com/mcmenaminadrian/mkfs.vmufat

A physical, factory-set, VMU is unlikely to require reformatting, of course.
