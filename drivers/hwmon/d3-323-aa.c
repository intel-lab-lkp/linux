// SPDX-License-Identifier: GPL-2.0
/*
 * d3-322-aa.c - support for the D3-323-AA Pyroelectric Passive Infrared Sensor
 *
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>

#define DELAY_MS 10

/*
 *        0  ...  8     9  ...  16    17    18  19    20  21  22    23    24 ...  46   47 48 49 50 51
 * -------------------------------------------------------------------------------------------------------
 * F37   F38 ... F46   F47 ... F54   F55   F56 F57   F58 F59 F60   F64   F65 ... F87   Input End Pattern
 *     |      0      |  threshold  |  0  |  FSTEP  |    FILSEL   |  0  |      0      |  0  1  1  0  1    |
 *
 * NOTE: F37 is not used
 */
#define THRESHOLD_OFFSET 9
#define THRESHOLD_LEN 8
#define FSTEP_OFFSET 18
#define FSTEP_LEN 2
#define FILSEL_OFFSET 20
#define FILSEL_LEN 3
#define INPUT_END_PATTERN_OFFSET 47
#define INPUT_END_PATTERN_LEN 5

#define REG_SETTING_SIZE 52

#define DEFAULT_THRESHOLD 0x1C
/* Input End Pattern: 01101 -> 10110 */
#define INPUT_END_PATTERN 0x16

#define SET_REGISTER_SEQ_CNT 104 /* (47 + 5) * 2 */
#define READ_REGISTER_SEQ_CNT 116 /* (47 + 1 + 10) * 2 */

static atomic_t clk_irq_count = ATOMIC_INIT(0);

enum filter_step {
	STEP_THREE = 0,
	STEP_ONE = 1,
	STEP_TWO = 3,
};

enum filter_type {
	TYPE_B = 0,
	TYPE_C = 1,
	TYPE_D = 2,
	TYPE_DIRECT = 3,
	TYPE_A = 7,
};

enum d3323aa_state {
	IDLE,
	POWER_ON,
	SETUP_WRITE,
	SETUP_READ,
	WAIT_FOR_STABLE,
	RUNNING,
};

struct d3323aa_data {
	struct device *dev;
	struct gpio_desc *clk;
	struct gpio_desc *si;
	struct gpio_desc *reset;
	u8 threshold; /* 0 ~ 255 */
	enum filter_step step;
	enum filter_type type;
	struct delayed_work setup_work;
	struct work_struct state_worker;
	struct hrtimer timer;
	/* Save the clk seq number */
	int seq;
	bool error;
	enum d3323aa_state state;
	bool detector;
	/* index of the bitmap */
	int idx;
	DECLARE_BITMAP(register_bitmap, REG_SETTING_SIZE);
};

static ssize_t pir_filter_type_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct d3323aa_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	switch (val) {
	case TYPE_A:
	case TYPE_B:
	case TYPE_C:
	case TYPE_D:
	case TYPE_DIRECT:
		data->type = val;
		break;
	default:
		return -EINVAL;
	}

	cancel_delayed_work_sync(&data->setup_work);
	schedule_delayed_work(&data->setup_work, msecs_to_jiffies(DELAY_MS));

	return count;
}

static ssize_t pir_filter_step_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct d3323aa_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	switch (val) {
	case STEP_ONE:
	case STEP_TWO:
	case STEP_THREE:
		data->type = val;
		break;
	default:
		return -EINVAL;
	}

	cancel_delayed_work_sync(&data->setup_work);
	schedule_delayed_work(&data->setup_work, msecs_to_jiffies(DELAY_MS));

	return count;
}

static ssize_t pir_threshold_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct d3323aa_data *data = dev_get_drvdata(dev);
	unsigned long val;
	int err;

	err = kstrtoul(buf, 10, &val);
	if (err)
		return err;

	if (val > 255)
		return -EINVAL;

	data->threshold = val;

	cancel_delayed_work_sync(&data->setup_work);
	schedule_delayed_work(&data->setup_work, msecs_to_jiffies(DELAY_MS));

	return count;
}

static ssize_t pir_detector_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct d3323aa_data *data = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", data->detector);
}

static DEVICE_ATTR_WO(pir_threshold);
static DEVICE_ATTR_WO(pir_filter_step);
static DEVICE_ATTR_WO(pir_filter_type);
static DEVICE_ATTR_RO(pir_detector);

static struct attribute *d3323aa_attrs[] = {
	&dev_attr_pir_threshold.attr,
	&dev_attr_pir_filter_step.attr,
	&dev_attr_pir_filter_type.attr,
	&dev_attr_pir_detector.attr,
	NULL,
};

ATTRIBUTE_GROUPS(d3323aa);

static void set_field(unsigned long *bmp, int start, int value, int size)
{
	int i;
	int mask = 1;

	for (i = 0; i < size; i++) {
		assign_bit(start + i, bmp, value & mask);
		mask <<= 1;
	}
}

static void build_register_data(struct d3323aa_data *data)
{
	unsigned long *bmap = data->register_bitmap;

	bitmap_zero(bmap, REG_SETTING_SIZE);

	set_field(bmap, THRESHOLD_OFFSET, data->threshold, THRESHOLD_LEN);

	set_field(bmap, FSTEP_OFFSET, data->step, FSTEP_LEN);

	set_field(bmap, FILSEL_OFFSET, data->type, FILSEL_LEN);

	set_field(bmap, INPUT_END_PATTERN_OFFSET, INPUT_END_PATTERN,
		  INPUT_END_PATTERN_LEN);
}

static irqreturn_t irq_handler(int irq, void *dev)
{
	struct d3323aa_data *data = dev;
	int count;

	if (data->state == POWER_ON) {
		int v = gpiod_get_value(data->clk);

		if (v == 1)
			return IRQ_HANDLED;

		count = atomic_inc_return(&clk_irq_count);

		/* This register setting and verification must be done during the
		 * configurable period and the starting point of the period is second
		 * falling edge of VOUT/CLK and DO/SI after turning on
		 */
		if (count == 2)
			schedule_work(&data->state_worker);
	} else {
		int v = gpiod_get_value(data->clk);

		data->detector = v ? true : false;
	}

	return IRQ_HANDLED;
}

static void d3323aa_reset(struct d3323aa_data *data)
{
	gpiod_set_value(data->reset, 1);

	/* The supply voltage should be less than 0.5V for 30msec,
	 * add 10ms more for VDD discharge
	 */
	msleep(40);
}

static void d3323aa_poweron(struct d3323aa_data *data)
{
	int ret;

	atomic_set(&clk_irq_count, 0);

	gpiod_direction_input(data->clk);
	gpiod_direction_input(data->si);

	if (data->state == RUNNING || data->state == POWER_ON)
		free_irq(gpiod_to_irq(data->clk), data);

	ret = devm_request_irq(data->dev, gpiod_to_irq(data->clk), irq_handler,
			       IRQF_TRIGGER_FALLING, "d3323aa_poweron_irq",
			       data);
	if (ret) {
		pr_err("Failed to request IRQ\n");
		return;
	}

	data->state = POWER_ON;
	data->detector = false;

	gpiod_set_value(data->reset, 0);
}

static void state_worker_func(struct work_struct *work)
{
	struct d3323aa_data *data =
		container_of(work, struct d3323aa_data, state_worker);

	switch (data->state) {
	case POWER_ON:
		free_irq(gpiod_to_irq(data->clk), data);

		gpiod_direction_output(data->clk, 0);
		gpiod_direction_output(data->si, 0);

		data->state = SETUP_WRITE;

		data->seq = 0;
		/* clk for register setting is 1kHz */
		hrtimer_start(&data->timer, ktime_set(0, 500 * 1000),
			      HRTIMER_MODE_REL_HARD);
		break;
	case SETUP_WRITE:
		/* si pin will receive the register setting */
		gpiod_direction_input(data->si);
		data->seq = 0;
		data->idx = -1; /* idx will be reset when dummy bit received */
		data->error = false;
		data->state = SETUP_READ;

		/* 9.5ms * 2 */
		hrtimer_start(&data->timer, ktime_set(0, ms_to_ktime(20)),
			      HRTIMER_MODE_REL_HARD);
		break;
	case SETUP_READ:
		/* clk pin will receive the pir detect signal */
		gpiod_direction_input(data->clk);
		data->state = WAIT_FOR_STABLE;

		/* The stability time(Max. 30 sec) is required for stability of signal
		 * output after turning on of VDD and after register setting.
		 */
		hrtimer_start(&data->timer, ktime_set(30, 0),
			      HRTIMER_MODE_REL_HARD);
		break;
	case WAIT_FOR_STABLE:
		int ret;

		data->state = RUNNING;
		ret = devm_request_irq(
			data->dev, gpiod_to_irq(data->clk), irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"d3323aa_detect_irq", data);

		break;
	default:
		break;
	}
}

static void setup_func(struct work_struct *work)
{
	struct d3323aa_data *data =
		container_of(work, struct d3323aa_data, setup_work.work);

	build_register_data(data);

	d3323aa_reset(data);

	d3323aa_poweron(data);
}

static enum hrtimer_restart hrtimer_handler(struct hrtimer *hrtimer)
{
	struct d3323aa_data *data =
		container_of(hrtimer, struct d3323aa_data, timer);

	switch (data->state) {
	case SETUP_WRITE:

		if (data->seq % 2 == 0)
			gpiod_set_value(data->clk, 0);
		else
			gpiod_set_value(data->clk, 1);

		gpiod_set_value(data->si,
				test_bit(data->seq / 2, data->register_bitmap));

		if (data->seq++ == SET_REGISTER_SEQ_CNT) {
			schedule_work(&data->state_worker);
			return HRTIMER_NORESTART;
		}
		break;
	case SETUP_READ:
		if (data->seq % 2 == 0) {
			gpiod_set_value(data->clk, 0);
		} else {
			gpiod_set_value(data->clk, 1);

			if (data->idx < 0) {
				/* Reset the idx when dummy bit received */
				if (gpiod_get_value(data->si) == 1)
					data->idx = 0;
			} else if (data->idx < REG_SETTING_SIZE) {
				if (gpiod_get_value(data->si) !=
				    test_bit(data->idx++,
					     data->register_bitmap))
					data->error = true;
			}
		}

		if (data->seq++ == READ_REGISTER_SEQ_CNT) {
			schedule_work(&data->state_worker);
			return HRTIMER_NORESTART;
		}
		break;
	case WAIT_FOR_STABLE:
		schedule_work(&data->state_worker);
		return HRTIMER_NORESTART;
	default:
		break;
	}

	hrtimer_forward_now(hrtimer, ktime_set(0, 500 * 1000));

	return HRTIMER_RESTART;
}

static int d3323aa_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct d3323aa_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	platform_set_drvdata(pdev, data);

	data->state = IDLE;
	INIT_WORK(&data->state_worker, state_worker_func);
	INIT_DELAYED_WORK(&data->setup_work, setup_func);
	hrtimer_init(&data->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	data->timer.function = hrtimer_handler;

	/* Set default register settings */
	data->threshold = DEFAULT_THRESHOLD;
	data->step = STEP_TWO;
	data->type = TYPE_B;

	/* Try requesting the GPIOs */
	data->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(data->reset)) {
		ret = PTR_ERR(data->reset);
		dev_err(dev, "Reset line GPIO request failed\n");
		goto err_release_reg;
	}

	data->clk = devm_gpiod_get(dev, "clk", GPIOD_OUT_LOW);
	if (IS_ERR(data->clk)) {
		ret = PTR_ERR(data->clk);
		dev_err(dev, "CLK line GPIO request failed\n");
		goto err_release_reg;
	}

	data->si = devm_gpiod_get(dev, "si", GPIOD_OUT_LOW);
	if (IS_ERR(data->si)) {
		ret = PTR_ERR(data->si);
		dev_err(dev, "SI line GPIO request failed\n");
		goto err_release_reg;
	}

	build_register_data(data);

	d3323aa_reset(data);

	d3323aa_poweron(data);

	hwmon_dev = devm_hwmon_device_register_with_groups(
		dev, pdev->name, data, d3323aa_groups);

	return PTR_ERR_OR_ZERO(hwmon_dev);

err_release_reg:
	return ret;
}

static const struct of_device_id d3323aa_dt_match[] = {
	{ .compatible = "nicera,d3-323-aa" },
	{},
};
MODULE_DEVICE_TABLE(of, d3323aa_dt_match);

static struct platform_driver d3323aa_driver = {
	.driver = {
		.name = "d3-323-aa",
		.of_match_table = of_match_ptr(d3323aa_dt_match),
	},
	.probe = d3323aa_probe,
};
module_platform_driver(d3323aa_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Nicera D3-323-AA Pyroelectric Infrared sensor driver");
