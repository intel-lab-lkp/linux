.. SPDX-License-Identifier: GPL-2.0

Dynamic Structures
==================

Dynamic metadata are created on the fly when files and blocks are
allocated to files.

Index Nodes
-----------

In a regular UNIX filesystem, the inode stores all the metadata
pertaining to the file (time stamps, block maps, extended attributes,
etc), not the directory entry. To find the information associated with a
file, one must traverse the directory files to find the directory entry
associated with a file, then load the inode to find the metadata for
that file. ext4 appears to cheat (for performance reasons) a little bit
by storing a copy of the file type (normally stored in the inode) in the
directory entry. (Compare all this to FAT, which stores all the file
information directly in the directory entry, but does not support hard
links and is in general more seek-happy than ext4 due to its simpler
block allocator and extensive use of linked lists.)

The inode table is a linear array of ``struct ext4_inode``. The table is
sized to have enough blocks to store at least
``sb.s_inode_size * sb.s_inodes_per_group`` bytes. The number of the
block group containing an inode can be calculated as
``(inode_number - 1) / sb.s_inodes_per_group``, and the offset into the
group's table is ``(inode_number - 1) % sb.s_inodes_per_group``. There
is no inode 0.

The inode checksum is calculated against the FS UUID, the inode number,
and the inode structure itself.

The inode table entry is laid out in ``struct ext4_inode``.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1
   :class: longtable

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le16
     - i_mode
     - File mode. See the table i_mode_ below.
   * - 0x2
     - __le16
     - i_uid
     - Lower 16-bits of Owner UID.
   * - 0x4
     - __le32
     - i_size_lo
     - Lower 32-bits of size in bytes.
   * - 0x8
     - __le32
     - i_atime
     - Last access time, in seconds since the epoch. However, if the EA_INODE
       inode flag is set, this inode stores an extended attribute value and
       this field contains the checksum of the value.
   * - 0xC
     - __le32
     - i_ctime
     - Last inode change time, in seconds since the epoch. However, if the
       EA_INODE inode flag is set, this inode stores an extended attribute
       value and this field contains the lower 32 bits of the attribute value's
       reference count.
   * - 0x10
     - __le32
     - i_mtime
     - Last data modification time, in seconds since the epoch. However, if the
       EA_INODE inode flag is set, this inode stores an extended attribute
       value and this field contains the number of the inode that owns the
       extended attribute.
   * - 0x14
     - __le32
     - i_dtime
     - Deletion Time, in seconds since the epoch.
   * - 0x18
     - __le16
     - i_gid
     - Lower 16-bits of GID.
   * - 0x1A
     - __le16
     - i_links_count
     - Hard link count. Normally, ext4 does not permit an inode to have more
       than 65,000 hard links. This applies to files as well as directories,
       which means that there cannot be more than 64,998 subdirectories in a
       directory (each subdirectory's '..' entry counts as a hard link, as does
       the '.' entry in the directory itself). With the DIR_NLINK feature
       enabled, ext4 supports more than 64,998 subdirectories by setting this
       field to 1 to indicate that the number of hard links is not known.
   * - 0x1C
     - __le32
     - i_blocks_lo
     - Lower 32-bits of “block” count. If the huge_file feature flag is not
       set on the filesystem, the file consumes ``i_blocks_lo`` 512-byte blocks
       on disk. If huge_file is set and EXT4_HUGE_FILE_FL is NOT set in
       ``inode.i_flags``, then the file consumes ``i_blocks_lo + (i_blocks_hi
       << 32)`` 512-byte blocks on disk. If huge_file is set and
       EXT4_HUGE_FILE_FL IS set in ``inode.i_flags``, then this file
       consumes (``i_blocks_lo + i_blocks_hi`` << 32) filesystem blocks on
       disk.
   * - 0x20
     - __le32
     - i_flags
     - Inode flags. See the table i_flags_ below.
   * - 0x24
     - 4 bytes
     - i_osd1
     - See the table i_osd1_ for more details.
   * - 0x28
     - 60 bytes
     - i_block[EXT4_N_BLOCKS=15]
     - Block map or extent tree. See the section “The Contents of inode.i_block”.
   * - 0x64
     - __le32
     - i_generation
     - File version (for NFS).
   * - 0x68
     - __le32
     - i_file_acl_lo
     - Lower 32-bits of extended attribute block. ACLs are of course one of
       many possible extended attributes; I think the name of this field is a
       result of the first use of extended attributes being for ACLs.
   * - 0x6C
     - __le32
     - i_size_high / i_dir_acl
     - Upper 32-bits of file/directory size. In ext2/3 this field was named
       i_dir_acl, though it was usually set to zero and never used.
   * - 0x70
     - __le32
     - i_obso_faddr
     - (Obsolete) fragment address.
   * - 0x74
     - 12 bytes
     - i_osd2
     - See the table i_osd2_ for more details.
   * - 0x80
     - __le16
     - i_extra_isize
     - Size of this inode - 128. Alternately, the size of the extended inode
       fields beyond the original ext2 inode, including this field.
   * - 0x82
     - __le16
     - i_checksum_hi
     - Upper 16-bits of the inode checksum.
   * - 0x84
     - __le32
     - i_ctime_extra
     - Extra change time bits. This provides sub-second precision. See Inode
       Timestamps section.
   * - 0x88
     - __le32
     - i_mtime_extra
     - Extra modification time bits. This provides sub-second precision.
   * - 0x8C
     - __le32
     - i_atime_extra
     - Extra access time bits. This provides sub-second precision.
   * - 0x90
     - __le32
     - i_crtime
     - File creation time, in seconds since the epoch.
   * - 0x94
     - __le32
     - i_crtime_extra
     - Extra file creation time bits. This provides sub-second precision.
   * - 0x98
     - __le32
     - i_version_hi
     - Upper 32-bits for version number.
   * - 0x9C
     - __le32
     - i_projid
     - Project ID.

.. _i_mode:

The ``i_mode`` value is a combination of the following flags:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - S_IXOTH (Others may execute)
   * - 0x2
     - S_IWOTH (Others may write)
   * - 0x4
     - S_IROTH (Others may read)
   * - 0x8
     - S_IXGRP (Group members may execute)
   * - 0x10
     - S_IWGRP (Group members may write)
   * - 0x20
     - S_IRGRP (Group members may read)
   * - 0x40
     - S_IXUSR (Owner may execute)
   * - 0x80
     - S_IWUSR (Owner may write)
   * - 0x100
     - S_IRUSR (Owner may read)
   * - 0x200
     - S_ISVTX (Sticky bit)
   * - 0x400
     - S_ISGID (Set GID)
   * - 0x800
     - S_ISUID (Set UID)
   * -
     - These are mutually-exclusive file types:
   * - 0x1000
     - S_IFIFO (FIFO)
   * - 0x2000
     - S_IFCHR (Character device)
   * - 0x4000
     - S_IFDIR (Directory)
   * - 0x6000
     - S_IFBLK (Block device)
   * - 0x8000
     - S_IFREG (Regular file)
   * - 0xA000
     - S_IFLNK (Symbolic link)
   * - 0xC000
     - S_IFSOCK (Socket)

.. _i_flags:

The ``i_flags`` field is a combination of these values:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - This file requires secure deletion (EXT4_SECRM_FL). (not implemented)
   * - 0x2
     - This file should be preserved, should undeletion be desired
       (EXT4_UNRM_FL). (not implemented)
   * - 0x4
     - File is compressed (EXT4_COMPR_FL). (not really implemented)
   * - 0x8
     - All writes to the file must be synchronous (EXT4_SYNC_FL).
   * - 0x10
     - File is immutable (EXT4_IMMUTABLE_FL).
   * - 0x20
     - File can only be appended (EXT4_APPEND_FL).
   * - 0x40
     - The dump(1) utility should not dump this file (EXT4_NODUMP_FL).
   * - 0x80
     - Do not update access time (EXT4_NOATIME_FL).
   * - 0x100
     - Dirty compressed file (EXT4_DIRTY_FL). (not used)
   * - 0x200
     - File has one or more compressed clusters (EXT4_COMPRBLK_FL). (not used)
   * - 0x400
     - Do not compress file (EXT4_NOCOMPR_FL). (not used)
   * - 0x800
     - Encrypted inode (EXT4_ENCRYPT_FL). This bit value previously was
       EXT4_ECOMPR_FL (compression error), which was never used.
   * - 0x1000
     - Directory has hashed indexes (EXT4_INDEX_FL).
   * - 0x2000
     - AFS magic directory (EXT4_IMAGIC_FL).
   * - 0x4000
     - File data must always be written through the journal
       (EXT4_JOURNAL_DATA_FL).
   * - 0x8000
     - File tail should not be merged (EXT4_NOTAIL_FL). (not used by ext4)
   * - 0x10000
     - All directory entry data should be written synchronously (see
       ``dirsync``) (EXT4_DIRSYNC_FL).
   * - 0x20000
     - Top of directory hierarchy (EXT4_TOPDIR_FL).
   * - 0x40000
     - This is a huge file (EXT4_HUGE_FILE_FL).
   * - 0x80000
     - Inode uses extents (EXT4_EXTENTS_FL).
   * - 0x100000
     - Verity protected file (EXT4_VERITY_FL).
   * - 0x200000
     - Inode stores a large extended attribute value in its data blocks
       (EXT4_EA_INODE_FL).
   * - 0x400000
     - This file has blocks allocated past EOF (EXT4_EOFBLOCKS_FL).
       (deprecated)
   * - 0x01000000
     - Inode is a snapshot (``EXT4_SNAPFILE_FL``). (not in mainline)
   * - 0x04000000
     - Snapshot is being deleted (``EXT4_SNAPFILE_DELETED_FL``). (not in
       mainline)
   * - 0x08000000
     - Snapshot shrink has completed (``EXT4_SNAPFILE_SHRUNK_FL``). (not in
       mainline)
   * - 0x10000000
     - Inode has inline data (EXT4_INLINE_DATA_FL).
   * - 0x20000000
     - Create children with the same project ID (EXT4_PROJINHERIT_FL).
   * - 0x80000000
     - Reserved for ext4 library (EXT4_RESERVED_FL).
   * -
     - Aggregate flags:
   * - 0x705BDFFF
     - User-visible flags.
   * - 0x604BC0FF
     - User-modifiable flags. Note that while EXT4_JOURNAL_DATA_FL and
       EXT4_EXTENTS_FL can be set with setattr, they are not in the kernel's
       EXT4_FL_USER_MODIFIABLE mask, since it needs to handle the setting of
       these flags in a special manner and they are masked out of the set of
       flags that are saved directly to i_flags.

.. _i_osd1:

The ``osd1`` field has multiple meanings depending on the creator:

Linux:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - l_i_version
     - Inode version. However, if the EA_INODE inode flag is set, this inode
       stores an extended attribute value and this field contains the upper 32
       bits of the attribute value's reference count.

Hurd:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - h_i_translator
     - ??

Masix:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - m_i_reserved
     - ??

.. _i_osd2:

The ``osd2`` field has multiple meanings depending on the filesystem creator:

Linux:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le16
     - l_i_blocks_high
     - Upper 16-bits of the block count. Please see the note attached to
       i_blocks_lo.
   * - 0x2
     - __le16
     - l_i_file_acl_high
     - Upper 16-bits of the extended attribute block (historically, the file
       ACL location). See the Extended Attributes section below.
   * - 0x4
     - __le16
     - l_i_uid_high
     - Upper 16-bits of the Owner UID.
   * - 0x6
     - __le16
     - l_i_gid_high
     - Upper 16-bits of the GID.
   * - 0x8
     - __le16
     - l_i_checksum_lo
     - Lower 16-bits of the inode checksum.
   * - 0xA
     - __le16
     - l_i_reserved
     - Unused.

Hurd:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le16
     - h_i_reserved1
     - ??
   * - 0x2
     - __u16
     - h_i_mode_high
     - Upper 16-bits of the file mode.
   * - 0x4
     - __le16
     - h_i_uid_high
     - Upper 16-bits of the Owner UID.
   * - 0x6
     - __le16
     - h_i_gid_high
     - Upper 16-bits of the GID.
   * - 0x8
     - __u32
     - h_i_author
     - Author code?

Masix:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le16
     - h_i_reserved1
     - ??
   * - 0x2
     - __u16
     - m_i_file_acl_high
     - Upper 16-bits of the extended attribute block (historically, the file
       ACL location).
   * - 0x4
     - __u32
     - m_i_reserved2[2]
     - ??

Inode Size
~~~~~~~~~~

In ext2 and ext3, the inode structure size was fixed at 128 bytes
(``EXT2_GOOD_OLD_INODE_SIZE``) and each inode had a disk record size of
128 bytes. Starting with ext4, it is possible to allocate a larger
on-disk inode at format time for all inodes in the filesystem to provide
space beyond the end of the original ext2 inode. The on-disk inode
record size is recorded in the superblock as ``s_inode_size``. The
number of bytes actually used by struct ext4_inode beyond the original
128-byte ext2 inode is recorded in the ``i_extra_isize`` field for each
inode, which allows struct ext4_inode to grow for a new kernel without
having to upgrade all of the on-disk inodes. Access to fields beyond
EXT2_GOOD_OLD_INODE_SIZE should be verified to be within
``i_extra_isize``. By default, ext4 inode records are 256 bytes, and (as
of August 2019) the inode structure is 160 bytes
(``i_extra_isize = 32``). The extra space between the end of the inode
structure and the end of the inode record can be used to store extended
attributes. Each inode record can be as large as the filesystem block
size, though this is not terribly efficient.

Finding an Inode
~~~~~~~~~~~~~~~~

Each block group contains ``sb->s_inodes_per_group`` inodes. Because
inode 0 is defined not to exist, this formula can be used to find the
block group that an inode lives in:
``bg = (inode_num - 1) / sb->s_inodes_per_group``. The particular inode
can be found within the block group's inode table at
``index = (inode_num - 1) % sb->s_inodes_per_group``. To get the byte
address within the inode table, use
``offset = index * sb->s_inode_size``.

Inode Timestamps
~~~~~~~~~~~~~~~~

Four timestamps are recorded in the lower 128 bytes of the inode
structure -- inode change time (ctime), access time (atime), data
modification time (mtime), and deletion time (dtime). The four fields
are 32-bit signed integers that represent seconds since the Unix epoch
(1970-01-01 00:00:00 GMT), which means that the fields will overflow in
January 2038. If the filesystem does not have orphan_file feature, inodes
that are not linked from any directory but are still open (orphan inodes) have
the dtime field overloaded for use with the orphan list. The superblock field
``s_last_orphan`` points to the first inode in the orphan list; dtime is then
the number of the next orphaned inode, or zero if there are no more orphans.

If the inode structure size ``sb->s_inode_size`` is larger than 128
bytes and the ``i_inode_extra`` field is large enough to encompass the
respective ``i_[cma]time_extra`` field, the ctime, atime, and mtime
inode fields are widened to 64 bits. Within this “extra” 32-bit field,
the lower two bits are used to extend the 32-bit seconds field to be 34
bit wide; the upper 30 bits are used to provide nanosecond timestamp
accuracy. Therefore, timestamps should not overflow until May 2446.
dtime was not widened. There is also a fifth timestamp to record inode
creation time (crtime); this field is 64-bits wide and decoded in the
same manner as 64-bit [cma]time. Neither crtime nor dtime are accessible
through the regular stat() interface, though debugfs will report them.

We use the 32-bit signed time value plus (2^32 * (extra epoch bits)).
In other words:

.. list-table::
   :widths: 20 20 20 20 20
   :header-rows: 1

   * - Extra epoch bits
     - MSB of 32-bit time
     - Adjustment for signed 32-bit to 64-bit tv_sec
     - Decoded 64-bit tv_sec
     - valid time range
   * - 0 0
     - 1
     - 0
     - ``-0x80000000 - -0x00000001``
     - 1901-12-13 to 1969-12-31
   * - 0 0
     - 0
     - 0
     - ``0x000000000 - 0x07fffffff``
     - 1970-01-01 to 2038-01-19
   * - 0 1
     - 1
     - 0x100000000
     - ``0x080000000 - 0x0ffffffff``
     - 2038-01-19 to 2106-02-07
   * - 0 1
     - 0
     - 0x100000000
     - ``0x100000000 - 0x17fffffff``
     - 2106-02-07 to 2174-02-25
   * - 1 0
     - 1
     - 0x200000000
     - ``0x180000000 - 0x1ffffffff``
     - 2174-02-25 to 2242-03-16
   * - 1 0
     - 0
     - 0x200000000
     - ``0x200000000 - 0x27fffffff``
     - 2242-03-16 to 2310-04-04
   * - 1 1
     - 1
     - 0x300000000
     - ``0x280000000 - 0x2ffffffff``
     - 2310-04-04 to 2378-04-22
   * - 1 1
     - 0
     - 0x300000000
     - ``0x300000000 - 0x37fffffff``
     - 2378-04-22 to 2446-05-10

This is a somewhat odd encoding since there are effectively seven times
as many positive values as negative values. There have also been
long-standing bugs decoding and encoding dates beyond 2038, which don't
seem to be fixed as of kernel 3.12 and e2fsprogs 1.42.8. 64-bit kernels
incorrectly use the extra epoch bits 1,1 for dates between 1901 and
1970. At some point the kernel will be fixed and e2fsck will fix this
situation, assuming that it is run before 2310.

The Contents of inode.i_block
------------------------------

Depending on the type of file an inode describes, the 60 bytes of
storage in ``inode.i_block`` can be used in different ways. In general,
regular files and directories will use it for file block indexing
information, and special files will use it for special purposes.

Symbolic Links
~~~~~~~~~~~~~~

The target of a symbolic link will be stored in this field if the target
string is less than 60 bytes long. Otherwise, either extents or block
maps will be used to allocate data blocks to store the link target.

Direct/Indirect Block Addressing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

In ext2/3, file block numbers were mapped to logical block numbers by
means of an (up to) three level 1-1 block map. To find the logical block
that stores a particular file block, the code would navigate through
this increasingly complicated structure. Notice that there is neither a
magic number nor a checksum to provide any level of confidence that the
block isn't full of garbage.

.. ifconfig:: builder != 'latex'

   .. include:: blockmap.rst

.. ifconfig:: builder == 'latex'

   [Table omitted because LaTeX doesn't support nested tables.]

Note that with this block mapping scheme, it is necessary to fill out a
lot of mapping data even for a large contiguous file! This inefficiency
led to the creation of the extent mapping scheme, discussed below.

Notice also that a file using this mapping scheme cannot be placed
higher than 2^32 blocks.

Extent Tree
~~~~~~~~~~~

In ext4, the file to logical block map has been replaced with an extent
tree. Under the old scheme, allocating a contiguous run of 1,000 blocks
requires an indirect block to map all 1,000 entries; with extents, the
mapping is reduced to a single ``struct ext4_extent`` with
``ee_len = 1000``. If flex_bg is enabled, it is possible to allocate
very large files with a single extent, at a considerable reduction in
metadata block use, and some improvement in disk efficiency. The inode
must have the extents flag (0x80000) flag set for this feature to be in
use.

Extents are arranged as a tree. Each node of the tree begins with a
``struct ext4_extent_header``. If the node is an interior node
(``eh.eh_depth`` > 0), the header is followed by ``eh.eh_entries``
instances of ``struct ext4_extent_idx``; each of these index entries
points to a block containing more nodes in the extent tree. If the node
is a leaf node (``eh.eh_depth == 0``), then the header is followed by
``eh.eh_entries`` instances of ``struct ext4_extent``; these instances
point to the file's data blocks. The root node of the extent tree is
stored in ``inode.i_block``, which allows for the first four extents to
be recorded without the use of extra metadata blocks.

The extent tree header is recorded in ``struct ext4_extent_header``,
which is 12 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le16
     - eh_magic
     - Magic number, 0xF30A.
   * - 0x2
     - __le16
     - eh_entries
     - Number of valid entries following the header.
   * - 0x4
     - __le16
     - eh_max
     - Maximum number of entries that could follow the header.
   * - 0x6
     - __le16
     - eh_depth
     - Depth of this extent node in the extent tree. 0 = this extent node
       points to data blocks; otherwise, this extent node points to other
       extent nodes. The extent tree can be at most 5 levels deep: a logical
       block number can be at most ``2^32``, and the smallest ``n`` that
       satisfies ``4*(((blocksize - 12)/12)^n) >= 2^32`` is 5.
   * - 0x8
     - __le32
     - eh_generation
     - Generation of the tree. (Used by Lustre, but not standard ext4).

Internal nodes of the extent tree, also known as index nodes, are
recorded as ``struct ext4_extent_idx``, and are 12 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - ei_block
     - This index node covers file blocks from 'block' onward.
   * - 0x4
     - __le32
     - ei_leaf_lo
     - Lower 32-bits of the block number of the extent node that is the next
       level lower in the tree. The tree node pointed to can be either another
       internal node or a leaf node, described below.
   * - 0x8
     - __le16
     - ei_leaf_hi
     - Upper 16-bits of the previous field.
   * - 0xA
     - __u16
     - ei_unused
     -

Leaf nodes of the extent tree are recorded as ``struct ext4_extent``,
and are also 12 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - ee_block
     - First file block number that this extent covers.
   * - 0x4
     - __le16
     - ee_len
     - Number of blocks covered by extent. If the value of this field is <=
       32768, the extent is initialized. If the value of the field is > 32768,
       the extent is uninitialized and the actual extent length is ``ee_len`` -
       32768. Therefore, the maximum length of a initialized extent is 32768
       blocks, and the maximum length of an uninitialized extent is 32767.
   * - 0x6
     - __le16
     - ee_start_hi
     - Upper 16-bits of the block number to which this extent points.
   * - 0x8
     - __le32
     - ee_start_lo
     - Lower 32-bits of the block number to which this extent points.

Prior to the introduction of metadata checksums, the extent header +
extent entries always left at least 4 bytes of unallocated space at the
end of each extent tree data block (because (2^x % 12) >= 4). Therefore,
the 32-bit checksum is inserted into this space. The 4 extents in the
inode do not need checksumming, since the inode is already checksummed.
The checksum is calculated against the FS UUID, the inode number, the
inode generation, and the entire extent block leading up to (but not
including) the checksum itself.

``struct ext4_extent_tail`` is 4 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - eb_checksum
     - Checksum of the extent block, crc32c(uuid+inum+igeneration+extentblock)

Inline Data
~~~~~~~~~~~

If the inline data feature is enabled for the filesystem and the flag is
set for the inode, it is possible that the first 60 bytes of the file
data are stored here.

Directory Entries
-----------------

In an ext4 filesystem, a directory is more or less a flat file that maps
an arbitrary byte string (usually ASCII) to an inode number on the
filesystem. There can be many directory entries across the filesystem
that reference the same inode number--these are known as hard links, and
that is why hard links cannot reference files on other filesystems. As
such, directory entries are found by reading the data block(s)
associated with a directory file for the particular directory entry that
is desired.

Linear (Classic) Directories
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

By default, each directory lists its entries in an “almost-linear”
array. I write “almost” because it's not a linear array in the memory
sense because directory entries are not split across filesystem blocks.
Therefore, it is more accurate to say that a directory is a series of
data blocks and that each block contains a linear array of directory
entries. The end of each per-block array is signified by reaching the
end of the block; the last entry in the block has a record length that
takes it all the way to the end of the block. The end of the entire
directory is of course signified by reaching the end of the file. Unused
directory entries are signified by inode = 0. By default the filesystem
uses ``struct ext4_dir_entry_2`` for directory entries unless the
“filetype” feature flag is not set, in which case it uses
``struct ext4_dir_entry``.

The original directory entry format is ``struct ext4_dir_entry``, which
is at most 263 bytes long, though on disk you'll need to reference
``dirent.rec_len`` to know for sure.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - inode
     - Number of the inode that this directory entry points to.
   * - 0x4
     - __le16
     - rec_len
     - Length of this directory entry. Must be a multiple of 4.
   * - 0x6
     - __le16
     - name_len
     - Length of the file name.
   * - 0x8
     - char
     - name[EXT4_NAME_LEN]
     - File name.

Since file names cannot be longer than 255 bytes, the new directory
entry format shortens the name_len field and uses the space for a file
type flag, probably to avoid having to load every inode during directory
tree traversal. This format is ``ext4_dir_entry_2``, which is at most
263 bytes long, though on disk you'll need to reference
``dirent.rec_len`` to know for sure.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - inode
     - Number of the inode that this directory entry points to.
   * - 0x4
     - __le16
     - rec_len
     - Length of this directory entry.
   * - 0x6
     - __u8
     - name_len
     - Length of the file name.
   * - 0x7
     - __u8
     - file_type
     - File type code, see ftype_ table below.
   * - 0x8
     - char
     - name[EXT4_NAME_LEN]
     - File name.

.. _ftype:

The directory file type is one of the following values:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x0
     - Unknown.
   * - 0x1
     - Regular file.
   * - 0x2
     - Directory.
   * - 0x3
     - Character device file.
   * - 0x4
     - Block device file.
   * - 0x5
     - FIFO.
   * - 0x6
     - Socket.
   * - 0x7
     - Symbolic link.

To support directories that are both encrypted and casefolded directories, we
must also include hash information in the directory entry. We append
``ext4_extended_dir_entry_2`` to ``ext4_dir_entry_2`` except for the entries
for dot and dotdot, which are kept the same. The structure follows immediately
after ``name`` and is included in the size listed by ``rec_len`` If a directory
entry uses this extension, it may be up to 271 bytes.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - hash
     - The hash of the directory name
   * - 0x4
     - __le32
     - minor_hash
     - The minor hash of the directory name


In order to add checksums to these classic directory blocks, a phony
``struct ext4_dir_entry`` is placed at the end of each leaf block to
hold the checksum. The directory entry is 12 bytes long. The inode
number and name_len fields are set to zero to fool old software into
ignoring an apparently empty directory entry, and the checksum is stored
in the place where the name normally goes. The structure is
``struct ext4_dir_entry_tail``:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - det_reserved_zero1
     - Inode number, which must be zero.
   * - 0x4
     - __le16
     - det_rec_len
     - Length of this directory entry, which must be 12.
   * - 0x6
     - __u8
     - det_reserved_zero2
     - Length of the file name, which must be zero.
   * - 0x7
     - __u8
     - det_reserved_ft
     - File type, which must be 0xDE.
   * - 0x8
     - __le32
     - det_checksum
     - Directory leaf block checksum.

The leaf directory block checksum is calculated against the FS UUID, the
directory's inode number, the directory's inode generation number, and
the entire directory entry block up to (but not including) the fake
directory entry.

Hash Tree Directories
~~~~~~~~~~~~~~~~~~~~~

A linear array of directory entries isn't great for performance, so a
new feature was added to ext3 to provide a faster (but peculiar)
balanced tree keyed off a hash of the directory entry name. If the
EXT4_INDEX_FL (0x1000) flag is set in the inode, this directory uses a
hashed btree (htree) to organize and find directory entries. For
backwards read-only compatibility with ext2, this tree is actually
hidden inside the directory file, masquerading as “empty” directory data
blocks! It was stated previously that the end of the linear directory
entry table was signified with an entry pointing to inode 0; this is
(ab)used to fool the old linear-scan algorithm into thinking that the
rest of the directory block is empty so that it moves on.

The root of the tree always lives in the first data block of the
directory. By ext2 custom, the '.' and '..' entries must appear at the
beginning of this first block, so they are put here as two
``struct ext4_dir_entry_2`` s and not stored in the tree. The rest of
the root node contains metadata about the tree and finally a hash->block
map to find nodes that are lower in the htree. If
``dx_root.info.indirect_levels`` is non-zero then the htree has two
levels; the data block pointed to by the root node's map is an interior
node, which is indexed by a minor hash. Interior nodes in this tree
contains a zeroed out ``struct ext4_dir_entry_2`` followed by a
minor_hash->block map to find leafe nodes. Leaf nodes contain a linear
array of all ``struct ext4_dir_entry_2``; all of these entries
(presumably) hash to the same value. If there is an overflow, the
entries simply overflow into the next leaf node, and the
least-significant bit of the hash (in the interior node map) that gets
us to this next leaf node is set.

To traverse the directory as a htree, the code calculates the hash of
the desired file name and uses it to find the corresponding block
number. If the tree is flat, the block is a linear array of directory
entries that can be searched; otherwise, the minor hash of the file name
is computed and used against this second block to find the corresponding
third block number. That third block number will be a linear array of
directory entries.

To traverse the directory as a linear array (such as the old code does),
the code simply reads every data block in the directory. The blocks used
for the htree will appear to have no entries (aside from '.' and '..')
and so only the leaf nodes will appear to have any interesting content.

The root of the htree is in ``struct dx_root``, which is the full length
of a data block:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - dot.inode
     - inode number of this directory.
   * - 0x4
     - __le16
     - dot.rec_len
     - Length of this record, 12.
   * - 0x6
     - u8
     - dot.name_len
     - Length of the name, 1.
   * - 0x7
     - u8
     - dot.file_type
     - File type of this entry, 0x2 (directory) (if the feature flag is set).
   * - 0x8
     - char
     - dot.name[4]
     - “.\0\0\0”
   * - 0xC
     - __le32
     - dotdot.inode
     - inode number of parent directory.
   * - 0x10
     - __le16
     - dotdot.rec_len
     - block_size - 12. The record length is long enough to cover all htree
       data.
   * - 0x12
     - u8
     - dotdot.name_len
     - Length of the name, 2.
   * - 0x13
     - u8
     - dotdot.file_type
     - File type of this entry, 0x2 (directory) (if the feature flag is set).
   * - 0x14
     - char
     - dotdot_name[4]
     - “..\0\0”
   * - 0x18
     - __le32
     - struct dx_root_info.reserved_zero
     - Zero.
   * - 0x1C
     - u8
     - struct dx_root_info.hash_version
     - Hash type, see dirhash_ table below.
   * - 0x1D
     - u8
     - struct dx_root_info.info_length
     - Length of the tree information, 0x8.
   * - 0x1E
     - u8
     - struct dx_root_info.indirect_levels
     - Depth of the htree. Cannot be larger than 3 if the INCOMPAT_LARGEDIR
       feature is set; cannot be larger than 2 otherwise.
   * - 0x1F
     - u8
     - struct dx_root_info.unused_flags
     -
   * - 0x20
     - __le16
     - limit
     - Maximum number of dx_entries that can follow this header, plus 1 for
       the header itself.
   * - 0x22
     - __le16
     - count
     - Actual number of dx_entries that follow this header, plus 1 for the
       header itself.
   * - 0x24
     - __le32
     - block
     - The block number (within the directory file) that goes with hash=0.
   * - 0x28
     - struct dx_entry
     - entries[0]
     - As many 8-byte ``struct dx_entry`` as fits in the rest of the data block.

.. _dirhash:

The directory hash is one of the following values:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x0
     - Legacy.
   * - 0x1
     - Half MD4.
   * - 0x2
     - Tea.
   * - 0x3
     - Legacy, unsigned.
   * - 0x4
     - Half MD4, unsigned.
   * - 0x5
     - Tea, unsigned.
   * - 0x6
     - Siphash.

Interior nodes of an htree are recorded as ``struct dx_node``, which is
also the full length of a data block:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - fake.inode
     - Zero, to make it look like this entry is not in use.
   * - 0x4
     - __le16
     - fake.rec_len
     - The size of the block, in order to hide all of the dx_node data.
   * - 0x6
     - u8
     - name_len
     - Zero. There is no name for this “unused” directory entry.
   * - 0x7
     - u8
     - file_type
     - Zero. There is no file type for this “unused” directory entry.
   * - 0x8
     - __le16
     - limit
     - Maximum number of dx_entries that can follow this header, plus 1 for
       the header itself.
   * - 0xA
     - __le16
     - count
     - Actual number of dx_entries that follow this header, plus 1 for the
       header itself.
   * - 0xE
     - __le32
     - block
     - The block number (within the directory file) that goes with the lowest
       hash value of this block. This value is stored in the parent block.
   * - 0x12
     - struct dx_entry
     - entries[0]
     - As many 8-byte ``struct dx_entry`` as fits in the rest of the data block.

The hash maps that exist in both ``struct dx_root`` and
``struct dx_node`` are recorded as ``struct dx_entry``, which is 8 bytes
long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - hash
     - Hash code.
   * - 0x4
     - __le32
     - block
     - Block number (within the directory file, not filesystem blocks) of the
       next node in the htree.

(If you think this is all quite clever and peculiar, so does the
author.)

If metadata checksums are enabled, the last 8 bytes of the directory
block (precisely the length of one dx_entry) are used to store a
``struct dx_tail``, which contains the checksum. The ``limit`` and
``count`` entries in the dx_root/dx_node structures are adjusted as
necessary to fit the dx_tail into the block. If there is no space for
the dx_tail, the user is notified to run e2fsck -D to rebuild the
directory index (which will ensure that there's space for the checksum.
The dx_tail structure is 8 bytes long and looks like this:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - u32
     - dt_reserved
     - Zero.
   * - 0x4
     - __le32
     - dt_checksum
     - Checksum of the htree directory block.

The checksum is calculated against the FS UUID, the htree index header
(dx_root or dx_node), all of the htree indices (dx_entry) that are in
use, and the tail block (dx_tail).

Extended Attributes
-------------------

Extended attributes (xattrs) are typically stored in a separate data
block on the disk and referenced from inodes via ``inode.i_file_acl*``.
The first use of extended attributes seems to have been for storing file
ACLs and other security data (selinux). With the ``user_xattr`` mount
option it is possible for users to store extended attributes so long as
all attribute names begin with “user”; this restriction seems to have
disappeared as of Linux 3.0.

There are two places where extended attributes can be found. The first
place is between the end of each inode entry and the beginning of the
next inode entry. For example, if inode.i_extra_isize = 28 and
sb.inode_size = 256, then there are 256 - (128 + 28) = 100 bytes
available for in-inode extended attribute storage. The second place
where extended attributes can be found is in the block pointed to by
``inode.i_file_acl``. As of Linux 3.11, it is not possible for this
block to contain a pointer to a second extended attribute block (or even
the remaining blocks of a cluster). In theory it is possible for each
attribute's value to be stored in a separate data block, though as of
Linux 3.11 the code does not permit this.

Keys are generally assumed to be ASCIIZ strings, whereas values can be
strings or binary data.

Extended attributes, when stored after the inode, have a header
``ext4_xattr_ibody_header`` that is 4 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - h_magic
     - Magic number for identification, 0xEA020000. This value is set by the
       Linux driver, though e2fsprogs doesn't seem to check it(?)

The beginning of an extended attribute block is in
``struct ext4_xattr_header``, which is 32 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - h_magic
     - Magic number for identification, 0xEA020000.
   * - 0x4
     - __le32
     - h_refcount
     - Reference count.
   * - 0x8
     - __le32
     - h_blocks
     - Number of disk blocks used.
   * - 0xC
     - __le32
     - h_hash
     - Hash value of all attributes.
   * - 0x10
     - __le32
     - h_checksum
     - Checksum of the extended attribute block.
   * - 0x14
     - __u32
     - h_reserved[3]
     - Zero.

The checksum is calculated against the FS UUID, the 64-bit block number
of the extended attribute block, and the entire block (header +
entries).

Following the ``struct ext4_xattr_header`` or
``struct ext4_xattr_ibody_header`` is an array of
``struct ext4_xattr_entry``; each of these entries is at least 16 bytes
long. When stored in an external block, the ``struct ext4_xattr_entry``
entries must be stored in sorted order. The sort order is
``e_name_index``, then ``e_name_len``, and finally ``e_name``.
Attributes stored inside an inode do not need be stored in sorted order.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __u8
     - e_name_len
     - Length of name.
   * - 0x1
     - __u8
     - e_name_index
     - Attribute name index. There is a discussion of this below.
   * - 0x2
     - __le16
     - e_value_offs
     - Location of this attribute's value on the disk block where it is stored.
       Multiple attributes can share the same value. For an inode attribute
       this value is relative to the start of the first entry; for a block this
       value is relative to the start of the block (i.e. the header).
   * - 0x4
     - __le32
     - e_value_inum
     - The inode where the value is stored. Zero indicates the value is in the
       same block as this entry. This field is only used if the
       INCOMPAT_EA_INODE feature is enabled.
   * - 0x8
     - __le32
     - e_value_size
     - Length of attribute value.
   * - 0xC
     - __le32
     - e_hash
     - Hash value of attribute name and attribute value. The kernel doesn't
       update the hash for in-inode attributes, so for that case this value
       must be zero, because e2fsck validates any non-zero hash regardless of
       where the xattr lives.
   * - 0x10
     - char
     - e_name[e_name_len]
     - Attribute name. Does not include trailing NULL.

Attribute values can follow the end of the entry table. There appears to
be a requirement that they be aligned to 4-byte boundaries. The values
are stored starting at the end of the block and grow towards the
xattr_header/xattr_entry table. When the two collide, the overflow is
put into a separate disk block. If the disk block fills up, the
filesystem returns -ENOSPC.

The first four fields of the ``ext4_xattr_entry`` are set to zero to
mark the end of the key list.

Attribute Name Indices
~~~~~~~~~~~~~~~~~~~~~~

Logically speaking, extended attributes are a series of key=value pairs.
The keys are assumed to be NULL-terminated strings. To reduce the amount
of on-disk space that the keys consume, the beginning of the key string
is matched against the attribute name index. If a match is found, the
attribute name index field is set, and matching string is removed from
the key name. Here is a map of name index values to key prefixes:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Name Index
     - Key Prefix
   * - 0
     - (no prefix)
   * - 1
     - “user.”
   * - 2
     - “system.posix_acl_access”
   * - 3
     - “system.posix_acl_default”
   * - 4
     - “trusted.”
   * - 6
     - “security.”
   * - 7
     - “system.” (inline_data only?)
   * - 8
     - “system.richacl” (SuSE kernels only?)

For example, if the attribute key is “user.fubar”, the attribute name
index is set to 1 and the “fubar” name is recorded on disk.

POSIX ACLs
~~~~~~~~~~

POSIX ACLs are stored in a reduced version of the Linux kernel (and
libacl's) internal ACL format. The key difference is that the version
number is different (1) and the ``e_id`` field is only stored for named
user and group ACLs.
