/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2024 Oracle and/or its affiliates.
 * Author: Anjali Kulkarni <anjali.k.kulkarni@oracle.com>
 */
extern int cn_display_hlist(pid_t pid, int max_len, int *hkey,
				int *key_display);
extern int cn_del_elem(pid_t pid);
extern int cn_add_elem(__u32 uexit_code, pid_t pid);
extern __u32 cn_del_get_exval(pid_t pid);
extern int cn_get_exval(pid_t pid);
extern bool cn_table_empty(void);
