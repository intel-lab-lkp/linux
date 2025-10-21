/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef _INTEL_CURSOR_H_
#define _INTEL_CURSOR_H_

enum pipe;
struct drm_mode_create_dumb;
struct intel_display;
struct intel_plane;
struct kthread_work;

struct intel_plane *
intel_cursor_plane_create(struct intel_display *display,
			  enum pipe pipe);

void intel_cursor_unpin_work(struct kthread_work *base);
int intel_cursor_dumb_create(struct intel_display *display,
			     struct drm_mode_create_dumb *args);

#endif
