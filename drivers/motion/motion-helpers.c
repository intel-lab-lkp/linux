// SPDX-License-Identifier: GPL-2.0
/*
 * Motion Control Subsystem - helper functions
 *
 * Copyright (C) 2024 Protonic Holland
 *      David Jander <david@protonic.nl>
 */

#include <linux/fwnode.h>
#include <linux/property.h>
#include <linux/device.h>
#include <linux/gfp_types.h>
#include <linux/hrtimer_types.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/motion.h>
#include "motion-helpers.h"
#include "motion-core.h"

#define MOTION_TIMER_PERIOD (20 * NSEC_PER_MSEC)

struct motion_timed_speed {
	struct motion_device *mdev;
	struct motion_timed_speed_ops *ops;
	unsigned int speed_full_scale;
	spinlock_t lock;
	struct hrtimer timer;
	unsigned int speed_actual;
	unsigned int dir;
	unsigned int speed_max;
	unsigned int speed_start;
	unsigned int speed_end;
	unsigned int deceleration;
	ktime_t taccel;
	ktime_t tdecel;
	ktime_t duration;
	ktime_t ts_start;
	unsigned int next_index;
	unsigned int next_dir;
	ktime_t next_duration;
	unsigned int ext_trg_index;
	unsigned int ext_trg_dir;
	ktime_t ext_trg_duration;
};

static inline int __to_signed_speed(unsigned int dir, unsigned int speed)
{
	if (dir)
		return speed;
	return -speed;
}

static int motion_timed_speed_open(struct motion_device *mdev)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	if (mts->ops->startup)
		mts->ops->startup(mdev);

	return 0;
}

static int motion_timed_speed_release(struct motion_device *mdev)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	dev_info(mdev->dev, "Release\n");
	hrtimer_cancel(&mts->timer);
	if (mts->ops->powerdown)
		mts->ops->powerdown(mdev);
	else
		mts->ops->set_speed(mdev, 0, 0);
	mts->next_duration = 0;
	mts->speed_actual = 0;
	mts->dir = 0;

	return 0;
}

static int motion_timed_speed_get_status(struct motion_device *mdev, struct mot_status *st)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	st->speed = __to_signed_speed(mts->dir, mts->speed_actual);
	st->position = 0; /* FIXME: Not yet supported */

	return 0;
}

static int motion_timed_speed_basic_run(struct motion_device *mdev, unsigned int ch, s32 v)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	int ret;
	unsigned int dir = (v < 0) ? 0 : 1;
	unsigned int dc = abs(v);
	unsigned long flags;

	hrtimer_cancel(&mts->timer);

	spin_lock_irqsave(&mts->lock, flags);
	ret = mts->ops->check_speed(mdev, dir, dc);
	if (!ret) {
		mts->speed_max = dc;
		mts->ops->set_speed(mdev, dir, dc);
		mts->speed_actual = dc;
		mts->dir = dir;
	}
	spin_unlock_irqrestore(&mts->lock, flags);

	return ret;
}

static int motion_timed_speed_basic_stop(struct motion_device *mdev, channel_mask_t ch)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	unsigned long flags;

	hrtimer_cancel(&mts->timer);

	spin_lock_irqsave(&mts->lock, flags);
	mts->ops->set_speed(mdev, 0, 0);
	mts->dir = 0;
	mts->speed_actual = 0;
	mts->speed_max = 0;
	spin_unlock_irqrestore(&mts->lock, flags);

	return 0;
}

static int motion_timed_speed_move_timed(struct motion_device *mdev, unsigned int ch,
		s32 v, ktime_t t)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	unsigned long flags;
	unsigned int dir = (v < 0) ? 0 : 1;
	unsigned int dc = abs(v);
	int ret;

	hrtimer_cancel(&mts->timer);

	spin_lock_irqsave(&mts->lock, flags);
	ret = mts->ops->check_speed(mdev, dir, dc);
	if (!ret) {
		mts->ops->set_speed(mdev, dir, dc);
		mts->speed_actual = dc;
		mts->dir = dir;
		mts->ts_start = ktime_get();
		mts->duration = t;
		mts->speed_max = dc;
		mts->deceleration = 0;
		mts->taccel = 0;
		mts->tdecel = 0;
		mts->speed_start = 0;
		mts->speed_end = 0;
	}
	spin_unlock_irqrestore(&mts->lock, flags);
	if (ret)
		return ret;

	hrtimer_start(&mts->timer, MOTION_TIMER_PERIOD, HRTIMER_MODE_REL_SOFT);

	return ret;
}

static int motion_timed_speed_validate_profile(struct motion_device *mdev,
		struct mot_profile *p)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	if ((p->na != 2) || (p->nv != 3))
		return -EINVAL;
	if ((p->acc[0] <= 0) || (p->acc[1] <= 0))
		return -EINVAL;
	if ((p->vel[0] > p->vel[1]) || (p->vel[2] > p->vel[1]))
		return -EINVAL;
	if ((p->vel[0] < 0) || (p->vel[1] <= 0) || (p->vel[2] < 0))
		return -EINVAL;
	if (p->vel[1] > mts->speed_full_scale)
		return -EINVAL;
	return 0;
}

static int motion_timed_speed_set_profile(struct motion_device *mdev,
		struct mot_profile *p)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	unsigned long flags;
	unsigned int smd;
	unsigned int esd;

	spin_lock_irqsave(&mts->lock, flags);
	mts->speed_start = p->vel[0];
	mts->speed_max = p->vel[1];
	mts->speed_end = p->vel[2];
	mts->deceleration = p->acc[1];
	smd = mts->speed_max - mts->speed_start;
	esd = mts->speed_max - mts->speed_end;
	mts->taccel = div_u64((u64)smd * NSEC_PER_SEC, p->acc[0]);
	mts->tdecel = div_u64((u64)esd * NSEC_PER_SEC, mts->deceleration);
	spin_unlock_irqrestore(&mts->lock, flags);

	return 0;
}

static void motion_timed_with_index(struct motion_device *mdev,
		unsigned int index, int dir, ktime_t t)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	motion_timed_speed_set_profile(mdev, &mdev->profiles[index]);
	mts->ops->set_speed(mdev, dir, mts->speed_start);
	mts->speed_actual = mts->speed_start;
	mts->dir = dir;
	mts->duration = t;
	mts->ts_start = ktime_get();
}

static int calc_speed(struct motion_timed_speed *mts, ktime_t now, ktime_t trem)
{
	int smd = mts->speed_max - mts->speed_actual;
	int dva = mts->speed_max - mts->speed_start;
	int dvd = mts->speed_actual - mts->speed_end;
	ktime_t tel = ktime_sub(now, mts->ts_start);

	if (trem <= 0)
		return 0;

	mts->tdecel = mts->deceleration ?
		      div_u64((u64)dvd * NSEC_PER_SEC, mts->deceleration) : 0;

	if ((smd <= 0) && (ktime_compare(trem, mts->tdecel) > 0))
		return mts->speed_max;

	/* Due to (trem > 0), zerodivision can't happen here */
	if (ktime_compare(trem, mts->tdecel) < 0)
		return mts->speed_end + div64_s64((dvd * trem), mts->tdecel);

	/* Due to (tel > 0) zerodivision can't happen here */
	if (ktime_compare(tel, mts->taccel) < 0)
		return mts->speed_start + div64_s64((dva * tel), mts->taccel);

	return mts->speed_actual;
}

static enum hrtimer_restart motion_timed_speed_timer(struct hrtimer *timer)
{
	struct motion_timed_speed *mts = container_of(timer,
			struct motion_timed_speed, timer);
	struct motion_device *mdev = mts->mdev;
	struct mot_event evt = {0};
	unsigned long flags;
	ktime_t now = ktime_get();
	ktime_t trem = ktime_sub(ktime_add(mts->ts_start, mts->duration), now);
	int speed;
	int ret = HRTIMER_RESTART;

	spin_lock_irqsave(&mts->lock, flags);
	speed = calc_speed(mts, now, trem);
	if (speed != mts->speed_actual) {
		mts->ops->set_speed(mdev, mts->dir, speed);
		mts->speed_actual = speed;
		mts->dir = mts->dir;
	}
	spin_unlock_irqrestore(&mts->lock, flags);
	if (trem <= 0) {
		mutex_lock(&mdev->mutex);
		if (mts->next_duration) {
			motion_timed_with_index(mdev, mts->next_index,
					mts->next_dir, mts->next_duration);
			mts->next_duration = 0;
		} else {
			ret = HRTIMER_NORESTART;
		}
		evt.speed = __to_signed_speed(mts->dir, mts->speed_actual);
		evt.timestamp = ktime2mot_time(now);
		evt.event = MOT_EVENT_TARGET;
		motion_report_event(mdev, &evt);
		mutex_unlock(&mdev->mutex);
	}

	if (ret == HRTIMER_RESTART)
		hrtimer_add_expires_ns(timer, MOTION_TIMER_PERIOD);

	return ret;
}

static int motion_timed_speed_motion_timed(struct motion_device *mdev, unsigned int ch,
			unsigned int index, unsigned int dir, ktime_t t,
			unsigned int when)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	int ret = 0;

	ret = mts->ops->check_speed(mdev, dir, 0);
	if (ret)
		return -EINVAL;

	switch (when) {
	case MOT_WHEN_NEXT:
		if (mts->next_duration) {
			ret = -EAGAIN;
		} else {
			mts->next_duration = t;
			mts->next_index = index;
			mts->next_dir = dir;
		}
		break;
	case MOT_WHEN_EXT_TRIGGER:
		if (mts->ext_trg_duration) {
			ret = -EAGAIN;
		} else {
			mts->ext_trg_duration = t;
			mts->ext_trg_index = index;
			mts->ext_trg_dir = dir;
		}
		break;
	case MOT_WHEN_IMMEDIATE:
		motion_timed_with_index(mdev, index, dir, t);
		hrtimer_start(&mts->timer, MOTION_TIMER_PERIOD,
				HRTIMER_MODE_REL_SOFT);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int motion_timed_speed_motion_stop(struct motion_device *mdev, channel_mask_t ch,
		unsigned int when)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;
	unsigned long flags;

	if (when != MOT_WHEN_IMMEDIATE)
		return -EINVAL;

	spin_lock_irqsave(&mts->lock, flags);
	if (hrtimer_active(&mts->timer)) {
		mts->duration = mts->tdecel;
		mts->ts_start = ktime_get();
	}
	spin_unlock_irqrestore(&mts->lock, flags);

	return 0;
}

static void motion_timed_speed_ext_trigger(struct motion_device *mdev, unsigned int index,
		channel_mask_t ch)
{
	struct motion_timed_speed *mts =
		(struct motion_timed_speed *)mdev->helper_cookie;

	if (mts->ext_trg_duration) {
		hrtimer_cancel(&mts->timer);

		motion_timed_with_index(mdev, mts->ext_trg_index,
				mts->ext_trg_dir, mts->ext_trg_duration);
		mts->ext_trg_duration = 0;
		hrtimer_start(&mts->timer, MOTION_TIMER_PERIOD,
				HRTIMER_MODE_REL_SOFT);
	}
}

static struct motion_ops motion_timed_speed_motion_ops = {
	.device_open = motion_timed_speed_open,
	.device_release = motion_timed_speed_release,
	.get_status = motion_timed_speed_get_status,
	.basic_run = motion_timed_speed_basic_run,
	.basic_stop = motion_timed_speed_basic_stop,
	.move_timed = motion_timed_speed_move_timed,
	.validate_profile = motion_timed_speed_validate_profile,
	.set_profile = motion_timed_speed_set_profile,
	.motion_timed = motion_timed_speed_motion_timed,
	.motion_stop = motion_timed_speed_motion_stop,
	.external_trigger = motion_timed_speed_ext_trigger
};

/**
 * motion_timed_speed_init - Initialize a simple timed-speed motion device
 * @mdev: Motion device that shall be initialized
 * @ops: API functions provided by driver
 * @full_scale: The maximum integer value for "full speed" for this device
 *
 * Allows a motion control driver that only has a means of adjusting motor
 * speed and optionally -direction to augment its functionality to support
 * trapezoidal motion profiles.
 *
 * Caller should create a struct motion_device and, populate
 * capabilities.type, capabilities.subdiv and optionally the scaling factors
 * and then call this function, which will add mdev->ops and fill in the
 * rest. It is responsibility of the driver to call motion_register_device()
 * afterwards.
 *
 * Return: 0 in case of success or a negative errno.
 */
int motion_timed_speed_init(struct motion_device *mdev,
		struct motion_timed_speed_ops *ops, unsigned int full_scale)
{
	struct motion_timed_speed *mts;

	mts = devm_kzalloc(mdev->parent, sizeof(struct motion_timed_speed),
			GFP_KERNEL);
	if (!mts)
		return -ENOMEM;

	mts->ops = ops;
	mts->mdev = mdev;
	mts->speed_full_scale = full_scale;
	mdev->ops = motion_timed_speed_motion_ops;
	mdev->capabilities.features |= MOT_FEATURE_SPEED | MOT_FEATURE_ACCEL |
	    MOT_FEATURE_PROFILE;
	mdev->capabilities.num_channels = 1;
	mdev->capabilities.max_apoints = 2;
	mdev->capabilities.max_vpoints = 3;
	mdev->capabilities.num_int_triggers = 0;
	mdev->capabilities.num_ext_triggers = 0; /* Filled in by core */
	mdev->capabilities.subdiv = 1;
	mdev->helper_cookie = mts;

	spin_lock_init(&mts->lock);
	hrtimer_init(&mts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	mts->timer.function = motion_timed_speed_timer;

	return 0;
}
EXPORT_SYMBOL(motion_timed_speed_init);

/**
 * motion_fwnode_get_capabilities - Get motion specific properties from fwnode
 * @mdev: Motion device to populate
 * @fwnode: fwnode handle to read properties from.
 *
 * Reads motion specific properties from @fwnode and populates @mdev
 * capabilities.
 */
void motion_fwnode_get_capabilities(struct motion_device *mdev,
		struct fwnode_handle *fwnode)
{
	unsigned int val, err;

	err = fwnode_property_read_u32(fwnode, "motion,speed-conv-mul", &val);
	if (!err)
		mdev->capabilities.speed_conv_mul = val;
	err = fwnode_property_read_u32(fwnode, "motion,speed-conv-div", &val);
	if (!err && val)
		mdev->capabilities.speed_conv_div = val;
	err = fwnode_property_read_u32(fwnode, "motion,acceleration-conv-mul", &val);
	if (!err)
		mdev->capabilities.accel_conv_mul = val;
	err = fwnode_property_read_u32(fwnode, "motion,acceleration-conv-div", &val);
	if (!err && val)
		mdev->capabilities.accel_conv_div = val;
}
EXPORT_SYMBOL(motion_fwnode_get_capabilities);

static inline int __d2t_vmax(u64 a, u64 d, u32 Vmax32, u64 Vs, u64 Ve, u64 Xt,
		u64 t2, u64 *Vs2, u64 *Ve2, ktime_t *t)
{
	u64 Vmax = (u64)Vmax32;
	u64 Vm2 = Vmax * Vmax32;
	u64 dva = Vmax32 - Vs;
	u64 dvd = Vmax32 - Ve;
	u64 ta = div_u64(dva * MSEC_PER_SEC, (u32)a);
	u64 td = div_u64(dvd * MSEC_PER_SEC, (u32)d);
	u64 X1;
	u64 X3;
	u64 Xtv = div_u64(Vmax * t2, MSEC_PER_SEC);
	u64 tms;

	*Vs2 = (u64)Vs * Vs;
	*Ve2 = (u64)Ve * Ve;
	X1 = div64_u64(Vm2 - *Vs2, a << 1);
	X3 = div64_u64(Vm2 - *Ve2, d << 1);

	/* Check if we can reach Vmax. If not try again with new Vmax */
	if (Xt > (X1 + X3 + Xtv)) {
		tms = ta + td;
		tms += div_u64(MSEC_PER_SEC * (Xt - X1 - X3), Vmax32);
		*t = ktime_add_ms(0, tms);
		return 0;
	}

	return -EAGAIN;
}

/**
 * motion_distance_to_time - Convert distance to time period
 * @mdev: Motion device
 * @index: The index of the motion profile to use
 * @distance: The covered distance of the complete movement
 * @t: Pointer to ktime_t result
 *
 * Converts the @distance of a movement using a motion (acceleration) profile
 * specified by @index into a time interval this movement would take.
 *
 * The only supported profile type is trapezoidal (3 velocity points and 2
 * acceleration values), and it takes into account Tvmax and the case where
 * Vmax cannot be reached because the distance is too short.
 *
 * Return: 0 on success and -ENOTSUPP if profile is not trapezoidal.
 */
long motion_distance_to_time(struct motion_device *mdev,
		unsigned int index, int distance, ktime_t *t)
{
	struct mot_profile *p = &mdev->profiles[index];
	unsigned int Vs = p->vel[0];
	unsigned int Ve = p->vel[2];
	u64 Vmax;
	u64 a = p->acc[0]; /* Has been checked to be non-zero */
	u64 d = p->acc[1]; /* Has been checked to be non-zero */
	u64 Xt = abs(distance);
	u64 t2 = ktime_to_ms(p->tvmax);
	u64 Ve2, Vs2, Bt, disc;
	s64 ACt;
	unsigned int bl;

	if ((p->na != 2) || (p->nv != 3))
		return -EOPNOTSUPP;

	if (!__d2t_vmax(a, d, p->vel[1], Vs, Ve, Xt, t2, &Vs2, &Ve2, t))
		return 0;

	/*
	 * We can't reach Vmax, so we need to determine Vmax that
	 * satisfies tvmax and distance, given a and d.
	 * For that we need to solve a quadratic equation in the form:
	 *
	 * 0 = Vm^2*(1/2a + 1/2d) + Vm * tvmax - Vs^2/2a - Ve^2/2d - Xt
	 *
	 * Doing this with only 64-bit integers will require scaling to
	 * adequate bit-lengths and an inevitable loss of precision.
	 * Precision is not critical since this function will be used
	 * to approximate a mechanical movement's distance by timing.
	 */
	bl = fls(a) + fls(d) + fls(Ve) + fls(Vs);
	bl = max(0, (bl >> 1) - 16);
	Bt = div_u64(a * d * t2, MSEC_PER_SEC);

	/*
	 * All terms are shifted left by bl bits *twice* (!)
	 * This will go into the square-root, so the result needs to be
	 * shifted right by bl bits only *once*.
	 */
	ACt = -((a*a) >> bl)*(Ve2 >> bl) - ((d*d) >> bl)*(Vs2 >> bl) -
	      ((a*d) >> bl)*((2*d*Xt + 2*a*Xt + Vs2 + Ve2) >> bl);
	disc = (Bt >> bl) * (Bt >> bl) - ACt;
	if (disc < 0) {
		/* This should not be possible if (Ve, Vs) < Vm */
		WARN_ONCE(true, "Discriminator is negative!");
		disc = 0;
	}

	/*
	 * We have all the parts of the quadratic formula, so we can
	 * calculate Vmax. There are some constraints we can take
	 * for granted here:
	 *  - The term 4AC (ACt) is strictly negative, so the
	 *    discriminant will always be bigger than Bt^2.
	 *  - Due to this, the result of the square root will be
	 *    bigger than Bt, which means there will always be one
	 *    positive real solution for Vmax.
	 *  - The dividend (a + d) cannot be zero, since a and d are
	 *    both tested to be positive and non zero in
	 *    motion_set_profile().
	 */
	 /* NOLINTNEXTLINE(clang-analyzer-core.DivideZero) */
	Vmax = div64_u64(-Bt + ((u64)int_sqrt64(disc) << bl), a + d);
	Ve = min(Ve, Vmax);
	Vs = min(Vs, Vmax);
	dev_info(mdev->dev, "D2T: Vs=%u, Vmax=%llu, Ve=%u\n", Vs, Vmax, Ve);

	/* Try again with new Vmax. This time will always succeed. */
	__d2t_vmax(a, d, Vmax, Vs, Ve, Xt, t2, &Vs2, &Ve2, t);

	return 0;
}
EXPORT_SYMBOL(motion_distance_to_time);
