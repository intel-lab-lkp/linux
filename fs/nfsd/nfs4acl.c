/*
 *  Common NFSv4 ACL handling code.
 *
 *  Copyright (c) 2002, 2003 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Marius Aamodt Eriksen <marius@umich.edu>
 *  Jeff Sedlak <jsedlak@umich.edu>
 *  J. Bruce Fields <bfields@umich.edu>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 *  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/posix_acl.h>

#include "nfsfh.h"
#include "nfsd.h"
#include "acl.h"
#include "vfs.h"

#define NFS4_ACL_TYPE_DEFAULT	0x01
#define NFS4_ACL_DIR		0x02
#define NFS4_ACL_OWNER		0x04
#define NFS4_ACL_DIR_STICKY	0x08

/* mode bit translations: */
#define NFS4_READ_MODE (NFS4_ACE_READ_DATA)
#define NFS4_WRITE_MODE (NFS4_ACE_WRITE_DATA | NFS4_ACE_APPEND_DATA)
#define NFS4_EXECUTE_MODE NFS4_ACE_EXECUTE
#define NFS4_ANYONE_MODE (NFS4_ACE_READ_ATTRIBUTES | NFS4_ACE_READ_ACL | NFS4_ACE_SYNCHRONIZE)
#define NFS4_OWNER_MODE (NFS4_ACE_WRITE_ATTRIBUTES | NFS4_ACE_WRITE_ACL)

/* flags used to simulate posix default ACLs */
#define NFS4_INHERITANCE_FLAGS (NFS4_ACE_FILE_INHERIT_ACE \
		| NFS4_ACE_DIRECTORY_INHERIT_ACE)

#define NFS4_SUPPORTED_FLAGS (NFS4_INHERITANCE_FLAGS \
		| NFS4_ACE_INHERIT_ONLY_ACE \
		| NFS4_ACE_IDENTIFIER_GROUP)

static u32
mask_from_posix(unsigned short perm, unsigned int flags)
{
	int mask = NFS4_ANYONE_MODE;

	if (flags & NFS4_ACL_OWNER)
		mask |= NFS4_OWNER_MODE;
	if (perm & ACL_READ)
		mask |= NFS4_READ_MODE;
	if (perm & ACL_WRITE)
		mask |= NFS4_WRITE_MODE;
	if ((perm & ACL_WRITE) && (flags & NFS4_ACL_DIR) && !(flags & NFS4_ACL_DIR_STICKY))
		mask |= NFS4_ACE_DELETE_CHILD;
	if (perm & ACL_EXECUTE)
		mask |= NFS4_EXECUTE_MODE;
	return mask;
}

static u32
deny_mask_from_posix(unsigned short perm, u32 flags)
{
	u32 mask = 0;

	if (perm & ACL_READ)
		mask |= NFS4_READ_MODE;
	if (perm & ACL_WRITE)
		mask |= NFS4_WRITE_MODE;
	if ((perm & ACL_WRITE) && (flags & NFS4_ACL_DIR) && !(flags & NFS4_ACL_DIR_STICKY))
		mask |= NFS4_ACE_DELETE_CHILD;
	if (perm & ACL_EXECUTE)
		mask |= NFS4_EXECUTE_MODE;
	return mask;
}

/* XXX: modify functions to return NFS errors; they're only ever
 * used by nfs code, after all.... */

/* We only map from NFSv4 to POSIX ACLs when setting ACLs, when we err on the
 * side of being more restrictive, so the mode bit mapping below is
 * pessimistic.  An optimistic version would be needed to handle DENY's,
 * but we expect to coalesce all ALLOWs and DENYs before mapping to mode
 * bits. */

static void
low_mode_from_nfs4(u32 perm, unsigned short *mode, unsigned int flags)
{
	u32 write_mode = NFS4_WRITE_MODE;

	if ((flags & NFS4_ACL_DIR) && !(flags | NFS4_ACL_DIR_STICKY))
		write_mode |= NFS4_ACE_DELETE_CHILD;
	*mode = 0;
	if ((perm & NFS4_READ_MODE) == NFS4_READ_MODE)
		*mode |= ACL_READ;
	if ((perm & write_mode) == write_mode)
		*mode |= ACL_WRITE;
	if ((perm & NFS4_EXECUTE_MODE) == NFS4_EXECUTE_MODE)
		*mode |= ACL_EXECUTE;
}

static short ace2type(struct nfs4_ace *);
static void _posix_to_nfsv4_one(struct posix_acl *, struct nfs4_acl *,
				unsigned int, kuid_t);

int
nfsd4_get_nfs4_acl(struct svc_rqst *rqstp, struct dentry *dentry,
		struct nfs4_acl **acl)
{
	struct inode *inode = d_inode(dentry);
	int error = 0;
	struct posix_acl *pacl = NULL, *dpacl = NULL;
	unsigned int flags = 0;
	int size = 0;

	pacl = get_inode_acl(inode, ACL_TYPE_ACCESS);
	if (!pacl)
		pacl = posix_acl_from_mode(inode->i_mode, GFP_KERNEL);

	if (IS_ERR(pacl))
		return PTR_ERR(pacl);

	/*
	 * allocate for worst case: one (deny, allow) pair for each
	 * plus for restricted deletion flag (sticky bit): 4 + one for each
	 */
	size += 2 * pacl->a_count + (4 + pacl->a_count);

	if (S_ISDIR(inode->i_mode)) {
		flags = NFS4_ACL_DIR;
		dpacl = get_inode_acl(inode, ACL_TYPE_DEFAULT);
		if (IS_ERR(dpacl)) {
			error = PTR_ERR(dpacl);
			goto rel_pacl;
		}

		if (dpacl)
			size += 2 * dpacl->a_count;

		/* propagate restricted deletion flag (sticky bit) */
		if (inode->i_mode & S_ISVTX)
			flags |= NFS4_ACL_DIR_STICKY;
	}

	*acl = kmalloc(nfs4_acl_bytes(size), GFP_KERNEL);
	if (*acl == NULL) {
		error = -ENOMEM;
		goto out;
	}
	(*acl)->naces = 0;

	_posix_to_nfsv4_one(pacl, *acl, flags & ~NFS4_ACL_TYPE_DEFAULT, inode->i_uid);

	if (dpacl)
		_posix_to_nfsv4_one(dpacl, *acl, (flags | NFS4_ACL_TYPE_DEFAULT) & ~NFS4_ACL_DIR_STICKY, inode->i_uid);

out:
	posix_acl_release(dpacl);
rel_pacl:
	posix_acl_release(pacl);
	return error;
}

struct posix_acl_summary {
	unsigned short owner;
	unsigned short users;
	unsigned short group;
	unsigned short groups;
	unsigned short other;
	unsigned short mask;
};

static void
summarize_posix_acl(struct posix_acl *acl, struct posix_acl_summary *pas)
{
	struct posix_acl_entry *pa, *pe;

	/*
	 * Only pas.users and pas.groups need initialization; previous
	 * posix_acl_valid() calls ensure that the other fields will be
	 * initialized in the following loop.  But, just to placate gcc:
	 */
	memset(pas, 0, sizeof(*pas));
	pas->mask = 07;

	pe = acl->a_entries + acl->a_count;

	FOREACH_ACL_ENTRY(pa, acl, pe) {
		switch (pa->e_tag) {
			case ACL_USER_OBJ:
				pas->owner = pa->e_perm;
				break;
			case ACL_GROUP_OBJ:
				pas->group = pa->e_perm;
				break;
			case ACL_USER:
				pas->users |= pa->e_perm;
				break;
			case ACL_GROUP:
				pas->groups |= pa->e_perm;
				break;
			case ACL_OTHER:
				pas->other = pa->e_perm;
				break;
			case ACL_MASK:
				pas->mask = pa->e_perm;
				break;
		}
	}
	/* We'll only care about effective permissions: */
	pas->users &= pas->mask;
	pas->group &= pas->mask;
	pas->groups &= pas->mask;
}

/* We assume the acl has been verified with posix_acl_valid. */
static void
_posix_to_nfsv4_one(struct posix_acl *pacl, struct nfs4_acl *acl,
		    unsigned int flags, kuid_t owner_uid)
{
	struct posix_acl_entry *pa, *group_owner_entry;
	struct nfs4_ace *ace;
	struct posix_acl_summary pas;
	unsigned short deny;
	int eflag = ((flags & NFS4_ACL_TYPE_DEFAULT) ?
		NFS4_INHERITANCE_FLAGS | NFS4_ACE_INHERIT_ONLY_ACE : 0);

	BUG_ON(pacl->a_count < 3);
	summarize_posix_acl(pacl, &pas);

	ace = acl->aces + acl->naces;

	/*
	 * Translate restricted deletion flag (sticky bit, SVTX) set on the
	 * directory to following NFS4 ACEs prepended before any other ACEs:
	 *
	 *   ALLOW OWNER@ DELETE_CHILD                                 (1)
	 *   DENY EVERYONE@ DELETE_CHILD
	 *   INHERIT_ONLY NO_PROPAGATE DENY user_owner_of_dir DELETE   (3)
	 *   INHERIT_ONLY NO_PROPAGATE DENY OWNER@ DELETE              (4)
	 *   INHERIT_ONLY NO_PROPAGATE DENY user1 DELETE               (ACL user1)
	 *   ...
	 *   INHERIT_ONLY NO_PROPAGATE DENY userN DELETE               (ACL userN)
	 *   INHERIT_ONLY NO_PROPAGATE ALLOW OWNER@ DELETE
	 *
	 *   (1) - only if user-owner has write access
	 *   (3) - only if user-owner does not have write access
	 *   (4) - only if non-user-owner does not have write access
	 *   (ACL user1) - only if user1 does not have write access
	 *   (ACL userN) - only if userN does not have write access
	 *
	 * These ACEs describe behavior of set restricted deletion flag (sticky
	 * bit) on directory as they allow only owner of individual child entries
	 * and owner of the directory to delete individual child entries.
	 * Everyone else is denied to remove child entries in this directory.
	 *
	 * For deleting entry in NFS4 ACL model it is needed either DELETE_CHILD
	 * permission (access mask) from the parent directory or DELETE permission
	 * (access mask) on the child. Just one of these two permissions is enough.
	 * So if there is explicit DENY DELETE_CHILD on the parent together with
	 * explicit ALLOW DELETE on the child, it means that deleting is allowed
	 * (evaluation of permissions is independent in NFS4 ACL model). OWNER@
	 * for inherited ACEs means owner of the child entry and not the owner
	 * of the parent from which was ACE inherited.
	 *
	 * This translation is imperfect just for a case when some group
	 * (including group-owner and others-group) does not have write access
	 * to directory. Handled is only the edge case (via rule 4) when
	 * everyone else except owner has no write access to the directory.
	 * This information is not present in NFS4 ACL because it looks like that
	 * this is not possible to express in this ACL model. So for ACL consumer
	 * it could look like that owner of the file can delete its own file even
	 * when group or other mode bits of the directory do not allow it.
	 */
	if (flags & NFS4_ACL_DIR_STICKY) {
		/*
		 * Explicitly allow directory owner to delete child entries
		 * if directory owner has write access
		 */
		if (pas.owner & ACL_WRITE) {
			ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
			ace->flag = 0;
			ace->access_mask = NFS4_ACE_DELETE_CHILD;
			ace->whotype = NFS4_ACL_WHO_OWNER;
			ace++;
			acl->naces++;
		}

		/*
		 * And then deny deleting child entries for all other users
		 * which do not have explicit DELETE permission granted by
		 * last rule in this section.
		 */
		ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
		ace->flag = 0;
		ace->access_mask = NFS4_ACE_DELETE_CHILD;
		ace->whotype = NFS4_ACL_WHO_EVERYONE;
		ace++;
		acl->naces++;

		/*
		 * Do not grant directory owner to delete child entries (by the
		 * last rule in this section) if it does not have write access.
		 */
		if (!(pas.owner & ACL_WRITE)) {
			ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
			ace->flag = NFS4_INHERITANCE_FLAGS |
				    NFS4_ACE_INHERIT_ONLY_ACE |
				    NFS4_ACE_NO_PROPAGATE_INHERIT_ACE;
			ace->access_mask = NFS4_ACE_DELETE;
			ace->whotype = NFS4_ACL_WHO_NAMED;
			ace->who_uid = owner_uid;
			ace++;
			acl->naces++;
		}

		if (!(pas.users & ACL_WRITE) && !(pas.group & ACL_WRITE) &&
		    !(pas.groups & ACL_WRITE) && !(pas.other & ACL_WRITE)) {
			/*
			 * Do not grant child owner who is not directory owner
			 * (handled by the first rule in this section) to
			 * delete own child entries if there is no possible
			 * directory write permission (checked for named users,
			 * group-owner, named groups and others-groups).
			 * This handles special edge case when only directory
			 * owner has write access to directory.
			 */
			ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
			ace->flag = NFS4_INHERITANCE_FLAGS |
				    NFS4_ACE_INHERIT_ONLY_ACE |
				    NFS4_ACE_NO_PROPAGATE_INHERIT_ACE;
			ace->access_mask = NFS4_ACE_DELETE;
			ace->whotype = NFS4_ACL_WHO_OWNER;
			ace++;
			acl->naces++;
		} else {
			/*
			 * Do not grant individual named users to delete child
			 * entries (by the last rule in this section) if user
			 * does not have write access to directory.
			 */
			for (pa = pacl->a_entries + 1; pa->e_tag == ACL_USER; pa++) {
				if (pa->e_perm & pas.mask & ACL_WRITE)
					continue;
				ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
				ace->flag = NFS4_INHERITANCE_FLAGS |
					    NFS4_ACE_INHERIT_ONLY_ACE |
					    NFS4_ACE_NO_PROPAGATE_INHERIT_ACE;
				ace->access_mask = NFS4_ACE_DELETE;
				ace->whotype = NFS4_ACL_WHO_NAMED;
				ace->who_uid = pa->e_uid;
				ace++;
				acl->naces++;
			}
		}

		/*
		 * Above rules filtered out users which do not have write access
		 * to the directory. Now allow child-owner to delete its own
		 * directory entries.
		 */
		ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
		ace->flag = NFS4_INHERITANCE_FLAGS |
			    NFS4_ACE_INHERIT_ONLY_ACE |
			    NFS4_ACE_NO_PROPAGATE_INHERIT_ACE;
		ace->access_mask = NFS4_ACE_DELETE;
		ace->whotype = NFS4_ACL_WHO_OWNER;
		ace++;
		acl->naces++;
	}

	pa = pacl->a_entries;

	/* We could deny everything not granted by the owner: */
	deny = ~pas.owner;
	/*
	 * but it is equivalent (and simpler) to deny only what is not
	 * granted by later entries:
	 */
	deny &= pas.users | pas.group | pas.groups | pas.other;
	if (deny) {
		ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
		ace->flag = eflag;
		ace->access_mask = deny_mask_from_posix(deny, flags);
		ace->whotype = NFS4_ACL_WHO_OWNER;
		ace++;
		acl->naces++;
	}

	ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
	ace->flag = eflag;
	ace->access_mask = mask_from_posix(pa->e_perm, flags | NFS4_ACL_OWNER);
	ace->whotype = NFS4_ACL_WHO_OWNER;
	ace++;
	acl->naces++;
	pa++;

	while (pa->e_tag == ACL_USER) {
		deny = ~(pa->e_perm & pas.mask);
		deny &= pas.groups | pas.group | pas.other;
		if (deny) {
			ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
			ace->flag = eflag;
			ace->access_mask = deny_mask_from_posix(deny, flags);
			ace->whotype = NFS4_ACL_WHO_NAMED;
			ace->who_uid = pa->e_uid;
			ace++;
			acl->naces++;
		}
		ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
		ace->flag = eflag;
		ace->access_mask = mask_from_posix(pa->e_perm & pas.mask,
						   flags);
		ace->whotype = NFS4_ACL_WHO_NAMED;
		ace->who_uid = pa->e_uid;
		ace++;
		acl->naces++;
		pa++;
	}

	/* In the case of groups, we apply allow ACEs first, then deny ACEs,
	 * since a user can be in more than one group.  */

	/* allow ACEs */

	group_owner_entry = pa;

	ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
	ace->flag = eflag;
	ace->access_mask = mask_from_posix(pas.group, flags);
	ace->whotype = NFS4_ACL_WHO_GROUP;
	ace++;
	acl->naces++;
	pa++;

	while (pa->e_tag == ACL_GROUP) {
		ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
		ace->flag = eflag | NFS4_ACE_IDENTIFIER_GROUP;
		ace->access_mask = mask_from_posix(pa->e_perm & pas.mask,
						   flags);
		ace->whotype = NFS4_ACL_WHO_NAMED;
		ace->who_gid = pa->e_gid;
		ace++;
		acl->naces++;
		pa++;
	}

	/* deny ACEs */

	pa = group_owner_entry;

	deny = ~pas.group & pas.other;
	if (deny) {
		ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
		ace->flag = eflag;
		ace->access_mask = deny_mask_from_posix(deny, flags);
		ace->whotype = NFS4_ACL_WHO_GROUP;
		ace++;
		acl->naces++;
	}
	pa++;

	while (pa->e_tag == ACL_GROUP) {
		deny = ~(pa->e_perm & pas.mask);
		deny &= pas.other;
		if (deny) {
			ace->type = NFS4_ACE_ACCESS_DENIED_ACE_TYPE;
			ace->flag = eflag | NFS4_ACE_IDENTIFIER_GROUP;
			ace->access_mask = deny_mask_from_posix(deny, flags);
			ace->whotype = NFS4_ACL_WHO_NAMED;
			ace->who_gid = pa->e_gid;
			ace++;
			acl->naces++;
		}
		pa++;
	}

	if (pa->e_tag == ACL_MASK)
		pa++;
	ace->type = NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE;
	ace->flag = eflag;
	ace->access_mask = mask_from_posix(pa->e_perm, flags);
	ace->whotype = NFS4_ACL_WHO_EVERYONE;
	acl->naces++;
}

static bool
pace_gt(struct posix_acl_entry *pace1, struct posix_acl_entry *pace2)
{
	if (pace1->e_tag != pace2->e_tag)
		return pace1->e_tag > pace2->e_tag;
	if (pace1->e_tag == ACL_USER)
		return uid_gt(pace1->e_uid, pace2->e_uid);
	if (pace1->e_tag == ACL_GROUP)
		return gid_gt(pace1->e_gid, pace2->e_gid);
	return false;
}

static void
sort_pacl_range(struct posix_acl *pacl, int start, int end) {
	int sorted = 0, i;

	/* We just do a bubble sort; easy to do in place, and we're not
	 * expecting acl's to be long enough to justify anything more. */
	while (!sorted) {
		sorted = 1;
		for (i = start; i < end; i++) {
			if (pace_gt(&pacl->a_entries[i],
				    &pacl->a_entries[i+1])) {
				sorted = 0;
				swap(pacl->a_entries[i],
				     pacl->a_entries[i + 1]);
			}
		}
	}
}

static void
sort_pacl(struct posix_acl *pacl)
{
	/* posix_acl_valid requires that users and groups be in order
	 * by uid/gid. */
	int i, j;

	/* no users or groups */
	if (!pacl || pacl->a_count <= 4)
		return;

	i = 1;
	while (pacl->a_entries[i].e_tag == ACL_USER)
		i++;
	sort_pacl_range(pacl, 1, i-1);

	BUG_ON(pacl->a_entries[i].e_tag != ACL_GROUP_OBJ);
	j = ++i;
	while (pacl->a_entries[j].e_tag == ACL_GROUP)
		j++;
	sort_pacl_range(pacl, i, j-1);
	return;
}

/*
 * While processing the NFSv4 ACE, this maintains bitmasks representing
 * which permission bits have been allowed and which denied to a given
 * entity: */
struct posix_ace_state {
	u32 allow;
	u32 deny;
};

struct posix_user_ace_state {
	union {
		kuid_t uid;
		kgid_t gid;
	};
	struct posix_ace_state perms;
};

struct posix_ace_state_array {
	int n;
	struct posix_user_ace_state aces[];
};

/*
 * While processing the NFSv4 ACE, this maintains the partial permissions
 * calculated so far: */

struct posix_acl_state {
	unsigned char valid;
	struct posix_ace_state owner;
	struct posix_ace_state group;
	struct posix_ace_state other;
	struct posix_ace_state everyone;
	struct posix_ace_state mask; /* Deny unused in this case */
	struct posix_ace_state_array *users;
	struct posix_ace_state_array *groups;
};

static int
init_state(struct posix_acl_state *state, int cnt)
{
	int alloc;

	memset(state, 0, sizeof(struct posix_acl_state));
	/*
	 * In the worst case, each individual acl could be for a distinct
	 * named user or group, but we don't know which, so we allocate
	 * enough space for either:
	 */
	alloc = sizeof(struct posix_ace_state_array)
		+ cnt*sizeof(struct posix_user_ace_state);
	state->users = kzalloc(alloc, GFP_KERNEL);
	if (!state->users)
		return -ENOMEM;
	state->groups = kzalloc(alloc, GFP_KERNEL);
	if (!state->groups) {
		kfree(state->users);
		return -ENOMEM;
	}
	return 0;
}

static void
free_state(struct posix_acl_state *state) {
	kfree(state->users);
	kfree(state->groups);
}

static inline void add_to_mask(struct posix_acl_state *state, struct posix_ace_state *astate)
{
	state->mask.allow |= astate->allow;
}

static struct posix_acl *
posix_state_to_acl(struct posix_acl_state *state, unsigned int flags)
{
	struct posix_acl_entry *pace;
	struct posix_acl *pacl;
	int nace;
	int i;

	/*
	 * ACLs with no ACEs are treated differently in the inheritable
	 * and effective cases: when there are no inheritable ACEs,
	 * calls ->set_acl with a NULL ACL structure.
	 */
	if (!state->valid && (flags & NFS4_ACL_TYPE_DEFAULT))
		return NULL;

	/*
	 * When there are no effective ACEs, the following will end
	 * up setting a 3-element effective posix ACL with all
	 * permissions zero.
	 */
	if (!state->users->n && !state->groups->n)
		nace = 3;
	else /* Note we also include a MASK ACE in this case: */
		nace = 4 + state->users->n + state->groups->n;
	pacl = posix_acl_alloc(nace, GFP_KERNEL);
	if (!pacl)
		return ERR_PTR(-ENOMEM);

	pace = pacl->a_entries;
	pace->e_tag = ACL_USER_OBJ;
	/* owner is never affected by restricted deletion flag, so clear NFS4_ACL_DIR_STICKY */
	low_mode_from_nfs4(state->owner.allow, &pace->e_perm, flags & ~NFS4_ACL_DIR_STICKY);

	for (i=0; i < state->users->n; i++) {
		pace++;
		pace->e_tag = ACL_USER;
		low_mode_from_nfs4(state->users->aces[i].perms.allow,
					&pace->e_perm, flags);
		pace->e_uid = state->users->aces[i].uid;
		add_to_mask(state, &state->users->aces[i].perms);
	}

	pace++;
	pace->e_tag = ACL_GROUP_OBJ;
	low_mode_from_nfs4(state->group.allow, &pace->e_perm, flags);
	add_to_mask(state, &state->group);

	for (i=0; i < state->groups->n; i++) {
		pace++;
		pace->e_tag = ACL_GROUP;
		low_mode_from_nfs4(state->groups->aces[i].perms.allow,
					&pace->e_perm, flags);
		pace->e_gid = state->groups->aces[i].gid;
		add_to_mask(state, &state->groups->aces[i].perms);
	}

	if (state->users->n || state->groups->n) {
		pace++;
		pace->e_tag = ACL_MASK;
		low_mode_from_nfs4(state->mask.allow, &pace->e_perm, flags);
	}

	pace++;
	pace->e_tag = ACL_OTHER;
	low_mode_from_nfs4(state->other.allow, &pace->e_perm, flags);

	return pacl;
}

static inline void allow_bits(struct posix_ace_state *astate, u32 mask)
{
	/* Allow all bits in the mask not already denied: */
	astate->allow |= mask & ~astate->deny;
}

static inline void deny_bits(struct posix_ace_state *astate, u32 mask)
{
	/* Deny all bits in the mask not already allowed: */
	astate->deny |= mask & ~astate->allow;
}

static int find_uid(struct posix_acl_state *state, kuid_t uid)
{
	struct posix_ace_state_array *a = state->users;
	int i;

	for (i = 0; i < a->n; i++)
		if (uid_eq(a->aces[i].uid, uid))
			return i;
	/* Not found: */
	a->n++;
	a->aces[i].uid = uid;
	a->aces[i].perms.allow = state->everyone.allow;
	a->aces[i].perms.deny  = state->everyone.deny;

	return i;
}

static int find_gid(struct posix_acl_state *state, kgid_t gid)
{
	struct posix_ace_state_array *a = state->groups;
	int i;

	for (i = 0; i < a->n; i++)
		if (gid_eq(a->aces[i].gid, gid))
			return i;
	/* Not found: */
	a->n++;
	a->aces[i].gid = gid;
	a->aces[i].perms.allow = state->everyone.allow;
	a->aces[i].perms.deny  = state->everyone.deny;

	return i;
}

static void deny_bits_array(struct posix_ace_state_array *a, u32 mask)
{
	int i;

	for (i=0; i < a->n; i++)
		deny_bits(&a->aces[i].perms, mask);
}

static void allow_bits_array(struct posix_ace_state_array *a, u32 mask)
{
	int i;

	for (i=0; i < a->n; i++)
		allow_bits(&a->aces[i].perms, mask);
}

static void process_one_v4_ace(struct posix_acl_state *state,
				struct nfs4_ace *ace)
{
	u32 mask = ace->access_mask;
	short type = ace2type(ace);
	int i;

	state->valid |= type;

	switch (type) {
	case ACL_USER_OBJ:
		if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE) {
			allow_bits(&state->owner, mask);
		} else {
			deny_bits(&state->owner, mask);
		}
		break;
	case ACL_USER:
		i = find_uid(state, ace->who_uid);
		if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE) {
			allow_bits(&state->users->aces[i].perms, mask);
		} else {
			deny_bits(&state->users->aces[i].perms, mask);
			mask = state->users->aces[i].perms.deny;
			deny_bits(&state->owner, mask);
		}
		break;
	case ACL_GROUP_OBJ:
		if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE) {
			allow_bits(&state->group, mask);
		} else {
			deny_bits(&state->group, mask);
			mask = state->group.deny;
			deny_bits(&state->owner, mask);
			deny_bits(&state->everyone, mask);
			deny_bits_array(state->users, mask);
			deny_bits_array(state->groups, mask);
		}
		break;
	case ACL_GROUP:
		i = find_gid(state, ace->who_gid);
		if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE) {
			allow_bits(&state->groups->aces[i].perms, mask);
		} else {
			deny_bits(&state->groups->aces[i].perms, mask);
			mask = state->groups->aces[i].perms.deny;
			deny_bits(&state->owner, mask);
			deny_bits(&state->group, mask);
			deny_bits(&state->everyone, mask);
			deny_bits_array(state->users, mask);
			deny_bits_array(state->groups, mask);
		}
		break;
	case ACL_OTHER:
		if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE) {
			allow_bits(&state->owner, mask);
			allow_bits(&state->group, mask);
			allow_bits(&state->other, mask);
			allow_bits(&state->everyone, mask);
			allow_bits_array(state->users, mask);
			allow_bits_array(state->groups, mask);
		} else {
			deny_bits(&state->owner, mask);
			deny_bits(&state->group, mask);
			deny_bits(&state->other, mask);
			deny_bits(&state->everyone, mask);
			deny_bits_array(state->users, mask);
			deny_bits_array(state->groups, mask);
		}
	}
}

static int nfs4_acl_nfsv4_to_posix(struct nfs4_acl *acl,
		struct posix_acl **pacl, struct posix_acl **dpacl,
		int *dir_sticky,
		unsigned int flags)
{
	struct posix_acl_state effective_acl_state, default_acl_state;
	bool dir_allow_nonowner_delete_child = false;
	bool dir_deny_everyone_delete_child = false;
	bool dir_allow_child_owner_delete = false;
	unsigned int eflags = 0;
	struct nfs4_ace *ace;
	int ret;

	ret = init_state(&effective_acl_state, acl->naces);
	if (ret)
		return ret;
	ret = init_state(&default_acl_state, acl->naces);
	if (ret)
		goto out_estate;
	ret = -EINVAL;
	for (ace = acl->aces; ace < acl->aces + acl->naces; ace++) {
		/*
		 * Process and parse ACE entry INHERIT_ONLY NO_PROPAGATE DELETE
		 * for detecting restricted deletion flag (sticky bit). Allow
		 * SYNCHRONIZE access mask to be present as NFS4 clients can
		 * include this access mask together with any other one.
		 *
		 * It needs to be done before validation as NFS4_SUPPORTED_FLAGS
		 * does not contain NFS4_ACE_NO_PROPAGATE_INHERIT_ACE and this
		 * ACE must not throw error.
		 */
		if ((flags & NFS4_ACL_DIR) &&
		    !(ace->flag & ~(NFS4_SUPPORTED_FLAGS|NFS4_ACE_NO_PROPAGATE_INHERIT_ACE)) &&
		    (ace->flag & NFS4_INHERITANCE_FLAGS) &&
		    (ace->flag & NFS4_ACE_INHERIT_ONLY_ACE) &&
		    (ace->flag & NFS4_ACE_NO_PROPAGATE_INHERIT_ACE) &&
		    (ace->access_mask & ~NFS4_ACE_SYNCHRONIZE) == NFS4_ACE_DELETE) {
			if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE &&
			    ace->whotype == NFS4_ACL_WHO_OWNER)
				dir_allow_child_owner_delete = true;
			continue;
		}

		if (ace->type != NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE &&
		    ace->type != NFS4_ACE_ACCESS_DENIED_ACE_TYPE)
			goto out_dstate;
		if (ace->flag & ~NFS4_SUPPORTED_FLAGS)
			goto out_dstate;
		if ((ace->flag & NFS4_INHERITANCE_FLAGS) == 0) {
			process_one_v4_ace(&effective_acl_state, ace);
			continue;
		}
		if (!(flags & NFS4_ACL_DIR))
			goto out_dstate;
		/*
		 * Note that when only one of FILE_INHERIT or DIRECTORY_INHERIT
		 * is set, we're effectively turning on the other.  That's OK,
		 * according to rfc 3530.
		 */
		process_one_v4_ace(&default_acl_state, ace);

		if (!(ace->flag & NFS4_ACE_INHERIT_ONLY_ACE))
			process_one_v4_ace(&effective_acl_state, ace);

		/*
		 * Process and parse ACE entry with DELETE_CHILD access mask
		 * for detecting restricted deletion flag (sticky bit).
		 */
		if ((flags & NFS4_ACL_DIR) &&
		    !(ace->flag & NFS4_ACE_INHERIT_ONLY_ACE) &&
		    (ace->access_mask & NFS4_ACE_DELETE_CHILD)) {
			if (ace->type == NFS4_ACE_ACCESS_ALLOWED_ACE_TYPE &&
			    !dir_deny_everyone_delete_child &&
			    ace->whotype != NFS4_ACL_WHO_OWNER)
				dir_allow_nonowner_delete_child = true;
			else if (ace->type == NFS4_ACE_ACCESS_DENIED_ACE_TYPE &&
				 ace->whotype == NFS4_ACL_WHO_EVERYONE)
				dir_deny_everyone_delete_child = true;
		}
	}

	/*
	 * Recognize restricted deletion flag (sticky bit) from directory ACL
	 * if ACEs on directory allow only owner of directory child entry to
	 * delete entry itself.
	 *
	 * This is relaxed check for rules generated by _posix_to_nfsv4_one().
	 * Relaxed check of restricted deletion flag is for security reasons
	 * and means that permissions would be more stricter, to prevent
	 * granting more access than what was specified in NFS4 ACL packet.
	 */
	if (flags & NFS4_ACL_DIR) {
		*dir_sticky = !dir_allow_nonowner_delete_child && dir_allow_child_owner_delete;
		if (*dir_sticky)
			eflags |= NFS4_ACL_DIR_STICKY;
	}

	/*
	 * At this point, the default ACL may have zeroed-out entries for owner,
	 * group and other. That usually results in a non-sensical resulting ACL
	 * that denies all access except to any ACE that was explicitly added.
	 *
	 * The setfacl command solves a similar problem with this logic:
	 *
	 * "If  a  Default  ACL  entry is created, and the Default ACL contains
	 *  no owner, owning group, or others entry,  a  copy of  the  ACL
	 *  owner, owning group, or others entry is added to the Default ACL."
	 *
	 * Copy any missing ACEs from the effective set, if any ACEs were
	 * explicitly set.
	 */
	if (default_acl_state.valid) {
		if (!(default_acl_state.valid & ACL_USER_OBJ))
			default_acl_state.owner = effective_acl_state.owner;
		if (!(default_acl_state.valid & ACL_GROUP_OBJ))
			default_acl_state.group = effective_acl_state.group;
		if (!(default_acl_state.valid & ACL_OTHER))
			default_acl_state.other = effective_acl_state.other;
	}

	*pacl = posix_state_to_acl(&effective_acl_state, flags | eflags);
	if (IS_ERR(*pacl)) {
		ret = PTR_ERR(*pacl);
		*pacl = NULL;
		goto out_dstate;
	}
	*dpacl = posix_state_to_acl(&default_acl_state,
						flags | NFS4_ACL_TYPE_DEFAULT);
	if (IS_ERR(*dpacl)) {
		ret = PTR_ERR(*dpacl);
		*dpacl = NULL;
		posix_acl_release(*pacl);
		*pacl = NULL;
		goto out_dstate;
	}
	sort_pacl(*pacl);
	sort_pacl(*dpacl);
	ret = 0;
out_dstate:
	free_state(&default_acl_state);
out_estate:
	free_state(&effective_acl_state);
	return ret;
}

__be32 nfsd4_acl_to_attr(enum nfs_ftype4 type, struct nfs4_acl *acl,
			 struct nfsd_attrs *attr, int *dir_sticky)
{
	int host_error;
	unsigned int flags = 0;

	if (!acl) {
		if (type == NF4DIR)
			*dir_sticky = -1;
		return nfs_ok;
	}

	if (type == NF4DIR)
		flags = NFS4_ACL_DIR;

	host_error = nfs4_acl_nfsv4_to_posix(acl, &attr->na_pacl,
					     &attr->na_dpacl, dir_sticky,
					     flags);
	if (host_error == -EINVAL)
		return nfserr_attrnotsupp;
	else
		return nfserrno(host_error);
}

static short
ace2type(struct nfs4_ace *ace)
{
	switch (ace->whotype) {
		case NFS4_ACL_WHO_NAMED:
			return (ace->flag & NFS4_ACE_IDENTIFIER_GROUP ?
					ACL_GROUP : ACL_USER);
		case NFS4_ACL_WHO_OWNER:
			return ACL_USER_OBJ;
		case NFS4_ACL_WHO_GROUP:
			return ACL_GROUP_OBJ;
		case NFS4_ACL_WHO_EVERYONE:
			return ACL_OTHER;
	}
	BUG();
	return -1;
}

/*
 * return the size of the struct nfs4_acl required to represent an acl
 * with @entries entries.
 */
int nfs4_acl_bytes(int entries)
{
	return sizeof(struct nfs4_acl) + entries * sizeof(struct nfs4_ace);
}

static struct {
	char *string;
	int   stringlen;
	int type;
} s2t_map[] = {
	{
		.string    = "OWNER@",
		.stringlen = sizeof("OWNER@") - 1,
		.type      = NFS4_ACL_WHO_OWNER,
	},
	{
		.string    = "GROUP@",
		.stringlen = sizeof("GROUP@") - 1,
		.type      = NFS4_ACL_WHO_GROUP,
	},
	{
		.string    = "EVERYONE@",
		.stringlen = sizeof("EVERYONE@") - 1,
		.type      = NFS4_ACL_WHO_EVERYONE,
	},
};

int
nfs4_acl_get_whotype(char *p, u32 len)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s2t_map); i++) {
		if (s2t_map[i].stringlen == len &&
				0 == memcmp(s2t_map[i].string, p, len))
			return s2t_map[i].type;
	}
	return NFS4_ACL_WHO_NAMED;
}

__be32 nfs4_acl_write_who(struct xdr_stream *xdr, int who)
{
	__be32 *p;
	int i;

	for (i = 0; i < ARRAY_SIZE(s2t_map); i++) {
		if (s2t_map[i].type != who)
			continue;
		p = xdr_reserve_space(xdr, s2t_map[i].stringlen + 4);
		if (!p)
			return nfserr_resource;
		p = xdr_encode_opaque(p, s2t_map[i].string,
					s2t_map[i].stringlen);
		return 0;
	}
	WARN_ON_ONCE(1);
	return nfserr_serverfault;
}
