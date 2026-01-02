// SPDX-License-Identifier: GPL-2.0

#include <linux/ww_mutex.h>

__rust_helper
void rust_helper_ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class)
{
	ww_mutex_init(lock, ww_class);
}

__rust_helper
void rust_helper_ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	ww_acquire_init(ctx, ww_class);
}

__rust_helper
void rust_helper_ww_acquire_done(struct ww_acquire_ctx *ctx)
{
	ww_acquire_done(ctx);
}

__rust_helper
void rust_helper_ww_acquire_fini(struct ww_acquire_ctx *ctx)
{
	ww_acquire_fini(ctx);
}

__rust_helper
void rust_helper_ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_slow(lock, ctx);
}

__rust_helper
int rust_helper_ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock_slow_interruptible(lock, ctx);
}

__rust_helper
bool rust_helper_ww_mutex_is_locked(struct ww_mutex *lock)
{
	return ww_mutex_is_locked(lock);
}
