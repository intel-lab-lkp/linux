/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DRBD_META_DATA_H
#define DRBD_META_DATA_H

/* how I came up with this magic?
 * base64 decode "actlog==" ;) */
#define DRBD_AL_MAGIC 0x69cb65a2

#define BM_BLOCK_SHIFT_4k	12			 /* 4k per bit */
#define BM_BLOCK_SHIFT_MIN	BM_BLOCK_SHIFT_4k
#define BM_BLOCK_SHIFT_MAX	20
#define BM_BLOCK_SIZE_4k	4096
#define BM_BLOCK_SIZE_MIN	(1<<BM_BLOCK_SHIFT_MIN)
#define BM_BLOCK_SIZE_MAX	(1<<BM_BLOCK_SHIFT_MAX)

struct peer_dev_md_on_disk_9 {
	__be64 bitmap_uuid;
	__be64 bitmap_dagtag;
	__be32 flags;
	__be32 bitmap_index;
	__be32 reserved_u32[2];
} __packed;

struct meta_data_on_disk_9 {
	__be64 effective_size;    /* last agreed size */
	__be64 current_uuid;
	__be64 members;		  /* only if MDF_HAVE_MEMBERS_MASK is in the flags */
	__be64 reserved_u64[3];   /* to have the magic at the same position as in v07, and v08 */
	__be64 device_uuid;
	__be32 flags;             /* MDF */
	__be32 magic;
	__be32 md_size_sect;
	__be32 al_offset;         /* offset to this block */
	__be32 al_nr_extents;     /* important for restoring the AL */
	__be32 bm_offset;         /* offset to the bitmap, from here */
	__be32 bm_bytes_per_bit;  /* BM_BLOCK_SIZE */
	__be32 la_peer_max_bio_size;   /* last peer max_bio_size */
	__be32 bm_max_peers;
	__be32 node_id;

	/* see al_tr_number_to_on_disk_sector() */
	__be32 al_stripes;
	__be32 al_stripe_size_4k;

	__be32 reserved_u32[2];

	struct peer_dev_md_on_disk_9 peers[DRBD_PEERS_MAX];
	__be64 history_uuids[HISTORY_UUIDS];

	unsigned char padding_start[0];
	unsigned char padding_end[0] __aligned(4096);
} __packed;

/* Attention, these two are defined in drbd_int.h as well! */
#define AL_UPDATES_PER_TRANSACTION 64
#define AL_CONTEXT_PER_TRANSACTION 919

enum al_transaction_types {
	AL_TR_UPDATE = 0,
	AL_TR_INITIALIZED = 0xffff
};
/* all fields on disc in big endian */
struct __packed al_transaction_on_disk {
	/* don't we all like magic */
	__be32	magic;

	/* to identify the most recent transaction block
	 * in the on disk ring buffer */
	__be32	tr_number;

	/* checksum on the full 4k block, with this field set to 0. */
	__be32	crc32c;

	/* type of transaction, special transaction types like:
	 * purge-all, set-all-idle, set-all-active, ... to-be-defined
	 * see also enum al_transaction_types */
	__be16	transaction_type;

	/* we currently allow only a few thousand extents,
	 * so 16bit will be enough for the slot number. */

	/* how many updates in this transaction */
	__be16	n_updates;

	/* maximum slot number, "al-extents" in drbd.conf speak.
	 * Having this in each transaction should make reconfiguration
	 * of that parameter easier. */
	__be16	context_size;

	/* slot number the context starts with */
	__be16	context_start_slot_nr;

	/* Some reserved bytes.  Expected usage is a 64bit counter of
	 * sectors-written since device creation, and other data generation tag
	 * supporting usage */
	__be32	__reserved[4];

	/* --- 36 byte used --- */

	/* Reserve space for up to AL_UPDATES_PER_TRANSACTION changes
	 * in one transaction, then use the remaining byte in the 4k block for
	 * context information.  "Flexible" number of updates per transaction
	 * does not help, as we have to account for the case when all update
	 * slots are used anyways, so it would only complicate code without
	 * additional benefit.
	 */
	__be16	update_slot_nr[AL_UPDATES_PER_TRANSACTION];

	/* but the extent number is 32bit, which at an extent size of 4 MiB
	 * allows to cover device sizes of up to 2**54 Byte (16 PiB) */
	__be32	update_extent_nr[AL_UPDATES_PER_TRANSACTION];

	/* --- 420 bytes used (36 + 64*6) --- */

	/* 4096 - 420 = 3676 = 919 * 4 */
	__be32	context[AL_CONTEXT_PER_TRANSACTION];
};

#define DRBD_AL_PMEM_MAGIC 0x6aa667a6 /* "al==pmem" */

struct __packed al_on_pmem {
	__be32 magic;
	__be32 slots[];
};

#endif
