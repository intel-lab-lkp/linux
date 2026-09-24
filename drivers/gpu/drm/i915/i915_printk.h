/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _I915_PRINTK_H_
#define _I915_PRINTK_H_

#include <drm/drm_print.h>

#define i915_printk(_i915, _level, _fmt, ...) \
	drm_##_level(&(_i915)->drm, _fmt, ## __VA_ARGS__)

#define i915_err(_i915, _fmt, ...) \
	i915_printk((_i915), err, _fmt, ##__VA_ARGS__)

#define i915_err_once(_i915, _fmt, ...) \
	i915_printk((_i915), err_once, _fmt, ##__VA_ARGS__)

#define i915_err_ratelimited(_i915, _fmt, ...) \
	i915_printk((_i915), err_ratelimited, _fmt, ##__VA_ARGS__)

#define i915_warn(_i915, _fmt, ...) \
	i915_printk((_i915), warn, _fmt, ##__VA_ARGS__)

#define i915_notice(_i915, _fmt, ...) \
	i915_printk((_i915), notice, _fmt, ##__VA_ARGS__)

#define i915_info(_i915, _fmt, ...) \
	i915_printk((_i915), info, _fmt, ##__VA_ARGS__)

#define i915_dbg(_i915, _fmt, ...) \
	i915_printk((_i915), dbg, _fmt, ##__VA_ARGS__)

#define i915_dbg_ratelimited(_i915, _fmt, ...) \
	i915_printk((_i915), dbg_ratelimited, _fmt, ##__VA_ARGS__)

#define i915_WARN_type(_i915, _type, _condition, _fmt, ...) \
	drm_WARN##_type(&(_i915)->drm, _condition, _fmt, ## __VA_ARGS__)

#define i915_WARN(_i915, _condition, _fmt, ...) \
	i915_WARN_type((_i915), _condition, __I915_PRINTK_FMT((_i915), _fmt, ## __VA_ARGS__))

#define i915_WARN_ONCE(_i915, _condition, _fmt, ...) \
	i915_WARN_type((_i915), _ONCE, _condition, __I915_PRINTK_FMT((_i915), _fmt, ## __VA_ARGS__))

#define i915_WARN_ON(_i915, _condition) \
	i915_WARN((_i915), _condition, "%s(%s)", "WARN_ON", __stringify(_condition))

#define i915_WARN_ON_ONCE(_i915, _condition) \
	i915_WARN_ONCE((_i915), _condition, "%s(%s)", "WARN_ON_ONCE", __stringify(_condition))

#endif
