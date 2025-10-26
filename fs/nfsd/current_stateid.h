/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NFSD4_CURRENT_STATE_H
#define _NFSD4_CURRENT_STATE_H

#include "state.h"
#include "xdr4.h"

/*
 * functions to set current state id
 */
extern void nfsd4_set_opendowngradestateid(struct nfsd4_compound_state *,
		union nfsd4_op_u *);
extern void nfsd4_set_openstateid(struct nfsd4_compound_state *,
		union nfsd4_op_u *);
extern void nfsd4_set_lockstateid(struct nfsd4_compound_state *,
		union nfsd4_op_u *);
extern void nfsd4_set_closestateid(struct nfsd4_compound_state *,
		union nfsd4_op_u *);

#endif   /* _NFSD4_CURRENT_STATE_H */
