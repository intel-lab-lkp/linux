/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MOTION_CORE_H__
#define __MOTION_CORE_H__

#include <linux/major.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/irq_work.h>
#include <uapi/linux/motion.h>

struct motion_device;

/**
 * struct motion_ops - Motion driver API
 * @device_open: Called when the associated device is opened
 * @device_release: Called when the associated device is closed
 * @basic_run: Start a basic movement of channel @ch with speed @v. Speed is a
 *      signed value. Sign indicates direction.
 * @basic_stop: Immediately stops all active movements of all channels set in
 *      channel mask @ch.
 * @get_status: Get current speed and position (if supported) from the channel
 *      specified in st->channel.
 * @move_distance: Start a movement just like basic_run(), but stop after
 *      reaching the specified distance. Optional.
 * @move_timed: Start a movement just like basic_run(), but stop after a
 *      specified time. Optional.
 * @validate_profile: Check if all parameters of a specified movement profile
 *      (acceleration/speed curve) is valid for this driver. Optional.
 * @motion_distance: Start or prepare to start a movement following a specified
 *      motion profile until reaching the target distance. Optional.
 * @motion_timed: Start or prepare to start a movement following a specified
 *      motion profile that takes exactly @t time. Optional.
 * @motion_stop: Stop or prepare to stop a movement that was initiated with
 *      either motion_timed() or motion_distance() prematurely while following
 *      the deceleration segment of the profile the movement was started with.
 *      Optional.
 * @config_trigger: Setup a trigger @index for a certaing function @func that
 *      applies to all channels set in channel mask @ch. Only applies to
 *      internal triggers. Optional.
 * @external_trigger: Initiate a movement by external trigger on all channels
 *      set in channel mask @ch. Optional.
 *
 * Channel mask parameters of typo channel_mask_t are bitmasks that specify
 * multiple channels the call applies to simultaneously.
 *
 * The parameter @when specifies one of the MOT_WHEN_* values defined in the
 * motion UAPI.
 * The parameter @func specifies one of the MOT_FUNC_* values defined in the
 * motion UAPI.
 * The parameter @edge can be either MOT_EDGE_FALLING or MOT_EDGE_RISING.
 * The parameter @index either refers to the index of a motion profile, or the
 * index of an internal trigger intput depending on the context.
 *
 * All function calls specified as "Optional" above need to be implemented only
 * if the driver can support the required functionality.
 */
struct motion_ops {
	int (*device_open)(struct motion_device *mdev);
	int (*device_release)(struct motion_device *mdev);
	int (*basic_run)(struct motion_device *mdev, unsigned int ch, s32 v);
	int (*basic_stop)(struct motion_device *mdev, channel_mask_t ch);
	int (*get_status)(struct motion_device *mdev, struct mot_status *st);
	int (*move_distance)(struct motion_device *mdev, unsigned int ch,
			s32 v, u32 d);
	int (*move_timed)(struct motion_device *mdev, unsigned int ch, s32 v,
			ktime_t t);
	int (*validate_profile)(struct motion_device *mdev,
			struct mot_profile *p);
	int (*set_profile)(struct motion_device *mdev, struct mot_profile *p);
	int (*motion_distance)(struct motion_device *mdev, unsigned int ch,
			unsigned int index, s32 d, unsigned int when);
	int (*motion_timed)(struct motion_device *mdev, unsigned int ch,
			unsigned int index, unsigned int dir, ktime_t t,
			unsigned int when);
	int (*motion_stop)(struct motion_device *mdev, channel_mask_t ch,
			unsigned int when);
	int (*config_trigger)(struct motion_device *mdev, unsigned int index,
			unsigned int func, unsigned int edge, channel_mask_t ch);
	void (*external_trigger)(struct motion_device *mdev, unsigned int index,
			channel_mask_t ch);
};

struct motion_gpio_input {
	struct gpio_desc *gpio;
	unsigned int function;
	unsigned int edge;
	unsigned int index;
	channel_mask_t chmask;
};

/**
 * struct motion_device - Represents a motion control subsystem device
 * @ops: struct motion_ops implementing the functionality of the device.
 * @parent: Parent struct device. This can be an underlying SPI/I2C device or
 *      a platform device, etc... This is mandatory.
 * @dev: Newly created motion device associated with the denode. Filled in
 *      by motion_register_device().
 * @minor: The motion device minor number allocated by motion_register_device().
 * @list: Internal housekeeping.
 * @groups: Attribute groups of the device. The driver can add an entry to the
 *      attributes table if required. Should be used for all run-time parameters
 *      of the underlying hardware, like current limits, virtual stop positions,
 *      etc...
 * @nodename: Optional name of the devnode. Default NULL will use motionXX
 * @mode: Optional mode for the devnode.
 * @mutex: Mutex for serializing access to the device. Used by the core and
 *      locked during calls to the @ops. Should be locked by the driver if
 *      entered from other places, like interrupt threads.
 * @read_mutex: Mutex used by the core for serializing read() calls to the
 *      device.
 * @capabilities: struct mot_capabilities, describes the capabilities of the
 *      particular driver.
 * @profiles: Statically allocated list of motion profiles. The core stores
 *      motion profiles supplied by user-space in this list. The @index
 *      parameter in @ops calls is an index into this list if applicable.
 * @gpios: Statically allocated list of external trigger inputs associated with
 *      this device. These are specified in the fwnode.
 * @num_gpios: Number of external GPIO trigger inputs parsed from the fwnode.
 * @wait: poll() waitqueue for motion events to user-space.
 * @events: KFIFO of motion events.
 * @iiotrig: IIO trigger of motion events.
 * @iiowork: The irq_work that dispatches the IIO trigger events.
 * @helper_cookie: internal data for helper functions such as timed_speed
 *      helpers.
 *
 * Motion device drivers should (devm_)kzalloc this struct and fill in all
 * required information (@ops, @parent and @capabilities) and then call
 * motion_register_device() from their probe function.
 *
 * @parent should hold any drvdata for the driver if needed, and the drvdata
 * struct should contain this struct motion_device as a member, so that it can
 * be retrieved with container_of() macros.
 */
struct motion_device {
	struct motion_ops		ops;
	struct device			*parent;
	struct device			*dev;
	int				minor;
	struct list_head		list;
	const struct attribute_group	*groups[3];
	const char			*nodename;
	umode_t				mode;
	struct mutex			mutex;
	struct mutex			read_mutex;
	struct mot_capabilities		capabilities;
	struct mot_profile		profiles[MOT_MAX_PROFILES];
	struct motion_gpio_input	gpios[MOT_MAX_INPUTS];
	unsigned int			num_gpios;
	wait_queue_head_t		wait;
	DECLARE_KFIFO(events, struct mot_event, 16);
	struct iio_trigger		*iiotrig;
	struct irq_work			iiowork;
	void				*helper_cookie;
};

static inline ktime_t mot_time2ktime(mot_time_t t)
{
	return (ktime_t)t;
}

static inline mot_time_t ktime2mot_time(ktime_t t)
{
	return (mot_time_t)t;
}

int motion_register_device(struct motion_device *mdev);
void motion_unregister_device(struct motion_device *mdev);
void motion_report_event(struct motion_device *mdev, struct mot_event *evt);

#endif /* __MOTION_CORE_H__ */
