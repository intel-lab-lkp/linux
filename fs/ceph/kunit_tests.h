/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit tests declarations
 *
 * Copyright (C) 2025, IBM Corporation
 *
 * Author: Viacheslav Dubeyko <Slava.Dubeyko@ibm.com>
 */

#ifndef _CEPH_KUNIT_TESTS_H
#define _CEPH_KUNIT_TESTS_H

#include <kunit/visibility.h>

#if IS_ENABLED(CONFIG_KUNIT)
int ceph_mds_state_2_str_idx(int mds_state);
extern const char *ceph_mds_state_name_strings[];
int ceph_session_op_2_str_idx(int op);
extern const char *ceph_session_op_name_strings[];
int ceph_mds_op_2_str_idx(int op);
extern const char *ceph_mds_op_name_strings[];
int ceph_cap_op_2_str_idx(int op);
extern const char *ceph_cap_op_name_strings[];
int ceph_lease_op_2_str_idx(int op);
extern const char *ceph_lease_op_name_strings[];
int ceph_snap_op_2_str_idx(int op);
extern const char *ceph_snap_op_name_strings[];
#endif

#endif /* _CEPH_KUNIT_TESTS_H */
