/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CEPH_CLS_LOCK_CLIENT_H
#define _LINUX_CEPH_CLS_LOCK_CLIENT_H

#include <linux/ceph/osd_client.h>

/*
 * Object class lock types: Defines the types of locks that can be acquired
 * on RADOS objects through the lock object class. Supports both exclusive
 * and shared locking semantics for distributed coordination.
 */
enum ceph_cls_lock_type {
	/* No lock held */
	CEPH_CLS_LOCK_NONE = 0,
	/* Exclusive lock - only one holder allowed */
	CEPH_CLS_LOCK_EXCLUSIVE = 1,
	/* Shared lock - multiple readers allowed */
	CEPH_CLS_LOCK_SHARED = 2,
};

/*
 * Lock holder identifier metadata: Uniquely identifies a client that holds
 * or is requesting a lock on a RADOS object. Combines client entity name
 * with a session-specific cookie for disambiguation.
 */
struct ceph_locker_id {
	/* Client entity name (type and number) */
	struct ceph_entity_name name;
	/* Unique session cookie for this lock holder */
	char *cookie;
};

/*
 * Lock holder information metadata: Contains additional information about
 * a lock holder, primarily the network address for client identification
 * and potential communication.
 */
struct ceph_locker_info {
	/* Network address of the lock holder */
	struct ceph_entity_addr addr;
};

/*
 * Complete lock holder metadata: Combines lock holder identification and
 * network information into a complete description of a client that holds
 * a lock on a RADOS object. Used for lock enumeration and management.
 */
struct ceph_locker {
	/* Lock holder identification (name + cookie) */
	struct ceph_locker_id id;
	/* Lock holder network information */
	struct ceph_locker_info info;
};

int ceph_cls_lock(struct ceph_osd_client *osdc,
		  struct ceph_object_id *oid,
		  struct ceph_object_locator *oloc,
		  char *lock_name, u8 type, char *cookie,
		  char *tag, char *desc, u8 flags);
int ceph_cls_unlock(struct ceph_osd_client *osdc,
		    struct ceph_object_id *oid,
		    struct ceph_object_locator *oloc,
		    char *lock_name, char *cookie);
int ceph_cls_break_lock(struct ceph_osd_client *osdc,
			struct ceph_object_id *oid,
			struct ceph_object_locator *oloc,
			char *lock_name, char *cookie,
			struct ceph_entity_name *locker);
int ceph_cls_set_cookie(struct ceph_osd_client *osdc,
			struct ceph_object_id *oid,
			struct ceph_object_locator *oloc,
			char *lock_name, u8 type, char *old_cookie,
			char *tag, char *new_cookie);

void ceph_free_lockers(struct ceph_locker *lockers, u32 num_lockers);

int ceph_cls_lock_info(struct ceph_osd_client *osdc,
		       struct ceph_object_id *oid,
		       struct ceph_object_locator *oloc,
		       char *lock_name, u8 *type, char **tag,
		       struct ceph_locker **lockers, u32 *num_lockers);

int ceph_cls_assert_locked(struct ceph_osd_request *req, int which,
			   char *lock_name, u8 type, char *cookie, char *tag);

#endif
