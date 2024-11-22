/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */

#ifndef _SAMPLES_FANOTIFY_FILTER_H
#define _SAMPLES_FANOTIFY_FILTER_H

enum fan_filter_sample_mode {
	/* Only show event in the subtree */
	FAN_FILTER_SAMPLE_MODE_FILTER = 1,
	/* Block access to files in the subtree */
	FAN_FILTER_SAMPLE_MODE_BLOCK = 2,
};

struct fan_filter_sample_args {
	int subtree_fd;
	enum fan_filter_sample_mode mode;
};

#endif /* _SAMPLES_FANOTIFY_FILTER_H */
