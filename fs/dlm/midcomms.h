/* SPDX-License-Identifier: GPL-2.0-only */
/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
**
**
*******************************************************************************
******************************************************************************/

#ifndef __MIDCOMMS_DOT_H__
#define __MIDCOMMS_DOT_H__

#include "config.h"

struct midcomms_node;

int dlm_validate_incoming_buffer(int nodeid, unsigned char *buf, int len);
int dlm_process_incoming_buffer(struct dlm_net *dn, int nodeid,
				unsigned char *buf, int len);
struct dlm_mhandle *dlm_midcomms_get_mhandle(struct dlm_net *dn, int nodeid,
					     int len, char **ppc);
void dlm_midcomms_commit_mhandle(struct dlm_mhandle *mh, const void *name,
				 int namelen);
int dlm_midcomms_addr(struct dlm_net *dn, int nodeid,
		      struct sockaddr_storage *addr);
void dlm_midcomms_version_wait(struct dlm_net *dn);
int dlm_midcomms_close(struct dlm_net *dn, int nodeid);
int dlm_midcomms_start(struct dlm_net *dn);
void dlm_midcomms_init(struct dlm_net *dn);
void dlm_midcomms_exit(struct dlm_net *dn);
void dlm_midcomms_stop(struct dlm_net *dn);
void dlm_midcomms_shutdown(struct dlm_net *dn);
void dlm_midcomms_add_member(struct dlm_net *dn, int nodeid);
void dlm_midcomms_remove_member(struct dlm_net *dn, int nodeid);
void dlm_midcomms_unack_msg_resend(struct dlm_net *dn, int nodeid);
const char *dlm_midcomms_state(struct midcomms_node *node);
unsigned long dlm_midcomms_flags(struct midcomms_node *node);
int dlm_midcomms_send_queue_cnt(struct midcomms_node *node);
uint32_t dlm_midcomms_version(struct midcomms_node *node);
int dlm_midcomms_rawmsg_send(struct midcomms_node *node, void *buf,
			     int buflen);
struct kmem_cache *dlm_midcomms_cache_create(void);

#endif				/* __MIDCOMMS_DOT_H__ */

