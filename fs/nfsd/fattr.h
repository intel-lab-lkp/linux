/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Attribute conversion helpers shared by the NFS and NFS_ACL procs.
 */

#ifndef _NFSD_FATTR_H
#define _NFSD_FATTR_H

struct fattr;
struct kstat;
struct post_op_attr;
struct svc_fh;
struct svc_rqst;

void nfsd_stat_to_fattr(struct svc_rqst *rqstp, struct fattr *fattr,
			const struct kstat *stat, const struct svc_fh *fhp);
void nfsd3_fill_post_op_attr(struct svc_rqst *rqstp, struct post_op_attr *attr,
			     struct svc_fh *fhp);

#endif /* _NFSD_FATTR_H */
