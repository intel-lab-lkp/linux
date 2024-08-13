/* SPDX-License-Identifier: GPL-2.0 */
#ifndef WORKINGSET_REPORT_H_
#define WORKINGSET_REPORT_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#define PAGETYPE_ANON 0
#define PAGETYPE_FILE 1
#define ANON_AND_FILE 2

int get_nr_nodes(void);
int drop_pagecache(void);

long sysfs_get_refresh_interval(int nid);
int sysfs_set_refresh_interval(int nid, long interval);

int sysfs_get_page_age_intervals_str(int nid, char *buf, int len);
int sysfs_set_page_age_intervals_str(int nid, const char *buf, int len);

int sysfs_set_page_age_intervals(int nid, const char *const intervals[],
				 int nr_intervals);

char *page_age_split_node(char *buf, int nid, char **next);
ssize_t sysfs_page_age_read(int nid, char *buf, size_t len);
ssize_t page_age_read(const char *buf, const char *interval, int pagetype);

int alloc_file_workingset(void *arg);
void cleanup_file_workingset(void);
int alloc_anon_workingset(void *arg);

#endif /* WORKINGSET_REPORT_H_ */
