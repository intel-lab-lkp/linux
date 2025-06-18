.. SPDX-License-Identifier: GPL-2.0

Global Structures
=================

The filesystem is sharded into a number of block groups, each of which
have static metadata at fixed locations.

Super Block
-----------

The superblock records various information about the enclosing
filesystem, such as block counts, inode counts, supported features,
maintenance information, and more.

If the sparse_super feature flag is set, redundant copies of the
superblock and group descriptors are kept only in the groups whose group
number is either 0 or a power of 3, 5, or 7. If the flag is not set,
redundant copies are kept in all groups.

The superblock checksum is calculated against the superblock structure,
which includes the FS UUID.

The ext4 superblock is laid out as follows in
``struct ext4_super_block``:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - s_inodes_count
     - Total inode count.
   * - 0x4
     - __le32
     - s_blocks_count_lo
     - Total block count.
   * - 0x8
     - __le32
     - s_r_blocks_count_lo
     - This number of blocks can only be allocated by the super-user.
   * - 0xC
     - __le32
     - s_free_blocks_count_lo
     - Free block count.
   * - 0x10
     - __le32
     - s_free_inodes_count
     - Free inode count.
   * - 0x14
     - __le32
     - s_first_data_block
     - First data block. This must be at least 1 for 1k-block filesystems and
       is typically 0 for all other block sizes.
   * - 0x18
     - __le32
     - s_log_block_size
     - Block size is 2 ^ (10 + s_log_block_size).
   * - 0x1C
     - __le32
     - s_log_cluster_size
     - Cluster size is 2 ^ (10 + s_log_cluster_size) blocks if bigalloc is
       enabled. Otherwise s_log_cluster_size must equal s_log_block_size.
   * - 0x20
     - __le32
     - s_blocks_per_group
     - Blocks per group.
   * - 0x24
     - __le32
     - s_clusters_per_group
     - Clusters per group, if bigalloc is enabled. Otherwise
       s_clusters_per_group must equal s_blocks_per_group.
   * - 0x28
     - __le32
     - s_inodes_per_group
     - Inodes per group.
   * - 0x2C
     - __le32
     - s_mtime
     - Mount time, in seconds since the epoch.
   * - 0x30
     - __le32
     - s_wtime
     - Write time, in seconds since the epoch.
   * - 0x34
     - __le16
     - s_mnt_count
     - Number of mounts since the last fsck.
   * - 0x36
     - __le16
     - s_max_mnt_count
     - Number of mounts beyond which a fsck is needed.
   * - 0x38
     - __le16
     - s_magic
     - Magic signature, 0xEF53
   * - 0x3A
     - __le16
     - s_state
     - File system state. See super_state_ for more info.
   * - 0x3C
     - __le16
     - s_errors
     - Behaviour when detecting errors. See super_errors_ for more info.
   * - 0x3E
     - __le16
     - s_minor_rev_level
     - Minor revision level.
   * - 0x40
     - __le32
     - s_lastcheck
     - Time of last check, in seconds since the epoch.
   * - 0x44
     - __le32
     - s_checkinterval
     - Maximum time between checks, in seconds.
   * - 0x48
     - __le32
     - s_creator_os
     - Creator OS. See the table super_creator_ for more info.
   * - 0x4C
     - __le32
     - s_rev_level
     - Revision level. See the table super_revision_ for more info.
   * - 0x50
     - __le16
     - s_def_resuid
     - Default uid for reserved blocks.
   * - 0x52
     - __le16
     - s_def_resgid
     - Default gid for reserved blocks.
   * -
     -
     -
     - These fields are for EXT4_DYNAMIC_REV superblocks only.
       
       .. note::
          the difference between the compatible feature set and the
          incompatible feature set is that if there is a bit set in the
          incompatible feature set that the kernel doesn't know about, it
          should refuse to mount the filesystem.
       
          e2fsck's requirements are more strict; if it doesn't know
          about a feature in either the compatible or incompatible feature set,
          it must abort and not try to meddle with things it doesn't
          understand...
   * - 0x54
     - __le32
     - s_first_ino
     - First non-reserved inode.
   * - 0x58
     - __le16
     - s_inode_size
     - Size of inode structure, in bytes.
   * - 0x5A
     - __le16
     - s_block_group_nr
     - Block group # of this superblock.
   * - 0x5C
     - __le32
     - s_feature_compat
     - Compatible feature set flags. Kernel can still read/write this fs even
       if it doesn't understand a flag; fsck should not do that. See the
       super_compat_ table for more info.
   * - 0x60
     - __le32
     - s_feature_incompat
     - Incompatible feature set. If the kernel or fsck doesn't understand one
       of these bits, it should stop. See the super_incompat_ table for more
       info.
   * - 0x64
     - __le32
     - s_feature_ro_compat
     - Readonly-compatible feature set. If the kernel doesn't understand one of
       these bits, it can still mount read-only. See the super_rocompat_ table
       for more info.
   * - 0x68
     - __u8
     - s_uuid[16]
     - 128-bit UUID for volume.
   * - 0x78
     - char
     - s_volume_name[16]
     - Volume label.
   * - 0x88
     - char
     - s_last_mounted[64]
     - Directory where filesystem was last mounted.
   * - 0xC8
     - __le32
     - s_algorithm_usage_bitmap
     - For compression (Not used in e2fsprogs/Linux)
   * -
     -
     -
     - Performance hints.  Directory preallocation should only happen if the
       EXT4_FEATURE_COMPAT_DIR_PREALLOC flag is on.
   * - 0xCC
     - __u8
     - s_prealloc_blocks
     - #. of blocks to try to preallocate for ... files? (Not used in
       e2fsprogs/Linux)
   * - 0xCD
     - __u8
     - s_prealloc_dir_blocks
     - #. of blocks to preallocate for directories. (Not used in
       e2fsprogs/Linux)
   * - 0xCE
     - __le16
     - s_reserved_gdt_blocks
     - Number of reserved GDT entries for future filesystem expansion.
   * -
     -
     -
     - Journalling support is valid only if EXT4_FEATURE_COMPAT_HAS_JOURNAL is
       set.
   * - 0xD0
     - __u8
     - s_journal_uuid[16]
     - UUID of journal superblock
   * - 0xE0
     - __le32
     - s_journal_inum
     - inode number of journal file.
   * - 0xE4
     - __le32
     - s_journal_dev
     - Device number of journal file, if the external journal feature flag is
       set.
   * - 0xE8
     - __le32
     - s_last_orphan
     - Start of list of orphaned inodes to delete.
   * - 0xEC
     - __le32
     - s_hash_seed[4]
     - HTREE hash seed.
   * - 0xFC
     - __u8
     - s_def_hash_version
     - Default hash algorithm to use for directory hashes. See super_def_hash_
       for more info.
   * - 0xFD
     - __u8
     - s_jnl_backup_type
     - If this value is 0 or EXT3_JNL_BACKUP_BLOCKS (1), then the
       ``s_jnl_blocks`` field contains a duplicate copy of the inode's
       ``i_block[]`` array and ``i_size``.
   * - 0xFE
     - __le16
     - s_desc_size
     - Size of group descriptors, in bytes, if the 64bit incompat feature flag
       is set.
   * - 0x100
     - __le32
     - s_default_mount_opts
     - Default mount options. See the super_mountopts_ table for more info.
   * - 0x104
     - __le32
     - s_first_meta_bg
     - First metablock block group, if the meta_bg feature is enabled.
   * - 0x108
     - __le32
     - s_mkfs_time
     - When the filesystem was created, in seconds since the epoch.
   * - 0x10C
     - __le32
     - s_jnl_blocks[17]
     - Backup copy of the journal inode's ``i_block[]`` array in the first 15
       elements and i_size_high and i_size in the 16th and 17th elements,
       respectively.
   * -
     -
     -
     - 64bit support is valid only if EXT4_FEATURE_COMPAT_64BIT is set.
   * - 0x150
     - __le32
     - s_blocks_count_hi
     - High 32-bits of the block count.
   * - 0x154
     - __le32
     - s_r_blocks_count_hi
     - High 32-bits of the reserved block count.
   * - 0x158
     - __le32
     - s_free_blocks_count_hi
     - High 32-bits of the free block count.
   * - 0x15C
     - __le16
     - s_min_extra_isize
     - All inodes have at least # bytes.
   * - 0x15E
     - __le16
     - s_want_extra_isize
     - New inodes should reserve # bytes.
   * - 0x160
     - __le32
     - s_flags
     - Miscellaneous flags. See the super_flags_ table for more info.
   * - 0x164
     - __le16
     - s_raid_stride
     - RAID stride. This is the number of logical blocks read from or written
       to the disk before moving to the next disk. This affects the placement
       of filesystem metadata, which will hopefully make RAID storage faster.
   * - 0x166
     - __le16
     - s_mmp_interval
     - #. seconds to wait in multi-mount prevention (MMP) checking. In theory,
       MMP is a mechanism to record in the superblock which host and device
       have mounted the filesystem, in order to prevent multiple mounts. This
       feature does not seem to be implemented...
   * - 0x168
     - __le64
     - s_mmp_block
     - Block # for multi-mount protection data.
   * - 0x170
     - __le32
     - s_raid_stripe_width
     - RAID stripe width. This is the number of logical blocks read from or
       written to the disk before coming back to the current disk. This is used
       by the block allocator to try to reduce the number of read-modify-write
       operations in a RAID5/6.
   * - 0x174
     - __u8
     - s_log_groups_per_flex
     - Size of a flexible block group is 2 ^ ``s_log_groups_per_flex``.
   * - 0x175
     - __u8
     - s_checksum_type
     - Metadata checksum algorithm type. The only valid value is 1 (crc32c).
   * - 0x176
     - \_\_u8
     - s\_encryption\_level
     - Versioning level for encryption.
   * - 0x177
     - \_\_u8
     - s\_reserved\_pad
     - Padding to next 32bits.
   * - 0x178
     - __le64
     - s_kbytes_written
     - Number of KiB written to this filesystem over its lifetime.
   * - 0x180
     - __le32
     - s_snapshot_inum
     - inode number of active snapshot. (Not used in e2fsprogs/Linux.)
   * - 0x184
     - __le32
     - s_snapshot_id
     - Sequential ID of active snapshot. (Not used in e2fsprogs/Linux.)
   * - 0x188
     - __le64
     - s_snapshot_r_blocks_count
     - Number of blocks reserved for active snapshot's future use. (Not used in
       e2fsprogs/Linux.)
   * - 0x190
     - __le32
     - s_snapshot_list
     - inode number of the head of the on-disk snapshot list. (Not used in
       e2fsprogs/Linux.)
   * - 0x194
     - __le32
     - s_error_count
     - Number of errors seen.
   * - 0x198
     - __le32
     - s_first_error_time
     - First time an error happened, in seconds since the epoch.
   * - 0x19C
     - __le32
     - s_first_error_ino
     - inode involved in first error.
   * - 0x1A0
     - __le64
     - s_first_error_block
     - Number of block involved of first error.
   * - 0x1A8
     - __u8
     - s_first_error_func[32]
     - Name of function where the error happened.
   * - 0x1C8
     - __le32
     - s_first_error_line
     - Line number where error happened.
   * - 0x1CC
     - __le32
     - s_last_error_time
     - Time of most recent error, in seconds since the epoch.
   * - 0x1D0
     - __le32
     - s_last_error_ino
     - inode involved in most recent error.
   * - 0x1D4
     - __le32
     - s_last_error_line
     - Line number where most recent error happened.
   * - 0x1D8
     - __le64
     - s_last_error_block
     - Number of block involved in most recent error.
   * - 0x1E0
     - __u8
     - s_last_error_func[32]
     - Name of function where the most recent error happened.
   * - 0x200
     - __u8
     - s_mount_opts[64]
     - ASCIIZ string of mount options.
   * - 0x240
     - __le32
     - s_usr_quota_inum
     - Inode number of user `quota <quota>`__ file.
   * - 0x244
     - __le32
     - s_grp_quota_inum
     - Inode number of group `quota <quota>`__ file.
   * - 0x248
     - __le32
     - s_overhead_blocks
     - Overhead blocks/clusters in fs. (Huh? This field is always zero, which
       means that the kernel calculates it dynamically.)
   * - 0x24C
     - __le32
     - s_backup_bgs[2]
     - Block groups containing superblock backups (if sparse_super2)
   * - 0x254
     - __u8
     - s_encrypt_algos[4]
     - Encryption algorithms in use. There can be up to four algorithms in use
       at any time; valid algorithm codes are given in the super_encrypt_ table
       below.
   * - 0x258
     - __u8
     - s_encrypt_pw_salt[16]
     - Salt for the string2key algorithm for encryption.
   * - 0x268
     - __le32
     - s_lpf_ino
     - Inode number of lost+found
   * - 0x26C
     - __le32
     - s_prj_quota_inum
     - Inode that tracks project quotas.
   * - 0x270
     - __le32
     - s_checksum_seed
     - Checksum seed used for metadata_csum calculations. This value is
       crc32c(~0, $orig_fs_uuid).
   * - 0x274
     - __u8
     - s_wtime_hi
     - Upper 8 bits of the s_wtime field.
   * - 0x275
     - __u8
     - s_mtime_hi
     - Upper 8 bits of the s_mtime field.
   * - 0x276
     - __u8
     - s_mkfs_time_hi
     - Upper 8 bits of the s_mkfs_time field.
   * - 0x277
     - __u8
     - s_lastcheck_hi
     - Upper 8 bits of the s_lastcheck field.
   * - 0x278
     - __u8
     - s_first_error_time_hi
     - Upper 8 bits of the s_first_error_time field.
   * - 0x279
     - __u8
     - s_last_error_time_hi
     - Upper 8 bits of the s_last_error_time field.
   * - 0x27A
     - \_\_u8
     - s\_first\_error\_errcode
     -
   * - 0x27B
     - \_\_u8
     - s\_last\_error\_errcode
     -
   * - 0x27C
     - __le16
     - s_encoding
     - Filename charset encoding.
   * - 0x27E
     - __le16
     - s_encoding_flags
     - Filename charset encoding flags.
   * - 0x280
     - __le32
     - s_orphan_file_inum
     - Orphan file inode number.
   * - 0x284
     - __le32
     - s_reserved[94]
     - Padding to the end of the block.
   * - 0x3FC
     - __le32
     - s_checksum
     - Superblock checksum.

.. _super_state:

The superblock state is some combination of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0x0001
     - Cleanly umounted
   * - 0x0002
     - Errors detected
   * - 0x0004
     - Orphans being recovered

.. _super_errors:

The superblock error policy is one of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 1
     - Continue
   * - 2
     - Remount read-only
   * - 3
     - Panic

.. _super_creator:

The filesystem creator is one of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0
     - Linux
   * - 1
     - Hurd
   * - 2
     - Masix
   * - 3
     - FreeBSD
   * - 4
     - Lites

.. _super_revision:

The superblock revision is one of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0
     - Original format
   * - 1
     - v2 format w/ dynamic inode sizes

Note that ``EXT4_DYNAMIC_REV`` refers to a revision 1 or newer filesystem.

.. _super_compat:

The superblock compatible features field is a combination of any of the
following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - Directory preallocation (COMPAT_DIR_PREALLOC).
   * - 0x2
     - “imagic inodes”. Not clear from the code what this does
       (COMPAT_IMAGIC_INODES).
   * - 0x4
     - Has a journal (COMPAT_HAS_JOURNAL).
   * - 0x8
     - Supports extended attributes (COMPAT_EXT_ATTR).
   * - 0x10
     - Has reserved GDT blocks for filesystem expansion
       (COMPAT_RESIZE_INODE). Requires RO_COMPAT_SPARSE_SUPER.
   * - 0x20
     - Has directory indices (COMPAT_DIR_INDEX).
   * - 0x40
     - “Lazy BG”. Not in Linux kernel, seems to have been for uninitialized
       block groups? (COMPAT_LAZY_BG)
   * - 0x80
     - “Exclude inode”. Not used. (COMPAT_EXCLUDE_INODE).
   * - 0x100
     - “Exclude bitmap”. Seems to be used to indicate the presence of
       snapshot-related exclude bitmaps? Not defined in kernel or used in
       e2fsprogs (COMPAT_EXCLUDE_BITMAP).
   * - 0x200
     - Sparse Super Block, v2. If this flag is set, the SB field s_backup_bgs
       points to the two block groups that contain backup superblocks
       (COMPAT_SPARSE_SUPER2).
   * - 0x400
     - Fast commits supported. Although fast commits blocks are
       backward incompatible, fast commit blocks are not always
       present in the journal. If fast commit blocks are present in
       the journal, JBD2 incompat feature
       (JBD2_FEATURE_INCOMPAT_FAST_COMMIT) gets
       set (COMPAT_FAST_COMMIT).
   * - 0x1000
     - Orphan file allocated. This is the special file for more efficient
       tracking of unlinked but still open inodes. When there may be any
       entries in the file, we additionally set proper rocompat feature
       (RO_COMPAT_ORPHAN_PRESENT).

.. _super_incompat:

The superblock incompatible features field is a combination of any of the
following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - Compression (INCOMPAT_COMPRESSION).
   * - 0x2
     - Directory entries record the file type. See ext4_dir_entry_2 below
       (INCOMPAT_FILETYPE).
   * - 0x4
     - Filesystem needs recovery (INCOMPAT_RECOVER).
   * - 0x8
     - Filesystem has a separate journal device (INCOMPAT_JOURNAL_DEV).
   * - 0x10
     - Meta block groups. See the earlier discussion of this feature
       (INCOMPAT_META_BG).
   * - 0x40
     - Files in this filesystem use extents (INCOMPAT_EXTENTS).
   * - 0x80
     - Enable a filesystem size of 2^64 blocks (INCOMPAT_64BIT).
   * - 0x100
     - Multiple mount protection (INCOMPAT_MMP).
   * - 0x200
     - Flexible block groups. See the earlier discussion of this feature
       (INCOMPAT_FLEX_BG).
   * - 0x400
     - Inodes can be used to store large extended attribute values
       (INCOMPAT_EA_INODE).
   * - 0x1000
     - Data in directory entry (INCOMPAT_DIRDATA). (Not implemented?)
   * - 0x2000
     - Metadata checksum seed is stored in the superblock. This feature enables
       the administrator to change the UUID of a metadata_csum filesystem
       while the filesystem is mounted; without it, the checksum definition
       requires all metadata blocks to be rewritten (INCOMPAT_CSUM_SEED).
   * - 0x4000
     - Large directory >2GB or 3-level htree (INCOMPAT_LARGEDIR). Prior to
       this feature, directories could not be larger than 4GiB and could not
       have an htree more than 2 levels deep. If this feature is enabled,
       directories can be larger than 4GiB and have a maximum htree depth of 3.
   * - 0x8000
     - Data in inode (INCOMPAT_INLINE_DATA).
   * - 0x10000
     - Encrypted inodes are present on the filesystem. (INCOMPAT_ENCRYPT).

.. _super_rocompat:

The superblock read-only compatible features field is a combination of any of
the following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - Sparse superblocks. See the earlier discussion of this feature
       (RO_COMPAT_SPARSE_SUPER).
   * - 0x2
     - This filesystem has been used to store a file greater than 2GiB
       (RO_COMPAT_LARGE_FILE).
   * - 0x4
     - Not used in kernel or e2fsprogs (RO_COMPAT_BTREE_DIR).
   * - 0x8
     - This filesystem has files whose sizes are represented in units of
       logical blocks, not 512-byte sectors. This implies a very large file
       indeed! (RO_COMPAT_HUGE_FILE)
   * - 0x10
     - Group descriptors have checksums. In addition to detecting corruption,
       this is useful for lazy formatting with uninitialized groups
       (RO_COMPAT_GDT_CSUM).
   * - 0x20
     - Indicates that the old ext3 32,000 subdirectory limit no longer applies
       (RO_COMPAT_DIR_NLINK). A directory's i_links_count will be set to 1
       if it is incremented past 64,999.
   * - 0x40
     - Indicates that large inodes exist on this filesystem
       (RO_COMPAT_EXTRA_ISIZE).
   * - 0x80
     - This filesystem has a snapshot (RO_COMPAT_HAS_SNAPSHOT).
   * - 0x100
     - `Quota <Quota>`__ (RO_COMPAT_QUOTA).
   * - 0x200
     - This filesystem supports “bigalloc”, which means that file extents are
       tracked in units of clusters (of blocks) instead of blocks
       (RO_COMPAT_BIGALLOC).
   * - 0x400
     - This filesystem supports metadata checksumming.
       (RO_COMPAT_METADATA_CSUM; implies RO_COMPAT_GDT_CSUM, though
       GDT_CSUM must not be set)
   * - 0x800
     - Filesystem supports replicas. This feature is neither in the kernel nor
       e2fsprogs. (RO_COMPAT_REPLICA)
   * - 0x1000
     - Read-only filesystem image; the kernel will not mount this image
       read-write and most tools will refuse to write to the image.
       (RO_COMPAT_READONLY)
   * - 0x2000
     - Filesystem tracks project quotas. (RO_COMPAT_PROJECT)
   * - 0x8000
     - Verity inodes may be present on the filesystem. (RO_COMPAT_VERITY)
   * - 0x10000
     - Indicates orphan file may have valid orphan entries and thus we need
       to clean them up when mounting the filesystem
       (RO_COMPAT_ORPHAN_PRESENT).

.. _super_def_hash:

The ``s_def_hash_version`` field is one of the following:

.. list-table::
   :widths: 8 72
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

.. _super_mountopts:

The ``s_default_mount_opts`` field is any combination of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0x0001
     - Print debugging info upon (re)mount. (EXT4_DEFM_DEBUG)
   * - 0x0002
     - New files take the gid of the containing directory (instead of the fsgid
       of the current process). (EXT4_DEFM_BSDGROUPS)
   * - 0x0004
     - Support userspace-provided extended attributes. (EXT4_DEFM_XATTR_USER)
   * - 0x0008
     - Support POSIX access control lists (ACLs). (EXT4_DEFM_ACL)
   * - 0x0010
     - Do not support 32-bit UIDs. (EXT4_DEFM_UID16)
   * - 0x0020
     - All data and metadata are committed to the journal.
       (EXT4_DEFM_JMODE_DATA)
   * - 0x0040
     - All data are flushed to the disk before metadata are committed to the
       journal. (EXT4_DEFM_JMODE_ORDERED)
   * - 0x0060
     - Data ordering is not preserved; data may be written after the metadata
       has been written. (EXT4_DEFM_JMODE_WBACK)
   * - 0x0100
     - Disable write flushes. (EXT4_DEFM_NOBARRIER)
   * - 0x0200
     - Track which blocks in a filesystem are metadata and therefore should not
       be used as data blocks. This option will be enabled by default on 3.18,
       hopefully. (EXT4_DEFM_BLOCK_VALIDITY)
   * - 0x0400
     - Enable DISCARD support, where the storage device is told about blocks
       becoming unused. (EXT4_DEFM_DISCARD)
   * - 0x0800
     - Disable delayed allocation. (EXT4_DEFM_NODELALLOC)

.. _super_flags:

The ``s_flags`` field is any combination of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0x0001
     - Signed directory hash in use.
   * - 0x0002
     - Unsigned directory hash in use.
   * - 0x0004
     - To test development code.

.. _super_encrypt:

The ``s_encrypt_algos`` list can contain any of the following:

.. list-table::
   :widths: 8 72
   :header-rows: 1

   * - Value
     - Description
   * - 0
     - Invalid algorithm (ENCRYPTION_MODE_INVALID).
   * - 1
     - 256-bit AES in XTS mode (ENCRYPTION_MODE_AES_256_XTS).
   * - 2
     - 256-bit AES in GCM mode (ENCRYPTION_MODE_AES_256_GCM).
   * - 3
     - 256-bit AES in CBC mode (ENCRYPTION_MODE_AES_256_CBC).

Total size of the superblock is 1024 bytes.

Block Group Descriptors
-----------------------

Each block group on the filesystem has one of these descriptors
associated with it. As noted in the Layout section above, the group
descriptors (if present) are the second item in the block group. The
standard configuration is for each block group to contain a full copy of
the block group descriptor table unless the sparse_super feature flag
is set.

Notice how the group descriptor records the location of both bitmaps and
the inode table (i.e. they can float). This means that within a block
group, the only data structures with fixed locations are the superblock
and the group descriptor table. The flex_bg mechanism uses this
property to group several block groups into a flex group and lay out all
of the groups' bitmaps and inode tables into one long run in the first
group of the flex group.

If the meta_bg feature flag is set, then several block groups are
grouped together into a meta group. Note that in the meta_bg case,
however, the first and last two block groups within the larger meta
group contain only group descriptors for the groups inside the meta
group.

flex_bg and meta_bg do not appear to be mutually exclusive features.

In ext2, ext3, and ext4 (when the 64bit feature is not enabled), the
block group descriptor was only 32 bytes long and therefore ends at
bg_checksum. On an ext4 filesystem with the 64bit feature enabled, the
block group descriptor expands to at least the 64 bytes described below;
the size is stored in the superblock.

If gdt_csum is set and metadata_csum is not set, the block group
checksum is the crc16 of the FS UUID, the group number, and the group
descriptor structure. If metadata_csum is set, then the block group
checksum is the lower 16 bits of the checksum of the FS UUID, the group
number, and the group descriptor structure. Both block and inode bitmap
checksums are calculated against the FS UUID, the group number, and the
entire bitmap.

The block group descriptor is laid out in ``struct ext4_group_desc``.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Size
     - Name
     - Description
   * - 0x0
     - __le32
     - bg_block_bitmap_lo
     - Lower 32-bits of location of block bitmap.
   * - 0x4
     - __le32
     - bg_inode_bitmap_lo
     - Lower 32-bits of location of inode bitmap.
   * - 0x8
     - __le32
     - bg_inode_table_lo
     - Lower 32-bits of location of inode table.
   * - 0xC
     - __le16
     - bg_free_blocks_count_lo
     - Lower 16-bits of free block count.
   * - 0xE
     - __le16
     - bg_free_inodes_count_lo
     - Lower 16-bits of free inode count.
   * - 0x10
     - __le16
     - bg_used_dirs_count_lo
     - Lower 16-bits of directory count.
   * - 0x12
     - __le16
     - bg_flags
     - Block group flags. See the bgflags_ table below.
   * - 0x14
     - __le32
     - bg_exclude_bitmap_lo
     - Lower 32-bits of location of snapshot exclusion bitmap.
   * - 0x18
     - __le16
     - bg_block_bitmap_csum_lo
     - Lower 16-bits of the block bitmap checksum.
   * - 0x1A
     - __le16
     - bg_inode_bitmap_csum_lo
     - Lower 16-bits of the inode bitmap checksum.
   * - 0x1C
     - __le16
     - bg_itable_unused_lo
     - Lower 16-bits of unused inode count. If set, we needn't scan past the
       ``(sb.s_inodes_per_group - gdt.bg_itable_unused)`` th entry in the
       inode table for this group.
   * - 0x1E
     - __le16
     - bg_checksum
     - Group descriptor checksum; crc16(sb_uuid+group_num+bg_desc) if the
       RO_COMPAT_GDT_CSUM feature is set, or
       crc32c(sb_uuid+group_num+bg_desc) & 0xFFFF if the
       RO_COMPAT_METADATA_CSUM feature is set.  The bg_checksum
       field in bg_desc is skipped when calculating crc16 checksum,
       and set to zero if crc32c checksum is used.
   * -
     -
     -
     - These fields only exist if the 64bit feature is enabled and s_desc_size
       > 32.
   * - 0x20
     - __le32
     - bg_block_bitmap_hi
     - Upper 32-bits of location of block bitmap.
   * - 0x24
     - __le32
     - bg_inode_bitmap_hi
     - Upper 32-bits of location of inodes bitmap.
   * - 0x28
     - __le32
     - bg_inode_table_hi
     - Upper 32-bits of location of inodes table.
   * - 0x2C
     - __le16
     - bg_free_blocks_count_hi
     - Upper 16-bits of free block count.
   * - 0x2E
     - __le16
     - bg_free_inodes_count_hi
     - Upper 16-bits of free inode count.
   * - 0x30
     - __le16
     - bg_used_dirs_count_hi
     - Upper 16-bits of directory count.
   * - 0x32
     - __le16
     - bg_itable_unused_hi
     - Upper 16-bits of unused inode count.
   * - 0x34
     - __le32
     - bg_exclude_bitmap_hi
     - Upper 32-bits of location of snapshot exclusion bitmap.
   * - 0x38
     - __le16
     - bg_block_bitmap_csum_hi
     - Upper 16-bits of the block bitmap checksum.
   * - 0x3A
     - __le16
     - bg_inode_bitmap_csum_hi
     - Upper 16-bits of the inode bitmap checksum.
   * - 0x3C
     - __u32
     - bg_reserved
     - Padding to 64 bytes.

.. _bgflags:

Block group flags can be any combination of the following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - inode table and bitmap are not initialized (EXT4_BG_INODE_UNINIT).
   * - 0x2
     - block bitmap is not initialized (EXT4_BG_BLOCK_UNINIT).
   * - 0x4
     - inode table is zeroed (EXT4_BG_INODE_ZEROED).

Block and inode Bitmaps
-----------------------

The data block bitmap tracks the usage of data blocks within the block
group.

The inode bitmap records which entries in the inode table are in use.

As with most bitmaps, one bit represents the usage status of one data
block or inode table entry. This implies a block group size of 8 *
number_of_bytes_in_a_logical_block.

.. note::
   If ``BLOCK_UNINIT`` is set for a given block group, various parts
   of the kernel and e2fsprogs code pretends that the block bitmap contains
   zeros (i.e. all blocks in the group are free). However, it is not
   necessarily the case that no blocks are in use -- if ``meta_bg`` is set,
   the bitmaps and group descriptor live inside the group. Unfortunately,
   ext2fs_test_block_bitmap2() will return '0' for those locations,
   which produces confusing debugfs output.

Inode Table
-----------
Inode tables are statically allocated at mkfs time.  Each block group
descriptor points to the start of the table, and the superblock records
the number of inodes per group.  See the section on inodes for more
information.

Multiple Mount Protection
-------------------------

Multiple mount protection (MMP) is a feature that protects the
filesystem against multiple hosts trying to use the filesystem
simultaneously. When a filesystem is opened (for mounting, or fsck,
etc.), the MMP code running on the node (call it node A) checks a
sequence number. If the sequence number is EXT4_MMP_SEQ_CLEAN, the
open continues. If the sequence number is EXT4_MMP_SEQ_FSCK, then
fsck is (hopefully) running, and open fails immediately. Otherwise, the
open code will wait for twice the specified MMP check interval and check
the sequence number again. If the sequence number has changed, then the
filesystem is active on another machine and the open fails. If the MMP
code passes all of those checks, a new MMP sequence number is generated
and written to the MMP block, and the mount proceeds.

While the filesystem is live, the kernel sets up a timer to re-check the
MMP block at the specified MMP check interval. To perform the re-check,
the MMP sequence number is re-read; if it does not match the in-memory
MMP sequence number, then another node (node B) has mounted the
filesystem, and node A remounts the filesystem read-only. If the
sequence numbers match, the sequence number is incremented both in
memory and on disk, and the re-check is complete.

The hostname and device filename are written into the MMP block whenever
an open operation succeeds. The MMP code does not use these values; they
are provided purely for informational purposes.

The checksum is calculated against the FS UUID and the MMP structure.
The MMP structure (``struct mmp_struct``) is as follows:

.. list-table::
   :widths: 8 12 20 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __le32
     - mmp_magic
     - Magic number for MMP, 0x004D4D50 (“MMP”).
   * - 0x4
     - __le32
     - mmp_seq
     - Sequence number, updated periodically.
   * - 0x8
     - __le64
     - mmp_time
     - Time that the MMP block was last updated.
   * - 0x10
     - char[64]
     - mmp_nodename
     - Hostname of the node that opened the filesystem.
   * - 0x50
     - char[32]
     - mmp_bdevname
     - Block device name of the filesystem.
   * - 0x70
     - __le16
     - mmp_check_interval
     - The MMP re-check interval, in seconds.
   * - 0x72
     - __le16
     - mmp_pad1
     - Zero.
   * - 0x74
     - __le32[226]
     - mmp_pad2
     - Zero.
   * - 0x3FC
     - __le32
     - mmp_checksum
     - Checksum of the MMP block.

Journal (jbd2)
--------------

Introduced in ext3, the ext4 filesystem employs a journal to protect the
filesystem against metadata inconsistencies in the case of a system crash. Up
to 10,240,000 file system blocks (see man mke2fs(8) for more details on journal
size limits) can be reserved inside the filesystem as a place to land
“important” data writes on-disk as quickly as possible. Once the important
data transaction is fully written to the disk and flushed from the disk write
cache, a record of the data being committed is also written to the journal. At
some later point in time, the journal code writes the transactions to their
final locations on disk (this could involve a lot of seeking or a lot of small
read-write-erases) before erasing the commit record. Should the system
crash during the second slow write, the journal can be replayed all the
way to the latest commit record, guaranteeing the atomicity of whatever
gets written through the journal to the disk. The effect of this is to
guarantee that the filesystem does not become stuck midway through a
metadata update.

For performance reasons, ext4 by default only writes filesystem metadata
through the journal. This means that file data blocks are /not/
guaranteed to be in any consistent state after a crash. If this default
guarantee level (``data=ordered``) is not satisfactory, there is a mount
option to control journal behavior. If ``data=journal``, all data and
metadata are written to disk through the journal. This is slower but
safest. If ``data=writeback``, dirty data blocks are not flushed to the
disk before the metadata are written to disk through the journal.

In case of ``data=ordered`` mode, Ext4 also supports fast commits which
help reduce commit latency significantly. The default ``data=ordered``
mode works by logging metadata blocks to the journal. In fast commit
mode, Ext4 only stores the minimal delta needed to recreate the
affected metadata in fast commit space that is shared with JBD2.
Once the fast commit area fills in or if fast commit is not possible
or if JBD2 commit timer goes off, Ext4 performs a traditional full commit.
A full commit invalidates all the fast commits that happened before
it and thus it makes the fast commit area empty for further fast
commits. This feature needs to be enabled at mkfs time.

The journal inode is typically inode 8. The first 68 bytes of the
journal inode are replicated in the ext4 superblock. The journal itself
is normal (but hidden) file within the filesystem. The file usually
consumes an entire block group, though mke2fs tries to put it in the
middle of the disk.

All fields in jbd2 are written to disk in big-endian order. This is the
opposite of ext4.

.. note:: Both ext4 and ocfs2 use jbd2.

The maximum size of a journal embedded in an ext4 filesystem is 2^32
blocks. jbd2 itself does not seem to care.

Layout
~~~~~~

Generally speaking, the journal has this format:

.. list-table::
   :widths: 16 48 16
   :header-rows: 1

   * - Superblock
     - descriptor_block (data_blocks or revocation_block) [more data or
       revocations] commmit_block
     - [more transactions...]
   * - 
     - One transaction
     -

Notice that a transaction begins with either a descriptor and some data,
or a block revocation list. A finished transaction always ends with a
commit. If there is no commit record (or the checksums don't match), the
transaction will be discarded during replay.

External Journal
~~~~~~~~~~~~~~~~

Optionally, an ext4 filesystem can be created with an external journal
device (as opposed to an internal journal, which uses a reserved inode).
In this case, on the filesystem device, ``s_journal_inum`` should be
zero and ``s_journal_uuid`` should be set. On the journal device there
will be an ext4 super block in the usual place, with a matching UUID.
The journal superblock will be in the next full block after the
superblock.

.. list-table::
   :widths: 12 12 12 32 12
   :header-rows: 1

   * - 1024 bytes of padding
     - ext4 Superblock
     - Journal Superblock
     - descriptor_block (data_blocks or revocation_block) [more data or
       revocations] commmit_block
     - [more transactions...]
   * - 
     -
     -
     - One transaction
     -

Block Header
~~~~~~~~~~~~

Every block in the journal starts with a common 12-byte header
``struct journal_header_s``:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __be32
     - h_magic
     - jbd2 magic number, 0xC03B3998.
   * - 0x4
     - __be32
     - h_blocktype
     - Description of what this block contains. See the jbd2_blocktype_ table
       below.
   * - 0x8
     - __be32
     - h_sequence
     - The transaction ID that goes with this block.

.. _jbd2_blocktype:

The journal block type can be any one of:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 1
     - Descriptor. This block precedes a series of data blocks that were
       written through the journal during a transaction.
   * - 2
     - Block commit record. This block signifies the completion of a
       transaction.
   * - 3
     - Journal superblock, v1.
   * - 4
     - Journal superblock, v2.
   * - 5
     - Block revocation records. This speeds up recovery by enabling the
       journal to skip writing blocks that were subsequently rewritten.

Super Block
~~~~~~~~~~~

The super block for the journal is much simpler as compared to ext4's.
The key data kept within are size of the journal, and where to find the
start of the log of transactions.

The journal superblock is recorded as ``struct journal_superblock_s``,
which is 1024 bytes long:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * -
     -
     -
     - Static information describing the journal.
   * - 0x0
     - journal_header_t (12 bytes)
     - s_header
     - Common header identifying this as a superblock.
   * - 0xC
     - __be32
     - s_blocksize
     - Journal device block size.
   * - 0x10
     - __be32
     - s_maxlen
     - Total number of blocks in this journal.
   * - 0x14
     - __be32
     - s_first
     - First block of log information.
   * -
     -
     -
     - Dynamic information describing the current state of the log.
   * - 0x18
     - __be32
     - s_sequence
     - First commit ID expected in log.
   * - 0x1C
     - __be32
     - s_start
     - Block number of the start of log. Contrary to the comments, this field
       being zero does not imply that the journal is clean!
   * - 0x20
     - __be32
     - s_errno
     - Error value, as set by jbd2_journal_abort().
   * -
     -
     -
     - The remaining fields are only valid in a v2 superblock.
   * - 0x24
     - __be32
     - s_feature_compat;
     - Compatible feature set. See the table jbd2_compat_ below.
   * - 0x28
     - __be32
     - s_feature_incompat
     - Incompatible feature set. See the table jbd2_incompat_ below.
   * - 0x2C
     - __be32
     - s_feature_ro_compat
     - Read-only compatible feature set. There aren't any of these currently.
   * - 0x30
     - __u8
     - s_uuid[16]
     - 128-bit uuid for journal. This is compared against the copy in the ext4
       super block at mount time.
   * - 0x40
     - __be32
     - s_nr_users
     - Number of file systems sharing this journal.
   * - 0x44
     - __be32
     - s_dynsuper
     - Location of dynamic super block copy. (Not used?)
   * - 0x48
     - __be32
     - s_max_transaction
     - Limit of journal blocks per transaction. (Not used?)
   * - 0x4C
     - __be32
     - s_max_trans_data
     - Limit of data blocks per transaction. (Not used?)
   * - 0x50
     - __u8
     - s_checksum_type
     - Checksum algorithm used for the journal.  See jbd2_checksum_type_ for
       more info.
   * - 0x51
     - __u8[3]
     - s_padding2
     -
   * - 0x54
     - __be32
     - s_num_fc_blocks
     - Number of fast commit blocks in the journal.
   * - 0x58
     - __be32
     - s_head
     - Block number of the head (first unused block) of the journal, only
       up-to-date when the journal is empty.
   * - 0x5C
     - __u32
     - s_padding[40]
     -
   * - 0xFC
     - __be32
     - s_checksum
     - Checksum of the entire superblock, with this field set to zero.
   * - 0x100
     - __u8
     - s_users[16*48]
     - ids of all file systems sharing the log. e2fsprogs/Linux don't allow
       shared external journals, but I imagine Lustre (or ocfs2?), which use
       the jbd2 code, might.

.. _jbd2_compat:

The journal compat features are any combination of the following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - Journal maintains checksums on the data blocks.
       (JBD2_FEATURE_COMPAT_CHECKSUM)

.. _jbd2_incompat:

The journal incompat features are any combination of the following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - Journal has block revocation records. (JBD2_FEATURE_INCOMPAT_REVOKE)
   * - 0x2
     - Journal can deal with 64-bit block numbers.
       (JBD2_FEATURE_INCOMPAT_64BIT)
   * - 0x4
     - Journal commits asynchronously. (JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT)
   * - 0x8
     - This journal uses v2 of the checksum on-disk format. Each journal
       metadata block gets its own checksum, and the block tags in the
       descriptor table contain checksums for each of the data blocks in the
       journal. (JBD2_FEATURE_INCOMPAT_CSUM_V2)
   * - 0x10
     - This journal uses v3 of the checksum on-disk format. This is the same as
       v2, but the journal block tag size is fixed regardless of the size of
       block numbers. (JBD2_FEATURE_INCOMPAT_CSUM_V3)
   * - 0x20
     - Journal has fast commit blocks. (JBD2_FEATURE_INCOMPAT_FAST_COMMIT)

.. _jbd2_checksum_type:

Journal checksum type codes are one of the following.  crc32 or crc32c are the
most likely choices.

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 1
     - CRC32
   * - 2
     - MD5
   * - 3
     - SHA1
   * - 4
     - CRC32C

Descriptor Block
~~~~~~~~~~~~~~~~

The descriptor block contains an array of journal block tags that
describe the final locations of the data blocks that follow in the
journal. Descriptor blocks are open-coded instead of being completely
described by a data structure, but here is the block structure anyway.
Descriptor blocks consume at least 36 bytes, but use a full block:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Descriptor
   * - 0x0
     - journal_header_t
     - (open coded)
     - Common block header.
   * - 0xC
     - struct journal_block_tag_s
     - open coded array[]
     - Enough tags either to fill up the block or to describe all the data
       blocks that follow this descriptor block.

Journal block tags have any of the following formats, depending on which
journal feature and block tag flags are set.

If JBD2_FEATURE_INCOMPAT_CSUM_V3 is set, the journal block tag is
defined as ``struct journal_block_tag3_s``, which looks like the
following. The size is 16 or 32 bytes.

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Descriptor
   * - 0x0
     - __be32
     - t_blocknr
     - Lower 32-bits of the location of where the corresponding data block
       should end up on disk.
   * - 0x4
     - __be32
     - t_flags
     - Flags that go with the descriptor. See the table jbd2_tag_flags_ for
       more info.
   * - 0x8
     - __be32
     - t_blocknr_high
     - Upper 32-bits of the location of where the corresponding data block
       should end up on disk. This is zero if JBD2_FEATURE_INCOMPAT_64BIT is
       not enabled.
   * - 0xC
     - __be32
     - t_checksum
     - Checksum of the journal UUID, the sequence number, and the data block.
   * -
     -
     -
     - This field appears to be open coded. It always comes at the end of the
       tag, after t_checksum. This field is not present if the "same UUID" flag
       is set.
   * - 0x8 or 0xC
     - char
     - uuid[16]
     - A UUID to go with this tag. This field appears to be copied from the
       ``j_uuid`` field in ``struct journal_s``, but only tune2fs touches that
       field.

.. _jbd2_tag_flags:

The journal tag flags are any combination of the following:

.. list-table::
   :widths: 16 64
   :header-rows: 1

   * - Value
     - Description
   * - 0x1
     - On-disk block is escaped. The first four bytes of the data block just
       happened to match the jbd2 magic number.
   * - 0x2
     - This block has the same UUID as previous, therefore the UUID field is
       omitted.
   * - 0x4
     - The data block was deleted by the transaction. (Not used?)
   * - 0x8
     - This is the last tag in this descriptor block.

If JBD2_FEATURE_INCOMPAT_CSUM_V3 is NOT set, the journal block tag
is defined as ``struct journal_block_tag_s``, which looks like the
following. The size is 8, 12, 24, or 28 bytes:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Descriptor
   * - 0x0
     - __be32
     - t_blocknr
     - Lower 32-bits of the location of where the corresponding data block
       should end up on disk.
   * - 0x4
     - __be16
     - t_checksum
     - Checksum of the journal UUID, the sequence number, and the data block.
       Note that only the lower 16 bits are stored.
   * - 0x6
     - __be16
     - t_flags
     - Flags that go with the descriptor. See the table jbd2_tag_flags_ for
       more info.
   * -
     -
     -
     - This next field is only present if the super block indicates support for
       64-bit block numbers.
   * - 0x8
     - __be32
     - t_blocknr_high
     - Upper 32-bits of the location of where the corresponding data block
       should end up on disk.
   * -
     -
     -
     - This field appears to be open coded. It always comes at the end of the
       tag, after t_flags or t_blocknr_high. This field is not present if the
       "same UUID" flag is set.
   * - 0x8 or 0xC
     - char
     - uuid[16]
     - A UUID to go with this tag. This field appears to be copied from the
       ``j_uuid`` field in ``struct journal_s``, but only tune2fs touches that
       field.

If JBD2_FEATURE_INCOMPAT_CSUM_V2 or
JBD2_FEATURE_INCOMPAT_CSUM_V3 are set, the end of the block is a
``struct jbd2_journal_block_tail``, which looks like this:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Descriptor
   * - 0x0
     - __be32
     - t_checksum
     - Checksum of the journal UUID + the descriptor block, with this field set
       to zero.

Data Block
~~~~~~~~~~

In general, the data blocks being written to disk through the journal
are written verbatim into the journal file after the descriptor block.
However, if the first four bytes of the block match the jbd2 magic
number then those four bytes are replaced with zeroes and the “escaped”
flag is set in the descriptor block tag.

Revocation Block
~~~~~~~~~~~~~~~~

A revocation block is used to prevent replay of a block in an earlier
transaction. This is used to mark blocks that were journalled at one
time but are no longer journalled. Typically this happens if a metadata
block is freed and re-allocated as a file data block; in this case, a
journal replay after the file block was written to disk will cause
corruption.

.. note::
   This mechanism is NOT used to express “this journal block is
   superseded by this other journal block”, as the author (djwong)
   mistakenly thought. Any block being added to a transaction will cause
   the removal of all existing revocation records for that block.

Revocation blocks are described in
``struct jbd2_journal_revoke_header_s``, are at least 16 bytes in
length, but use a full block:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - journal_header_t
     - r_header
     - Common block header.
   * - 0xC
     - __be32
     - r_count
     - Number of bytes used in this block.
   * - 0x10
     - __be32 or __be64
     - blocks[0]
     - Blocks to revoke.

After r_count is a linear array of block numbers that are effectively
revoked by this transaction. The size of each block number is 8 bytes if
the superblock advertises 64-bit block number support, or 4 bytes
otherwise.

If JBD2_FEATURE_INCOMPAT_CSUM_V2 or
JBD2_FEATURE_INCOMPAT_CSUM_V3 are set, the end of the revocation
block is a ``struct jbd2_journal_revoke_tail``, which has this format:

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Description
   * - 0x0
     - __be32
     - r_checksum
     - Checksum of the journal UUID + revocation block

Commit Block
~~~~~~~~~~~~

The commit block is a sentry that indicates that a transaction has been
completely written to the journal. Once this commit block reaches the
journal, the data stored with this transaction can be written to their
final locations on disk.

The commit block is described by ``struct commit_header``, which is 32
bytes long (but uses a full block):

.. list-table::
   :widths: 8 8 24 40
   :header-rows: 1

   * - Offset
     - Type
     - Name
     - Descriptor
   * - 0x0
     - journal_header_s
     - (open coded)
     - Common block header.
   * - 0xC
     - unsigned char
     - h_chksum_type
     - The type of checksum to use to verify the integrity of the data blocks
       in the transaction. See jbd2_checksum_type_ for more info.
   * - 0xD
     - unsigned char
     - h_chksum_size
     - The number of bytes used by the checksum. Most likely 4.
   * - 0xE
     - unsigned char
     - h_padding[2]
     -
   * - 0x10
     - __be32
     - h_chksum[JBD2_CHECKSUM_BYTES]
     - 32 bytes of space to store checksums. If
       JBD2_FEATURE_INCOMPAT_CSUM_V2 or JBD2_FEATURE_INCOMPAT_CSUM_V3
       are set, the first ``__be32`` is the checksum of the journal UUID and
       the entire commit block, with this field zeroed. If
       JBD2_FEATURE_COMPAT_CHECKSUM is set, the first ``__be32`` is the
       crc32 of all the blocks already written to the transaction.
   * - 0x30
     - __be64
     - h_commit_sec
     - The time that the transaction was committed, in seconds since the epoch.
   * - 0x38
     - __be32
     - h_commit_nsec
     - Nanoseconds component of the above timestamp.

Fast commits
~~~~~~~~~~~~

Fast commit area is organized as a log of tag length values. Each TLV has
a ``struct ext4_fc_tl`` in the beginning which stores the tag and the length
of the entire field. It is followed by variable length tag specific value.
Here is the list of supported tags and their meanings:

.. list-table::
   :widths: 8 20 20 32
   :header-rows: 1

   * - Tag
     - Meaning
     - Value struct
     - Description
   * - EXT4_FC_TAG_HEAD
     - Fast commit area header
     - ``struct ext4_fc_head``
     - Stores the TID of the transaction after which these fast commits should
       be applied.
   * - EXT4_FC_TAG_ADD_RANGE
     - Add extent to inode
     - ``struct ext4_fc_add_range``
     - Stores the inode number and extent to be added in this inode
   * - EXT4_FC_TAG_DEL_RANGE
     - Remove logical offsets to inode
     - ``struct ext4_fc_del_range``
     - Stores the inode number and the logical offset range that needs to be
       removed
   * - EXT4_FC_TAG_CREAT
     - Create directory entry for a newly created file
     - ``struct ext4_fc_dentry_info``
     - Stores the parent inode number, inode number and directory entry of the
       newly created file
   * - EXT4_FC_TAG_LINK
     - Link a directory entry to an inode
     - ``struct ext4_fc_dentry_info``
     - Stores the parent inode number, inode number and directory entry
   * - EXT4_FC_TAG_UNLINK
     - Unlink a directory entry of an inode
     - ``struct ext4_fc_dentry_info``
     - Stores the parent inode number, inode number and directory entry

   * - EXT4_FC_TAG_PAD
     - Padding (unused area)
     - None
     - Unused bytes in the fast commit area.

   * - EXT4_FC_TAG_TAIL
     - Mark the end of a fast commit
     - ``struct ext4_fc_tail``
     - Stores the TID of the commit, CRC of the fast commit of which this tag
       represents the end of

Fast Commit Replay Idempotence
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Fast commits tags are idempotent in nature provided the recovery code follows
certain rules. The guiding principle that the commit path follows while
committing is that it stores the result of a particular operation instead of
storing the procedure.

Let's consider this rename operation: 'mv /a /b'. Let's assume dirent '/a'
was associated with inode 10. During fast commit, instead of storing this
operation as a procedure "rename a to b", we store the resulting file system
state as a "series" of outcomes:

- Link dirent b to inode 10
- Unlink dirent a
- Inode 10 with valid refcount

Now when recovery code runs, it needs "enforce" this state on the file
system. This is what guarantees idempotence of fast commit replay.

Let's take an example of a procedure that is not idempotent and see how fast
commits make it idempotent. Consider following sequence of operations:

1) rm A
2) mv B A
3) read A

If we store this sequence of operations as is then the replay is not idempotent.
Let's say while in replay, we crash after (2). During the second replay,
file A (which was actually created as a result of "mv B A" operation) would get
deleted. Thus, file named A would be absent when we try to read A. So, this
sequence of operations is not idempotent. However, as mentioned above, instead
of storing the procedure fast commits store the outcome of each procedure. Thus
the fast commit log for above procedure would be as follows:

(Let's assume dirent A was linked to inode 10 and dirent B was linked to
inode 11 before the replay)

1) Unlink A
2) Link A to inode 11
3) Unlink B
4) Inode 11

If we crash after (3) we will have file A linked to inode 11. During the second
replay, we will remove file A (inode 11). But we will create it back and make
it point to inode 11. We won't find B, so we'll just skip that step. At this
point, the refcount for inode 11 is not reliable, but that gets fixed by the
replay of last inode 11 tag. Thus, by converting a non-idempotent procedure
into a series of idempotent outcomes, fast commits ensured idempotence during
the replay.

Journal Checkpoint
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Checkpointing the journal ensures all transactions and their associated buffers
are submitted to the disk. In-progress transactions are waited upon and included
in the checkpoint. Checkpointing is used internally during critical updates to
the filesystem including journal recovery, filesystem resizing, and freeing of
the journal_t structure.

A journal checkpoint can be triggered from userspace via the ioctl
EXT4_IOC_CHECKPOINT. This ioctl takes a single, u64 argument for flags.
Currently, three flags are supported. First, EXT4_IOC_CHECKPOINT_FLAG_DRY_RUN
can be used to verify input to the ioctl. It returns error if there is any
invalid input, otherwise it returns success without performing
any checkpointing. This can be used to check whether the ioctl exists on a
system and to verify there are no issues with arguments or flags. The
other two flags are EXT4_IOC_CHECKPOINT_FLAG_DISCARD and
EXT4_IOC_CHECKPOINT_FLAG_ZEROOUT. These flags cause the journal blocks to be
discarded or zero-filled, respectively, after the journal checkpoint is
complete. EXT4_IOC_CHECKPOINT_FLAG_DISCARD and EXT4_IOC_CHECKPOINT_FLAG_ZEROOUT
cannot both be set. The ioctl may be useful when snapshotting a system or for
complying with content deletion SLOs.

Orphan file
-----------

In unix there can inodes that are unlinked from directory hierarchy but that
are still alive because they are open. In case of crash the filesystem has to
clean up these inodes as otherwise they (and the blocks referenced from them)
would leak. Similarly if we truncate or extend the file, we need not be able
to perform the operation in a single journalling transaction. In such case we
track the inode as orphan so that in case of crash extra blocks allocated to
the file get truncated.

Traditionally ext4 tracks orphan inodes in a form of single linked list where
superblock contains the inode number of the last orphan inode (s_last_orphan
field) and then each inode contains inode number of the previously orphaned
inode (we overload i_dtime inode field for this). However this filesystem
global single linked list is a scalability bottleneck for workloads that result
in heavy creation of orphan inodes. When orphan file feature
(COMPAT_ORPHAN_FILE) is enabled, the filesystem has a special inode
(referenced from the superblock through s_orphan_file_inum) with several
blocks. Each of these blocks has a structure:

============= ================ =============== ===============================
Offset        Type             Name            Description
============= ================ =============== ===============================
0x0           Array of         Orphan inode    Each __le32 entry is either
              __le32 entries   entries         empty (0) or it contains
	                                       inode number of an orphan
					       inode.
blocksize-8   __le32           ob_magic        Magic value stored in orphan
                                               block tail (0x0b10ca04)
blocksize-4   __le32           ob_checksum     Checksum of the orphan block.
============= ================ =============== ===============================

When a filesystem with orphan file feature is writeably mounted, we set
RO_COMPAT_ORPHAN_PRESENT feature in the superblock to indicate there may
be valid orphan entries. In case we see this feature when mounting the
filesystem, we read the whole orphan file and process all orphan inodes found
there as usual. When cleanly unmounting the filesystem we remove the
RO_COMPAT_ORPHAN_PRESENT feature to avoid unnecessary scanning of the orphan
file and also make the filesystem fully compatible with older kernels.
