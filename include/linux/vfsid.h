/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MNT_VFSID_H
#define _LINUX_MNT_VFSID_H

#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/build_bug.h>

typedef struct {
	uid_t val;
} vfsuid_t;

typedef struct {
	gid_t val;
} vfsgid_t;

static_assert(sizeof(vfsuid_t) == sizeof(kuid_t));
static_assert(sizeof(vfsgid_t) == sizeof(kgid_t));
static_assert(offsetof(vfsuid_t, val) == offsetof(kuid_t, val));
static_assert(offsetof(vfsgid_t, val) == offsetof(kgid_t, val));

#ifdef CONFIG_MULTIUSER
static inline uid_t __vfsuid_val(vfsuid_t uid)
{
	return uid.val;
}

static inline gid_t __vfsgid_val(vfsgid_t gid)
{
	return gid.val;
}
#else
static inline uid_t __vfsuid_val(vfsuid_t uid)
{
	return 0;
}

static inline gid_t __vfsgid_val(vfsgid_t gid)
{
	return 0;
}
#endif

static inline bool vfsuid_valid(vfsuid_t uid)
{
	return __vfsuid_val(uid) != (uid_t)-1;
}

static inline bool vfsgid_valid(vfsgid_t gid)
{
	return __vfsgid_val(gid) != (gid_t)-1;
}

static inline bool vfsuid_eq(vfsuid_t left, vfsuid_t right)
{
	return vfsuid_valid(left) && __vfsuid_val(left) == __vfsuid_val(right);
}

static inline bool vfsgid_eq(vfsgid_t left, vfsgid_t right)
{
	return vfsgid_valid(left) && __vfsgid_val(left) == __vfsgid_val(right);
}

/*
 * vfs{g,u}ids are created from k{g,u}ids.
 * We don't allow them to be created from regular {u,g}id.
 */
#define VFSUIDT_INIT(val) (vfsuid_t){ __kuid_val(val) }
#define VFSGIDT_INIT(val) (vfsgid_t){ __kgid_val(val) }

#define INVALID_VFSUID VFSUIDT_INIT(INVALID_UID)
#define INVALID_VFSGID VFSGIDT_INIT(INVALID_GID)

#endif /* _LINUX_MNT_VFSID_H */
