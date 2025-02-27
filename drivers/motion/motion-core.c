// SPDX-License-Identifier: GPL-2.0
/*
 * Motion Control Subsystem - Core
 *
 * Copyright (C) 2024 Protonic Holland
 *                    David Jander <david@protonic.nl>
 */

#include <asm-generic/bitops/builtin-fls.h>
#include <asm-generic/errno-base.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/container_of.h>
#include <linux/hrtimer_types.h>
#include <linux/gfp_types.h>
#include <linux/module.h>

#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/kmod.h>
#include <linux/motion.h>
#include <linux/poll.h>
#include <linux/ptrace.h>
#include <linux/ktime.h>
#include <linux/iio/trigger.h>
#include <linux/gpio/consumer.h>

#include "motion-core.h"
#include "motion-helpers.h"
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/math.h>
#include <linux/math64.h>

#define MOTION_PROFILE_VALID BIT(31)

static LIST_HEAD(motion_list);
static DEFINE_MUTEX(motion_mtx);
static int motion_major;
static DEFINE_IDA(motion_minors_ida);

struct iio_motion_trigger_info {
	unsigned int minor;
};

static int motion_minor_alloc(void)
{
	int ret;

	ret = ida_alloc_range(&motion_minors_ida, 0, MINORMASK, GFP_KERNEL);
	return ret;
}

static void motion_minor_free(int minor)
{
	ida_free(&motion_minors_ida, minor);
}

static int motion_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	struct motion_device *mdev = NULL, *iter;
	int err;

	mutex_lock(&motion_mtx);

	list_for_each_entry(iter, &motion_list, list) {
		if (iter->minor != minor)
			continue;
		mdev = iter;
		break;
	}

	if (!mdev) {
		err = -ENODEV;
		goto fail;
	}

	dev_info(mdev->dev, "MOTION: open %d\n", mdev->minor);
	file->private_data = mdev;

	if (mdev->ops.device_open)
		err = mdev->ops.device_open(mdev);
	else
		err = 0;
fail:
	mutex_unlock(&motion_mtx);
	return err;
}

static int motion_release(struct inode *inode, struct file *file)
{
	struct motion_device *mdev = file->private_data;
	int i;

	if (mdev->ops.device_release)
		mdev->ops.device_release(mdev);

	for (i = 0; i < mdev->num_gpios; i++) {
		int irq;
		struct motion_gpio_input *gpio = &mdev->gpios[i];

		if (gpio->function == MOT_INP_FUNC_NONE)
			continue;
		irq = gpiod_to_irq(gpio->gpio);
		devm_free_irq(mdev->dev, irq, gpio);
		gpio->function = MOT_INP_FUNC_NONE;
	}

	if (!kfifo_is_empty(&mdev->events))
		kfifo_reset(&mdev->events);

	/* FIXME: Stop running motions? Probably not... */

	return 0;
}

static ssize_t motion_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	struct motion_device *mdev = file->private_data;
	unsigned int copied = 0L;
	int ret;

	if (!mdev->dev)
		return -ENODEV;

	if (count < sizeof(struct mot_event))
		return -EINVAL;

	do {
		if (kfifo_is_empty(&mdev->events)) {
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;

			ret = wait_event_interruptible(mdev->wait,
					!kfifo_is_empty(&mdev->events) ||
					mdev->dev == NULL);
			if (ret)
				return ret;
			if (mdev->dev == NULL)
				return -ENODEV;
		}

		if (mutex_lock_interruptible(&mdev->read_mutex))
			return -ERESTARTSYS;
		ret = kfifo_to_user(&mdev->events, buffer, count, &copied);
		mutex_unlock(&mdev->read_mutex);

		if (ret)
			return ret;
	} while (!copied);

	return copied;
}

static __poll_t motion_poll(struct file *file, poll_table *wait)
{
	struct motion_device *mdev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &mdev->wait, wait);
	if (!kfifo_is_empty(&mdev->events))
		mask = EPOLLIN | EPOLLRDNORM;
	dev_info(mdev->dev, "Obtained POLL events: 0x%08x\n", mask);

	return mask;
}

static long motion_move_distance(struct motion_device *mdev,
		channel_mask_t ch, speed_raw_t speed, pos_raw_t distance)
{
	ktime_t time;
	u64 tmp;
	u64 tmpmul = NSEC_PER_SEC; /* Convert speed (1/s) to time in nsec */

	if (mdev->ops.move_distance)
		return mdev->ops.move_distance(mdev, ch, speed, distance);

	if (!mdev->ops.move_timed)
		return -EOPNOTSUPP;

	if (!speed)
		return -EINVAL;

	/*
	 * Handling of potential integer overflows when converting distance
	 * to time duration without sacrificing too much precision:
	 * speed_conv_div and speed_conv_mul can be very large, yet the
	 * resulting quotient is most likely a lot smaller. If we do the
	 * multiplication first we retain the highest precision, but we need
	 * to be mindful of integer overflows, so we do one test to see if there
	 * are enough bits left to increase the precision further.
	 */
	tmp = ((u64)distance * mdev->capabilities.speed_conv_div);
	if (tmp < (1ULL << 48)) {
		tmpmul = NSEC_PER_MSEC;
		tmp *= MSEC_PER_SEC;
	}
	tmp = div_u64(tmp, mdev->capabilities.speed_conv_mul);
	tmp = div_u64(tmp, speed);
	time = tmp * tmpmul;
	return mdev->ops.move_timed(mdev, ch, speed, time);
}

static long motion_move_timed(struct motion_device *mdev, channel_mask_t ch,
		speed_raw_t speed, mot_time_t duration)
{
	ktime_t t;

	if (mdev->ops.move_timed) {
		t = mot_time2ktime(duration);
		return mdev->ops.move_timed(mdev, ch, speed, t);
	}

	return -EOPNOTSUPP;
}

static long motion_set_profile_locked(struct motion_device *mdev,
		struct mot_profile *prof)
{
	long ret;
	struct mot_profile *dst;
	int i;

	lockdep_assert_held(&mdev->mutex);

	if ((prof->na > mdev->capabilities.max_apoints) ||
			(prof->nv > mdev->capabilities.max_vpoints))
		return -EINVAL;

	/* Check if used acceleration values are positive and non zero */
	for (i = 0; i < prof->na; i++)
		if (prof->acc[i] <= 0)
			return -EINVAL;

	if (!mdev->ops.validate_profile || !mdev->ops.set_profile)
		return -EOPNOTSUPP;

	if (prof->index >= MOT_MAX_PROFILES)
		return -EINVAL;

	ret = mdev->ops.validate_profile(mdev, prof);
	if (ret)
		return ret;

	dst = &mdev->profiles[prof->index];

	*dst = *prof;
	dst->index |= MOTION_PROFILE_VALID;

	return 0L;
}

static long motion_get_profile_locked(struct motion_device *mdev, u32 index,
		struct mot_profile *dst)
{
	struct mot_profile *src;

	lockdep_assert_held(&mdev->mutex);

	if (index >= MOT_MAX_PROFILES)
		return -EINVAL;

	if (!(mdev->profiles[index].index & MOTION_PROFILE_VALID))
		return -EINVAL;

	src = &mdev->profiles[index];
	*dst = *src;

	return 0L;
}

static long motion_start_locked(struct motion_device *mdev, struct mot_start *s)
{
	long ret = 0L;
	mot_time_t conv_duration;

	lockdep_assert_held(&mdev->mutex);

	if (s->reserved1 || s->reserved2)
		return -EINVAL;
	if (s->channel >= mdev->capabilities.num_channels)
		return -EINVAL;
	if ((s->index >= MOT_MAX_PROFILES) || (s->direction > MOT_DIRECTION_RIGHT))
		return -EINVAL;
	if (!(mdev->profiles[s->index].index & MOTION_PROFILE_VALID))
		return -EINVAL;
	if (s->when >= MOT_WHEN_NUM_WHENS)
		return -EINVAL;
	if (s->duration && s->distance)
		return -EINVAL;
	if (!mdev->ops.motion_distance && !mdev->ops.motion_timed)
		return -EOPNOTSUPP;
	if (s->duration) {
		if (!mdev->ops.motion_timed)
			return -EOPNOTSUPP;
		/* FIXME: Implement time to distance conversion? */
		return mdev->ops.motion_timed(mdev, s->channel, s->index,
				s->direction, s->duration, s->when);
	}
	if (!mdev->ops.motion_distance) {
		ret = motion_distance_to_time(mdev, s->index, s->distance,
				&conv_duration);
		if (ret)
			return ret;
		return mdev->ops.motion_timed(mdev, s->channel, s->index,
				s->direction, conv_duration, s->when);
	}
	ret = mdev->ops.motion_distance(mdev, s->channel, s->index,
			s->distance, s->when);

	return ret;
}

static irqreturn_t motion_gpio_interrupt(int irq, void *dev_id)
{
	struct motion_gpio_input *gpio = dev_id;
	struct motion_device *mdev = container_of(gpio, struct motion_device,
			gpios[gpio->index]);
	struct mot_event evt = {0};
	struct mot_status st;
	int val = gpiod_get_raw_value(gpio->gpio);
	channel_mask_t chmsk;
	channel_mask_t chmsk_l = 0;
	channel_mask_t chmsk_r = 0;

	dev_info(mdev->dev, "GPIO IRQ val=%d edge=%d\n", val, gpio->edge);
	/* FIXME: This is racy and we shouldn't try to support shared IRQ! */
	if ((gpio->edge == MOT_EDGE_FALLING) && val)
		return IRQ_NONE;

	if ((gpio->edge == MOT_EDGE_RISING) && !val)
		return IRQ_NONE;

	evt.event = MOT_EVENT_INPUT;
	evt.input_index = gpio->index;
	evt.timestamp = ktime2mot_time(ktime_get());

	mutex_lock(&mdev->mutex);
	/* FIXME: It may be possible and desirable to obtain position and
	 * speed from multiple channels with one call to the driver.
	 */
	chmsk = gpio->chmask;
	while (chmsk) {
		unsigned int ch = ffs(chmsk) - 1;

		chmsk &= ~(1 << ch);
		evt.channel = ch;
		st.channel = ch;
		mdev->ops.get_status(mdev, &st);
		evt.speed = st.speed;
		evt.position = st.position;
		motion_report_event(mdev, &evt);
		if (st.speed < 0)
			chmsk_l |= (1 << ch);
		else if (st.speed > 0)
			chmsk_r |= (1 << ch);
	}

	switch (gpio->function) {
	case MOT_INP_FUNC_STOP_NEG:
		if (chmsk_l)
			mdev->ops.basic_stop(mdev, chmsk_l);
		break;
	case MOT_INP_FUNC_STOP_POS:
		if (chmsk_r)
			mdev->ops.basic_stop(mdev, chmsk_r);
		break;
	case MOT_INP_FUNC_STOP:
		mdev->ops.basic_stop(mdev, gpio->chmask);
		break;
	case MOT_INP_FUNC_DECEL_NEG:
		if (chmsk_l)
			mdev->ops.motion_stop(mdev, chmsk_l, MOT_WHEN_IMMEDIATE);
		break;
	case MOT_INP_FUNC_DECEL_POS:
		if (chmsk_r)
			mdev->ops.motion_stop(mdev, chmsk_r, MOT_WHEN_IMMEDIATE);
		break;
	case MOT_INP_FUNC_DECEL:
		mdev->ops.motion_stop(mdev, gpio->chmask, MOT_WHEN_IMMEDIATE);
		break;
	case MOT_INP_FUNC_START:
		if (mdev->ops.external_trigger)
			mdev->ops.external_trigger(mdev, gpio->index,
					gpio->chmask);
		break;
	default:
		break;
	}
	mutex_unlock(&mdev->mutex);

	return IRQ_HANDLED;
}

static int motion_config_gpio(struct motion_device *mdev, int idx,
		unsigned int func, unsigned int edge, channel_mask_t chmsk)
{
	struct motion_gpio_input *gpio = &mdev->gpios[idx];
	bool irq_claimed = false;
	int irq = gpiod_to_irq(gpio->gpio);
	int flags;

	if (gpio->function != MOT_INP_FUNC_NONE) {
		if (func == MOT_INP_FUNC_NONE)
			devm_free_irq(mdev->dev, irq, mdev);
		irq_claimed = true;
	}
	gpio->chmask = chmsk;
	gpio->function = func;
	gpio->edge = edge;
	if (!irq_claimed) {
		if (edge == MOT_EDGE_FALLING)
			flags = IRQF_TRIGGER_FALLING;
		else
			flags = IRQF_TRIGGER_RISING;
		flags |= IRQF_SHARED | IRQF_ONESHOT;
		dev_info(mdev->dev, "Claiming GPIO IRQ %d\n", irq);
		return devm_request_threaded_irq(mdev->dev, irq, NULL,
				motion_gpio_interrupt, flags,
				dev_name(mdev->dev), gpio);
	}

	return 0;
}

static long motion_config_input_locked(struct motion_device *mdev, struct mot_input *inp)
{
	int idx;

	lockdep_assert_held(&mdev->mutex);

	if (!inp->external)
		return mdev->ops.config_trigger(mdev, inp->index, inp->function,
				inp->edge, inp->chmask);

	idx = inp->index;
	idx -= mdev->capabilities.num_ext_triggers - mdev->num_gpios;
	/*
	 * FIXME: idx is now the index of GPIO external trigger.
	 * Other types of external triggers are not yet supported.
	 */
	if ((idx >= mdev->num_gpios) || (idx < 0)) {
		WARN_ONCE(true, "Input index unexpectedly out of range.");
		return -EINVAL;
	}
	return motion_config_gpio(mdev, idx, inp->function, inp->edge,
			inp->chmask);
}

static long motion_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct motion_device *mdev = file->private_data;
	void __user *argp = (void __user *)arg;
	long ret;

	switch (cmd) {
	case MOT_IOCTL_APIVER:
		force_successful_syscall_return();
		return MOT_UAPI_VERSION;
	case MOT_IOCTL_BASIC_RUN: {
		struct mot_speed_duration spd;

		if (copy_from_user(&spd, argp, sizeof(spd)))
			return -EFAULT;
		if (!mdev->ops.basic_run)
			return -EINVAL;
		if (spd.channel >= mdev->capabilities.num_channels)
			return -EINVAL;
		if (spd.distance && spd.duration)
			return -EINVAL;
		/* FIXME: Check reserved for zero! */
		mutex_lock(&mdev->mutex);
		if (!spd.distance && !spd.duration)
			ret = mdev->ops.basic_run(mdev, spd.channel, spd.speed);
		else if (spd.distance)
			ret = motion_move_distance(mdev, spd.channel,
					spd.speed, spd.distance);
		else
			ret = motion_move_timed(mdev, spd.channel, spd.speed,
				mot_time2ktime(spd.duration));
		mutex_unlock(&mdev->mutex);
		break;
	}
	case MOT_IOCTL_BASIC_STOP: {
		u32 ch;

		if (copy_from_user(&ch, argp, sizeof(ch)))
			return -EFAULT;
		/* Stop takes channel mask as only argument */
		if (fls(ch) > mdev->capabilities.num_channels)
			return -EINVAL;
		mutex_lock(&mdev->mutex);
		ret = mdev->ops.basic_stop(mdev, ch);
		mutex_unlock(&mdev->mutex);
		break;
	}
	case MOT_IOCTL_GET_CAPA:
		ret = copy_to_user(argp, &mdev->capabilities, sizeof(struct mot_capabilities));
		break;
	case MOT_IOCTL_GET_STATUS: {
		struct mot_status st;

		if (copy_from_user(&st, argp, sizeof(st)))
			return -EFAULT;
		if (st.channel >= mdev->capabilities.num_channels)
			return -EINVAL;
		if (!mdev->ops.get_status)
			return -EINVAL;
		mutex_lock(&mdev->mutex);
		ret = mdev->ops.get_status(mdev, &st);
		mutex_unlock(&mdev->mutex);
		if (ret)
			break;
		ret = copy_to_user(argp, &st, sizeof(struct mot_status));
		break;
	}
	case MOT_IOCTL_SET_PROFILE: {
		struct mot_profile prof;

		if (copy_from_user(&prof, argp, sizeof(prof)))
			return -EFAULT;
		mutex_lock(&mdev->mutex);
		ret = motion_set_profile_locked(mdev, &prof);
		mutex_unlock(&mdev->mutex);
		break;
	}
	case MOT_IOCTL_GET_PROFILE: {
		struct mot_profile prof;

		if (copy_from_user(&prof, argp, sizeof(prof)))
			return -EFAULT;
		mutex_lock(&mdev->mutex);
		ret = motion_get_profile_locked(mdev, prof.index, &prof);
		mutex_unlock(&mdev->mutex);
		if (ret)
			break;
		ret = copy_to_user(argp, &prof, sizeof(prof));
		break;
	}
	case MOT_IOCTL_START: {
		struct mot_start start;

		if (copy_from_user(&start, argp, sizeof(start)))
			return -EFAULT;
		mutex_lock(&mdev->mutex);
		ret = motion_start_locked(mdev, &start);
		mutex_unlock(&mdev->mutex);
		break;
	}
	case MOT_IOCTL_STOP: {
		struct mot_stop stop;

		if (copy_from_user(&stop, argp, sizeof(stop)))
			return -EFAULT;
		if (fls(stop.chmask) > mdev->capabilities.num_channels)
			return -EINVAL;
		if (stop.when >= MOT_WHEN_NUM_WHENS)
			return -EINVAL;
		if (!mdev->ops.motion_stop)
			return -EINVAL;
		mutex_lock(&mdev->mutex);
		ret = mdev->ops.motion_stop(mdev, stop.chmask, stop.when);
		mutex_unlock(&mdev->mutex);
		break;
	}
	case MOT_IOCTL_CONFIG_INPUT: {
		struct mot_input inp;

		if (copy_from_user(&inp, argp, sizeof(inp)))
			return -EFAULT;
		if (fls(inp.chmask) > mdev->capabilities.num_channels)
			return -EINVAL;
		if ((inp.external > 1) || (inp.function > MOT_INP_FUNC_NUM_FUNCS))
			return -EINVAL;
		if (!inp.external && (inp.index >= mdev->capabilities.num_int_triggers))
			return -EINVAL;
		if (inp.external && (inp.index >= mdev->capabilities.num_ext_triggers))
			return -EINVAL;
		if (!inp.external && !mdev->ops.config_trigger)
			return -EOPNOTSUPP;
		mutex_lock(&mdev->mutex);
		ret = motion_config_input_locked(mdev, &inp);
		mutex_unlock(&mdev->mutex);
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static char *motion_devnode(const struct device *dev, umode_t *mode)
{
	const struct motion_device *mdev = dev_get_drvdata(dev);

	if (mode && mdev->mode)
		*mode = mdev->mode;
	if (mdev->nodename)
		return kstrdup(mdev->nodename, GFP_KERNEL);
	return NULL;
}

static const struct class motion_class = {
	.name		= "motion",
	.devnode	= motion_devnode,
};

static const struct file_operations motion_fops = {
	.owner		= THIS_MODULE,
	.read		= motion_read,
	.poll		= motion_poll,
	.unlocked_ioctl = motion_ioctl,
	.open		= motion_open,
	.llseek		= noop_llseek,
	.release	= motion_release,
};

static int motion_of_parse_gpios(struct motion_device *mdev)
{
	int ngpio, i;

	ngpio = gpiod_count(mdev->parent, "motion,input");
	if (ngpio < 0) {
		if (ngpio == -ENOENT)
			return 0;
		return ngpio;
	}

	if (ngpio >= MOT_MAX_INPUTS)
		return -EINVAL;

	for (i = 0; i < ngpio; i++) {
		mdev->gpios[i].gpio = devm_gpiod_get_index(mdev->parent,
				"motion,input", i, GPIOD_IN);
		if (IS_ERR(mdev->gpios[i].gpio))
			return PTR_ERR(mdev->gpios[i].gpio);
		mdev->gpios[i].function = MOT_INP_FUNC_NONE;
		mdev->gpios[i].chmask = 0;
		mdev->gpios[i].index = i;
	}

	mdev->num_gpios = ngpio;
	mdev->capabilities.num_ext_triggers += ngpio;

	return 0;
}

static void motion_trigger_work(struct irq_work *work)
{
	struct motion_device *mdev = container_of(work, struct motion_device,
							iiowork);
	iio_trigger_poll(mdev->iiotrig);
}

/**
 * motion_register_device - Register a new Motion Device
 * @mdev: description and handle of the motion device
 *
 * Register a new motion device with the motion subsystem core.
 * It also handles OF parsing of external trigger GPIOs and registers an IIO
 * trigger device if IIO support is configured.
 *
 * Return: 0 on success, negative errno on failure.
 */
int motion_register_device(struct motion_device *mdev)
{
	dev_t devt;
	int err = 0;
	struct iio_motion_trigger_info *trig_info;

	if (!mdev->capabilities.num_channels)
		mdev->capabilities.num_channels = 1;
	if (mdev->capabilities.features | MOT_FEATURE_PROFILE)
		mdev->capabilities.max_profiles = MOT_MAX_PROFILES;
	if (!mdev->capabilities.speed_conv_mul)
		mdev->capabilities.speed_conv_mul = 1;
	if (!mdev->capabilities.speed_conv_div)
		mdev->capabilities.speed_conv_div = 1;
	if (!mdev->capabilities.accel_conv_mul)
		mdev->capabilities.accel_conv_mul = 1;
	if (!mdev->capabilities.accel_conv_div)
		mdev->capabilities.accel_conv_div = 1;

	mutex_init(&mdev->mutex);
	mutex_init(&mdev->read_mutex);
	INIT_KFIFO(mdev->events);
	init_waitqueue_head(&mdev->wait);

	err = motion_of_parse_gpios(mdev);
	if (err)
		return err;

	mdev->minor = motion_minor_alloc();

	mdev->iiotrig = iio_trigger_alloc(NULL, "mottrig%d", mdev->minor);
	if (!mdev->iiotrig) {
		err = -ENOMEM;
		goto error_free_minor;
	}

	trig_info = kzalloc(sizeof(*trig_info), GFP_KERNEL);
	if (!trig_info) {
		err = -ENOMEM;
		goto error_free_trigger;
	}

	iio_trigger_set_drvdata(mdev->iiotrig, trig_info);

	trig_info->minor = mdev->minor;
	err = iio_trigger_register(mdev->iiotrig);
	if (err)
		goto error_free_trig_info;

	mdev->iiowork = IRQ_WORK_INIT_HARD(motion_trigger_work);

	INIT_LIST_HEAD(&mdev->list);

	mutex_lock(&motion_mtx);

	devt = MKDEV(motion_major, mdev->minor);
	mdev->dev = device_create_with_groups(&motion_class, mdev->parent,
				devt, mdev, mdev->groups, "motion%d", mdev->minor);
	if (IS_ERR(mdev->dev)) {
		dev_err(mdev->parent, "Error creating motion device %d\n",
				mdev->minor);
		mutex_unlock(&motion_mtx);
		goto error_free_trig_info;
	}
	list_add_tail(&mdev->list, &motion_list);
	mutex_unlock(&motion_mtx);

	return 0;

error_free_trig_info:
	kfree(trig_info);
error_free_trigger:
	iio_trigger_free(mdev->iiotrig);
error_free_minor:
	motion_minor_free(mdev->minor);
	dev_info(mdev->parent, "Registering motion device err=%d\n", err);
	return err;
}
EXPORT_SYMBOL(motion_register_device);

void motion_unregister_device(struct motion_device *mdev)
{
	struct iio_motion_trigger_info *trig_info;

	trig_info = iio_trigger_get_drvdata(mdev->iiotrig);
	iio_trigger_unregister(mdev->iiotrig);
	kfree(trig_info);
	iio_trigger_free(mdev->iiotrig);
	mutex_lock(&motion_mtx);
	list_del(&mdev->list);
	device_destroy(&motion_class, MKDEV(motion_major, mdev->minor));
	motion_minor_free(mdev->minor);
	mdev->dev = NULL; /* Trigger chardev read abort */
	mutex_unlock(&motion_mtx);
}
EXPORT_SYMBOL(motion_unregister_device);

/**
 * motion_report_event - Report an event to the motion core.
 * @mdev: The motion device reporting the event
 * @evt: The event to be reported and queued.
 *
 * Drivers should call this function when there is a motion event, such as
 * target reached or a (virtual-) stop triggered. This applies only to internal
 * trigger inputs; external GPIO trigger events are handled by the core.
 */
void motion_report_event(struct motion_device *mdev, struct mot_event *evt)
{
	int ret;

	dev_info(mdev->dev, "Report event: %d\n", evt->event);
	switch (evt->event) {
	case MOT_EVENT_INPUT:
	case MOT_EVENT_TARGET:
	case MOT_EVENT_STOP:
		ret = kfifo_put(&mdev->events, *evt);
		if (ret)
			wake_up_poll(&mdev->wait, EPOLLIN);
		irq_work_queue(&mdev->iiowork);
		break;
	default:
		break;
	}

}
EXPORT_SYMBOL(motion_report_event);

static int __init motion_init(void)
{
	int err;

	err = class_register(&motion_class);
	if (err)
		return err;

	motion_major = register_chrdev(0, "motion", &motion_fops);
	if (motion_major <= 0) {
		err = -EIO;
		goto fail;
	}
	return 0;

fail:
	pr_err("unable to get major number for motion devices\n");
	class_unregister(&motion_class);
	return err;
}
subsys_initcall(motion_init);
