/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FANOTIFY_H
#define _LINUX_FANOTIFY_H

#include <linux/kobject.h>
#include <linux/sysctl.h>
#include <uapi/linux/fanotify.h>

#define FAN_GROUP_FLAG(group, flag) \
	((group)->fanotify_data.flags & (flag))

/*
 * Flags allowed to be passed from/to userspace.
 *
 * We intentionally do not add new bits to the old FAN_ALL_* constants, because
 * they are uapi exposed constants. If there are programs out there using
 * these constant, the programs may break if re-compiled with new uapi headers
 * and then run on an old kernel.
 */

/* Group classes where permission events are allowed */
#define FANOTIFY_PERM_CLASSES	(FAN_CLASS_CONTENT | \
				 FAN_CLASS_PRE_CONTENT)

#define FANOTIFY_CLASS_BITS	(FAN_CLASS_NOTIF | FANOTIFY_PERM_CLASSES)

#define FANOTIFY_FID_BITS	(FAN_REPORT_DFID_NAME_TARGET)

#define FANOTIFY_INFO_MODES	(FANOTIFY_FID_BITS | FAN_REPORT_PIDFD)

/*
 * fanotify_init() flags that require CAP_SYS_ADMIN.
 * We do not allow unprivileged groups to request permission events.
 * We do not allow unprivileged groups to get other process pid in events.
 * We do not allow unprivileged groups to use unlimited resources.
 */
#define FANOTIFY_ADMIN_INIT_FLAGS	(FANOTIFY_PERM_CLASSES | \
					 FAN_REPORT_TID | \
					 FAN_REPORT_PIDFD | \
					 FAN_REPORT_FD_ERROR | \
					 FAN_UNLIMITED_QUEUE | \
					 FAN_UNLIMITED_MARKS)

/*
 * fanotify_init() flags that are allowed for user without CAP_SYS_ADMIN.
 * FAN_CLASS_NOTIF is the only class we allow for unprivileged group.
 * We do not allow unprivileged groups to get file descriptors in events,
 * so one of the flags for reporting file handles is required.
 */
#define FANOTIFY_USER_INIT_FLAGS	(FAN_CLASS_NOTIF | \
					 FANOTIFY_FID_BITS | \
					 FAN_CLOEXEC | FAN_NONBLOCK)

#define FANOTIFY_INIT_FLAGS	(FANOTIFY_ADMIN_INIT_FLAGS | \
				 FANOTIFY_USER_INIT_FLAGS)

/* Internal group flags */
#define FANOTIFY_UNPRIV		0x80000000
#define FANOTIFY_INTERNAL_GROUP_FLAGS	(FANOTIFY_UNPRIV)

#define FANOTIFY_MARK_TYPE_BITS	(FAN_MARK_INODE | FAN_MARK_MOUNT | \
				 FAN_MARK_FILESYSTEM)

#define FANOTIFY_MARK_CMD_BITS	(FAN_MARK_ADD | FAN_MARK_REMOVE | \
				 FAN_MARK_FLUSH)

#define FANOTIFY_MARK_IGNORE_BITS (FAN_MARK_IGNORED_MASK | \
				   FAN_MARK_IGNORE)

#define FANOTIFY_MARK_FLAGS	(FANOTIFY_MARK_TYPE_BITS | \
				 FANOTIFY_MARK_CMD_BITS | \
				 FANOTIFY_MARK_IGNORE_BITS | \
				 FAN_MARK_DONT_FOLLOW | \
				 FAN_MARK_ONLYDIR | \
				 FAN_MARK_IGNORED_SURV_MODIFY | \
				 FAN_MARK_EVICTABLE)

/*
 * Events that can be reported with data type FSNOTIFY_EVENT_PATH.
 * Note that FAN_MODIFY can also be reported with data type
 * FSNOTIFY_EVENT_INODE.
 */
#define FANOTIFY_PATH_EVENTS	(FAN_ACCESS | FAN_MODIFY | \
				 FAN_CLOSE | FAN_OPEN | FAN_OPEN_EXEC)

/*
 * Directory entry modification events - reported only to directory
 * where entry is modified and not to a watching parent.
 */
#define FANOTIFY_DIRENT_EVENTS	(FAN_MOVE | FAN_CREATE | FAN_DELETE | \
				 FAN_RENAME)

/* Events that can be reported with event->fd */
#define FANOTIFY_FD_EVENTS (FANOTIFY_PATH_EVENTS | FANOTIFY_PERM_EVENTS)

/* Events that can only be reported with data type FSNOTIFY_EVENT_INODE */
#define FANOTIFY_INODE_EVENTS	(FANOTIFY_DIRENT_EVENTS | \
				 FAN_ATTRIB | FAN_MOVE_SELF | FAN_DELETE_SELF)

/* Events that can only be reported with data type FSNOTIFY_EVENT_ERROR */
#define FANOTIFY_ERROR_EVENTS	(FAN_FS_ERROR)

/* Events that user can request to be notified on */
#define FANOTIFY_EVENTS		(FANOTIFY_PATH_EVENTS | \
				 FANOTIFY_INODE_EVENTS | \
				 FANOTIFY_ERROR_EVENTS)

/* Events that require a permission response from user */
#define FANOTIFY_PERM_EVENTS	(FAN_OPEN_PERM | FAN_ACCESS_PERM | \
				 FAN_OPEN_EXEC_PERM)

/* Extra flags that may be reported with event or control handling of events */
#define FANOTIFY_EVENT_FLAGS	(FAN_EVENT_ON_CHILD | FAN_ONDIR)

/* Events that may be reported to user */
#define FANOTIFY_OUTGOING_EVENTS	(FANOTIFY_EVENTS | \
					 FANOTIFY_PERM_EVENTS | \
					 FAN_Q_OVERFLOW | FAN_ONDIR)

/* Events and flags relevant only for directories */
#define FANOTIFY_DIRONLY_EVENT_BITS	(FANOTIFY_DIRENT_EVENTS | \
					 FAN_EVENT_ON_CHILD | FAN_ONDIR)

#define ALL_FANOTIFY_EVENT_BITS		(FANOTIFY_OUTGOING_EVENTS | \
					 FANOTIFY_EVENT_FLAGS)

/* These masks check for invalid bits in permission responses. */
#define FANOTIFY_RESPONSE_ACCESS (FAN_ALLOW | FAN_DENY)
#define FANOTIFY_RESPONSE_FLAGS (FAN_AUDIT | FAN_INFO)
#define FANOTIFY_RESPONSE_VALID_MASK (FANOTIFY_RESPONSE_ACCESS | FANOTIFY_RESPONSE_FLAGS)

/* Do not use these old uapi constants internally */
#undef FAN_ALL_CLASS_BITS
#undef FAN_ALL_INIT_FLAGS
#undef FAN_ALL_MARK_FLAGS
#undef FAN_ALL_EVENTS
#undef FAN_ALL_PERM_EVENTS
#undef FAN_ALL_OUTGOING_EVENTS

struct fsnotify_group;
struct qstr;
struct inode;
struct fanotify_filter_hook;

/*
 * Event passed to fanotify filter
 *
 * @mask:	event type and flags
 * @data:	object that event happened on
 * @data_type:	type of object for fanotify_data_XXX() accessors
 * @dir:	optional directory associated with event -
 *		if @file_name is not NULL, this is the directory that
 *		@file_name is relative to
 * @file_name:	optional file name associated with event
 * @match_mask:	mark types of this group that matched the event
 */
struct fanotify_filter_event {
	u32 mask;
	const void *data;
	int data_type;
	struct inode *dir;
	const struct qstr *file_name;
	__kernel_fsid_t *fsid;
	u32 match_mask;
};

/*
 * fanotify filter should implement these ops.
 *
 * filter - Main call for the filter.
 * @group:	The group being notified
 * @filter_hook:	fanotify_filter_hook for the attach on @group.
 * Returns: enum fanotify_filter_return.
 *
 * filter_init - Initialize the fanotify_filter_hook.
 * @group:	The group that getting the filter
 * @hook:	fanotify_filter_hook to be initialized
 * @args:	Arguments used to initialize @hook
 *
 * filter_free - Free the fanotify_filter_hook.
 * @hook:	fanotify_filter_hook to be freed.
 *
 * @name:	Name of the fanotify_filter_ops. This need to be unique
 *		in the system
 * @owner:	Owner module of this fanotify_filter_ops
 * @list:	Attach to global list of fanotify_filter_ops
 * @flags:	Flags for the fanotify_filter_ops
 * @init_args_size: expected size of @args of filter_init()
 * @desc:	Description of what this filter do (optional)
 * @init_args:	Description of the init_args in a string (optional)
 */
struct fanotify_filter_ops {
	int (*filter)(struct fsnotify_group *group,
		      struct fanotify_filter_hook *filter_hook,
		      struct fanotify_filter_event *filter_event);
	int (*filter_init)(struct fsnotify_group *group,
			   struct fanotify_filter_hook *hook,
			   void *args);
	void (*filter_free)(struct fanotify_filter_hook *hook);

	char name[FAN_FILTER_NAME_MAX];
	struct module *owner;
	struct list_head list;
	u32 flags;
	u32 init_args_size;
	const char *desc;
	const char *init_args;

	/* internal */
	struct kobject kobj;
};

/* Flags for fanotify_filter_ops->flags */
enum fanotify_filter_flags {
	/* CAP_SYS_ADMIN is required to use this filter */
	FAN_FILTER_F_SYS_ADMIN_ONLY = BIT(0),

	FAN_FILTER_F_ALL = FAN_FILTER_F_SYS_ADMIN_ONLY,
};

/*
 * Hook that attaches fanotify_filter_ops to a group.
 * @ops:	the ops
 * @data:	per group data used by the ops
 */
struct fanotify_filter_hook {
	struct fanotify_filter_ops *ops;
	void *data;
};

#ifdef CONFIG_FANOTIFY_FILTER

int fanotify_filter_register(struct fanotify_filter_ops *ops);
void fanotify_filter_unregister(struct fanotify_filter_ops *ops);
int fanotify_filter_add(struct fsnotify_group *group,
			struct fanotify_filter_args __user *args);
void fanotify_filter_del(struct fsnotify_group *group);
void fanotify_filter_hook_free(struct fanotify_filter_hook *filter_hook);

#else /* CONFIG_FANOTIFY_FILTER */

static inline int fanotify_filter_register(struct fanotify_filter_ops *ops)
{
	return -EOPNOTSUPP;
}

static inline void fanotify_filter_unregister(struct fanotify_filter_ops *ops)
{
}

static inline int fanotify_filter_add(struct fsnotify_group *group,
				      struct fanotify_filter_args __user *args)
{
	return -ENOENT;
}

static inline void fanotify_filter_del(struct fsnotify_group *group)
{
}

static inline void fanotify_filter_hook_free(struct fanotify_filter_hook *filter_hook)
{
}

#endif /* CONFIG_FANOTIFY_FILTER */

#endif /* _LINUX_FANOTIFY_H */
