/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_OSDMAP_H
#define _FS_CEPH_OSDMAP_H

#include <linux/rbtree.h>
#include <linux/ceph/types.h>
#include <linux/ceph/decode.h>
#include <linux/crush/crush.h>

/*
 * The osd map describes the current membership of the osd cluster and
 * specifies the mapping of objects to placement groups and placement
 * groups to (sets of) osds.  That is, it completely specifies the
 * (desired) distribution of all data objects in the system at some
 * point in time.
 *
 * Each map version is identified by an epoch, which increases monotonically.
 *
 * The map can be updated either via an incremental map (diff) describing
 * the change between two successive epochs, or as a fully encoded map.
 */
/*
 * Placement group identifier metadata: Identifies a placement group within
 * the RADOS system. PGs group objects together for replication and distribution
 * across OSDs using a deterministic mapping based on pool and placement seed.
 */
struct ceph_pg {
	/* Pool identifier this PG belongs to */
	uint64_t pool;
	/* Placement seed for object distribution within the pool */
	uint32_t seed;
};

#define CEPH_SPG_NOSHARD	-1

/*
 * Sharded placement group metadata: Extends placement group identification
 * with shard information for erasure-coded pools. Each PG can be split
 * into multiple shards for parallel processing and distribution.
 */
struct ceph_spg {
	/* Base placement group identifier */
	struct ceph_pg pgid;
	/* Shard number within the PG (CEPH_SPG_NOSHARD for replicated pools) */
	s8 shard;
};

int ceph_pg_compare(const struct ceph_pg *lhs, const struct ceph_pg *rhs);
int ceph_spg_compare(const struct ceph_spg *lhs, const struct ceph_spg *rhs);

#define CEPH_POOL_FLAG_HASHPSPOOL	(1ULL << 0) /* hash pg seed and pool id
						       together */
#define CEPH_POOL_FLAG_FULL		(1ULL << 1) /* pool is full */
#define CEPH_POOL_FLAG_FULL_QUOTA	(1ULL << 10) /* pool ran out of quota,
							will set FULL too */
#define CEPH_POOL_FLAG_NEARFULL		(1ULL << 11) /* pool is nearfull */

/*
 * Pool information metadata: Complete description of a RADOS storage pool
 * including replication settings, placement group configuration, and tiering
 * information. Contains all parameters needed for object placement decisions.
 */
struct ceph_pg_pool_info {
	/* Red-black tree node for efficient lookup */
	struct rb_node node;
	/* Unique pool identifier */
	s64 id;
	/* Pool type (replicated, erasure-coded) */
	u8 type; /* CEPH_POOL_TYPE_* */
	/* Number of replicas or erasure coding width */
	u8 size;
	/* Minimum replicas required for I/O */
	u8 min_size;
	/* CRUSH rule for object placement */
	u8 crush_ruleset;
	/* Hash function for object name hashing */
	u8 object_hash;
	/* Last epoch when force resend was required */
	u32 last_force_request_resend;
	/* Number of placement groups and placement groups for placement */
	u32 pg_num, pgp_num;
	/* Bitmasks derived from pg_num and pgp_num */
	int pg_num_mask, pgp_num_mask;
	/* Read tier pool (for cache tiering) */
	s64 read_tier;
	/* Write tier pool (takes precedence for read+write) */
	s64 write_tier; /* wins for read+write ops */
	/* Pool status and behavior flags */
	u64 flags; /* CEPH_POOL_FLAG_* */
	/* Human-readable pool name */
	char *name;

	/* Previous full state (for map change handling) */
	bool was_full;  /* for handle_one_map() */
};

static inline bool ceph_can_shift_osds(struct ceph_pg_pool_info *pool)
{
	switch (pool->type) {
	case CEPH_POOL_TYPE_REP:
		return true;
	case CEPH_POOL_TYPE_EC:
		return false;
	default:
		BUG();
	}
}

/*
 * Object locator metadata: Specifies the storage location for an object
 * within the RADOS cluster. Combines pool identification with optional
 * namespace for fine-grained object organization.
 */
struct ceph_object_locator {
	/* Target pool ID (-1 for unspecified) */
	s64 pool;
	/* Optional namespace within the pool */
	struct ceph_string *pool_ns;
};

static inline void ceph_oloc_init(struct ceph_object_locator *oloc)
{
	oloc->pool = -1;
	oloc->pool_ns = NULL;
}

static inline bool ceph_oloc_empty(const struct ceph_object_locator *oloc)
{
	return oloc->pool == -1;
}

void ceph_oloc_copy(struct ceph_object_locator *dest,
		    const struct ceph_object_locator *src);
void ceph_oloc_destroy(struct ceph_object_locator *oloc);

/*
 * 51-char inline_name is long enough for all cephfs and all but one
 * rbd requests: <imgname> in "<imgname>.rbd"/"rbd_id.<imgname>" can be
 * arbitrarily long (~PAGE_SIZE).  It's done once during rbd map; all
 * other rbd requests fit into inline_name.
 *
 * Makes ceph_object_id 64 bytes on 64-bit.
 */
#define CEPH_OID_INLINE_LEN 52

/*
 * Both inline and external buffers have space for a NUL-terminator,
 * which is carried around.  It's not required though - RADOS object
 * names don't have to be NUL-terminated and may contain NULs.
 *
 * Object identifier metadata: Flexible object naming with inline optimization.
 * Uses inline storage for short names (common case) and dynamic allocation
 * for longer names. Supports arbitrary byte sequences including NUL bytes.
 */
struct ceph_object_id {
	/* Pointer to object name (may point to inline_name) */
	char *name;
	/* Inline storage for short object names */
	char inline_name[CEPH_OID_INLINE_LEN];
	/* Length of object name in bytes */
	int name_len;
};

#define __CEPH_OID_INITIALIZER(oid) { .name = (oid).inline_name }

#define CEPH_DEFINE_OID_ONSTACK(oid)				\
	struct ceph_object_id oid = __CEPH_OID_INITIALIZER(oid)

static inline void ceph_oid_init(struct ceph_object_id *oid)
{
	*oid = (struct ceph_object_id) __CEPH_OID_INITIALIZER(*oid);
}

static inline bool ceph_oid_empty(const struct ceph_object_id *oid)
{
	return oid->name == oid->inline_name && !oid->name_len;
}

void ceph_oid_copy(struct ceph_object_id *dest,
		   const struct ceph_object_id *src);
__printf(2, 3)
void ceph_oid_printf(struct ceph_object_id *oid, const char *fmt, ...);
__printf(3, 4)
int ceph_oid_aprintf(struct ceph_object_id *oid, gfp_t gfp,
		     const char *fmt, ...);
void ceph_oid_destroy(struct ceph_object_id *oid);

/*
 * Workspace manager metadata: Manages a pool of compression workspaces
 * for CRUSH map processing. Provides efficient allocation and reuse of
 * workspaces to avoid frequent memory allocation during map calculations.
 */
struct workspace_manager {
	/* List of idle workspaces ready for use */
	struct list_head idle_ws;
	/* Spinlock protecting workspace list operations */
	spinlock_t ws_lock;
	/* Number of free workspaces available */
	int free_ws;
	/* Total number of allocated workspaces */
	atomic_t total_ws;
	/* Wait queue for threads waiting for free workspace */
	wait_queue_head_t ws_wait;
};

/*
 * Placement group mapping override metadata: Allows administrators to override
 * the default CRUSH-generated OSD mappings for specific placement groups.
 * Supports various override types for operational flexibility.
 */
struct ceph_pg_mapping {
	/* Red-black tree node for efficient lookup */
	struct rb_node node;
	/* Placement group this mapping applies to */
	struct ceph_pg pgid;

	/* Different types of mapping overrides */
	union {
		/* Temporary OSD set override */
		struct {
			/* Number of OSDs in override set */
			int len;
			/* Array of OSD IDs */
			int osds[];
		} pg_temp, pg_upmap;
		/* Temporary primary OSD override */
		struct {
			/* Primary OSD ID */
			int osd;
		} primary_temp;
		/* Item-by-item OSD remapping */
		struct {
			/* Number of from->to mappings */
			int len;
			/* Array of [from_osd, to_osd] pairs */
			int from_to[][2];
		} pg_upmap_items;
	};
};

/*
 * OSD cluster map metadata: Complete description of the RADOS cluster topology
 * and configuration. Contains all information needed to locate objects, determine
 * OSD health, and route requests. Updated with each cluster state change.
 */
struct ceph_osdmap {
	/* Cluster filesystem identifier */
	struct ceph_fsid fsid;
	/* Map version number (monotonically increasing) */
	u32 epoch;
	/* Timestamps for map creation and modification */
	struct ceph_timespec created, modified;

	/* Global cluster flags */
	u32 flags;         /* CEPH_OSDMAP_* */

	/* OSD array size and state information */
	u32 max_osd;       /* size of osd_state, _offload, _addr arrays */
	/* Per-OSD state flags (exists, up, etc.) */
	u32 *osd_state;    /* CEPH_OSD_* */
	/* Per-OSD weight (0=failed, 0x10000=100% normal) */
	u32 *osd_weight;
	/* Per-OSD network addresses */
	struct ceph_entity_addr *osd_addr;

	/* Temporary PG to OSD mappings */
	struct rb_root pg_temp;
	struct rb_root primary_temp;

	/* Post-CRUSH, pre-up remappings for load balancing */
	struct rb_root pg_upmap;	/* PG := raw set */
	struct rb_root pg_upmap_items;	/* from -> to within raw set */

	/* Per-OSD primary affinity weights */
	u32 *osd_primary_affinity;

	/* Storage pool definitions */
	struct rb_root pg_pools;
	u32 pool_max;

	/* CRUSH map for object placement calculations.
	 * The CRUSH map specifies the mapping of placement groups to
	 * the list of osds that store+replicate them. */
	struct crush_map *crush;

	/* Workspace manager for CRUSH calculations */
	struct workspace_manager crush_wsm;
};

static inline bool ceph_osd_exists(struct ceph_osdmap *map, int osd)
{
	return osd >= 0 && osd < map->max_osd &&
	       (map->osd_state[osd] & CEPH_OSD_EXISTS);
}

static inline bool ceph_osd_is_up(struct ceph_osdmap *map, int osd)
{
	return ceph_osd_exists(map, osd) &&
	       (map->osd_state[osd] & CEPH_OSD_UP);
}

static inline bool ceph_osd_is_down(struct ceph_osdmap *map, int osd)
{
	return !ceph_osd_is_up(map, osd);
}

char *ceph_osdmap_state_str(char *str, int len, u32 state);
extern u32 ceph_get_primary_affinity(struct ceph_osdmap *map, int osd);

static inline struct ceph_entity_addr *ceph_osd_addr(struct ceph_osdmap *map,
						     int osd)
{
	if (osd >= map->max_osd)
		return NULL;
	return &map->osd_addr[osd];
}

#define CEPH_PGID_ENCODING_LEN		(1 + 8 + 4 + 4)

static inline int ceph_decode_pgid(void **p, void *end, struct ceph_pg *pgid)
{
	__u8 version;

	if (!ceph_has_room(p, end, CEPH_PGID_ENCODING_LEN)) {
		pr_warn("incomplete pg encoding\n");
		return -EINVAL;
	}
	version = ceph_decode_8(p);
	if (version > 1) {
		pr_warn("do not understand pg encoding %d > 1\n",
			(int)version);
		return -EINVAL;
	}

	pgid->pool = ceph_decode_64(p);
	pgid->seed = ceph_decode_32(p);
	*p += 4;	/* skip deprecated preferred value */

	return 0;
}

struct ceph_osdmap *ceph_osdmap_alloc(void);
struct ceph_osdmap *ceph_osdmap_decode(void **p, void *end, bool msgr2);
struct ceph_osdmap *osdmap_apply_incremental(void **p, void *end, bool msgr2,
					     struct ceph_osdmap *map);
extern void ceph_osdmap_destroy(struct ceph_osdmap *map);

/*
 * OSD set metadata: Represents a set of OSDs that store replicas of a
 * placement group. Contains the ordered list of OSDs and identifies
 * the primary OSD responsible for coordinating operations.
 */
struct ceph_osds {
	/* Array of OSD IDs in preference order */
	int osds[CEPH_PG_MAX_SIZE];
	/* Number of OSDs in the set */
	int size;
	/* Primary OSD ID (not array index) */
	int primary; /* id, NOT index */
};

static inline void ceph_osds_init(struct ceph_osds *set)
{
	set->size = 0;
	set->primary = -1;
}

void ceph_osds_copy(struct ceph_osds *dest, const struct ceph_osds *src);

bool ceph_pg_is_split(const struct ceph_pg *pgid, u32 old_pg_num,
		      u32 new_pg_num);
bool ceph_is_new_interval(const struct ceph_osds *old_acting,
			  const struct ceph_osds *new_acting,
			  const struct ceph_osds *old_up,
			  const struct ceph_osds *new_up,
			  int old_size,
			  int new_size,
			  int old_min_size,
			  int new_min_size,
			  u32 old_pg_num,
			  u32 new_pg_num,
			  bool old_sort_bitwise,
			  bool new_sort_bitwise,
			  bool old_recovery_deletes,
			  bool new_recovery_deletes,
			  const struct ceph_pg *pgid);
bool ceph_osds_changed(const struct ceph_osds *old_acting,
		       const struct ceph_osds *new_acting,
		       bool any_change);

void __ceph_object_locator_to_pg(struct ceph_pg_pool_info *pi,
				 const struct ceph_object_id *oid,
				 const struct ceph_object_locator *oloc,
				 struct ceph_pg *raw_pgid);
int ceph_object_locator_to_pg(struct ceph_osdmap *osdmap,
			      const struct ceph_object_id *oid,
			      const struct ceph_object_locator *oloc,
			      struct ceph_pg *raw_pgid);

void ceph_pg_to_up_acting_osds(struct ceph_osdmap *osdmap,
			       struct ceph_pg_pool_info *pi,
			       const struct ceph_pg *raw_pgid,
			       struct ceph_osds *up,
			       struct ceph_osds *acting);
bool ceph_pg_to_primary_shard(struct ceph_osdmap *osdmap,
			      struct ceph_pg_pool_info *pi,
			      const struct ceph_pg *raw_pgid,
			      struct ceph_spg *spgid);
int ceph_pg_to_acting_primary(struct ceph_osdmap *osdmap,
			      const struct ceph_pg *raw_pgid);

/*
 * CRUSH location constraint metadata: Specifies a location constraint
 * for CRUSH map placement. Used to restrict object placement to specific
 * parts of the cluster hierarchy (e.g., specific racks, hosts).
 */
struct crush_loc {
	/* CRUSH hierarchy level type (e.g., "rack", "host") */
	char *cl_type_name;
	/* Name of the specific location within that type */
	char *cl_name;
};

/*
 * CRUSH location node metadata: Red-black tree node for efficient storage
 * and lookup of CRUSH location constraints. Contains the location data
 * inline for memory efficiency.
 */
struct crush_loc_node {
	/* Red-black tree linkage */
	struct rb_node cl_node;
	/* Location constraint (pointers into cl_data) */
	struct crush_loc cl_loc;
	/* Inline storage for location strings */
	char cl_data[];
};

int ceph_parse_crush_location(char *crush_location, struct rb_root *locs);
int ceph_compare_crush_locs(struct rb_root *locs1, struct rb_root *locs2);
void ceph_clear_crush_locs(struct rb_root *locs);

int ceph_get_crush_locality(struct ceph_osdmap *osdmap, int id,
			    struct rb_root *locs);

extern struct ceph_pg_pool_info *ceph_pg_pool_by_id(struct ceph_osdmap *map,
						    u64 id);
extern const char *ceph_pg_pool_name_by_id(struct ceph_osdmap *map, u64 id);
extern int ceph_pg_poolid_by_name(struct ceph_osdmap *map, const char *name);
u64 ceph_pg_pool_flags(struct ceph_osdmap *map, u64 id);

#endif
