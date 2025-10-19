// PWM RGB LED driver with per-color grouped LED arrays

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/list.h>

#define MAX_LEDS_PER_COLOR 5

enum led_color_channel {
	LED_RED = 0,
	LED_GREEN = 1,
	LED_BLUE = 2,
	LED_COLOR_MAX
};

struct color_channel {
	struct led_classdev **leds;      // Array of LEDs for this color
	int num_leds;                     // Count of LEDs in this color
	enum led_brightness brightness;   // Current brightness for this color
	char *color_name;
};

struct virtual_led {
	struct led_classdev cdev;
	struct led_classdev **monochromatics;     // Regular monochromatic LEDs
	struct color_channel colors[LED_COLOR_MAX];  // RGB color channels
	int num_monochromatics;
	struct leds_virtualcolor *vc_data;
	int priority;
	struct list_head list;
};

struct leds_virtualcolor {
	struct virtual_led *vleds;
	int num_vleds;
	struct list_head active_leds;
	struct mutex lock;
};

/**
 * Apply brightness to all LEDs of a specific color
 */
static void color_set_brightness(struct color_channel *color,
				  enum led_brightness brightness)
{
	int i;

	for (i = 0; i < color->num_leds; i++)
		led_set_brightness(color->leds[i], brightness);

	color->brightness = brightness;
}

/**
 * Apply brightness to monochromatic LEDs
 */
static void virtual_set_monochromatic_brightness(struct virtual_led *vled,
						  enum led_brightness brightness)
{
	int i;

	for (i = 0; i < vled->num_monochromatics; i++)
		led_set_brightness(vled->monochromatics[i], brightness);
}

/**
 * Apply brightness to all RGB color channels
 */
static void virtual_set_rgb_brightness(struct virtual_led *vled,
				       enum led_brightness brightness)
{
	int i;

	for (i = 0; i < LED_COLOR_MAX; i++) {
		if (vled->colors[i].num_leds > 0)
			color_set_brightness(&vled->colors[i], brightness);
	}
}

/**
 * Set independent brightness per color (R, G, B)
 */
static void virtual_set_rgb_independent(struct virtual_led *vled,
					 enum led_brightness r,
					 enum led_brightness g,
					 enum led_brightness b)
{
	if (vled->colors[LED_RED].num_leds > 0)
		color_set_brightness(&vled->colors[LED_RED], r);

	if (vled->colors[LED_GREEN].num_leds > 0)
		color_set_brightness(&vled->colors[LED_GREEN], g);

	if (vled->colors[LED_BLUE].num_leds > 0)
		color_set_brightness(&vled->colors[LED_BLUE], b);
}

static ssize_t priority_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	int priority;

	mutex_lock(&vled->vc_data->lock);
	priority = vled->priority;
	mutex_unlock(&vled->vc_data->lock);

	return sysfs_emit(buf, "%d\n", priority);
}

static ssize_t priority_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	int new_priority;
	int ret;

	ret = kstrtoint(buf, 10, &new_priority);
	if (ret < 0)
		return ret;

	mutex_lock(&vled->vc_data->lock);
	vled->priority = new_priority;
	mutex_unlock(&vled->vc_data->lock);

	return count;
}

/**
 * Per-color brightness sysfs attributes
 */
static ssize_t red_brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", vled->colors[LED_RED].brightness);
}

static ssize_t red_brightness_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	int brightness;
	int ret;

	ret = kstrtoint(buf, 10, &brightness);
	if (ret < 0)
		return ret;

	if (brightness < 0 || brightness > LED_FULL)
		return -EINVAL;

	mutex_lock(&vled->vc_data->lock);
	color_set_brightness(&vled->colors[LED_RED], brightness);
	mutex_unlock(&vled->vc_data->lock);

	return count;
}

static ssize_t green_brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", vled->colors[LED_GREEN].brightness);
}

static ssize_t green_brightness_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	int brightness;
	int ret;

	ret = kstrtoint(buf, 10, &brightness);
	if (ret < 0)
		return ret;

	if (brightness < 0 || brightness > LED_FULL)
		return -EINVAL;

	mutex_lock(&vled->vc_data->lock);
	color_set_brightness(&vled->colors[LED_GREEN], brightness);
	mutex_unlock(&vled->vc_data->lock);

	return count;
}

static ssize_t blue_brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", vled->colors[LED_BLUE].brightness);
}

static ssize_t blue_brightness_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct virtual_led *vled = dev_get_drvdata(dev);
	int brightness;
	int ret;

	ret = kstrtoint(buf, 10, &brightness);
	if (ret < 0)
		return ret;

	if (brightness < 0 || brightness > LED_FULL)
		return -EINVAL;

	mutex_lock(&vled->vc_data->lock);
	color_set_brightness(&vled->colors[LED_BLUE], brightness);
	mutex_unlock(&vled->vc_data->lock);

	return count;
}

static DEVICE_ATTR_RW(priority);
static DEVICE_ATTR_RW(red_brightness);
static DEVICE_ATTR_RW(green_brightness);
static DEVICE_ATTR_RW(blue_brightness);

static struct attribute *virtual_led_attrs[] = {
	&dev_attr_priority.attr,
	&dev_attr_red_brightness.attr,
	&dev_attr_green_brightness.attr,
	&dev_attr_blue_brightness.attr,
	NULL,
};

static const struct attribute_group virtual_led_attr_group = {
	.attrs = virtual_led_attrs,
};

static bool virtual_led_is_active(struct list_head *head, struct virtual_led *vled)
{
	struct virtual_led *entry;

	list_for_each_entry(entry, head, list) {
		if (entry == vled)
			return true;
	}

	return false;
}

static int virtual_led_brightness_set(struct led_classdev *cdev,
				      enum led_brightness brightness)
{
	struct virtual_led *vled = container_of(cdev, struct virtual_led, cdev);
	struct leds_virtualcolor *vc_data = vled->vc_data;
	struct virtual_led *active;

	mutex_lock(&vc_data->lock);

	active = list_first_entry_or_null(&vc_data->active_leds, struct virtual_led, list);
	if (active) {
		if (active->priority > vled->priority)
			goto out_unlock;

		virtual_set_monochromatic_brightness(active, LED_OFF);
		virtual_set_rgb_brightness(active, LED_OFF);
	}

	if (brightness == LED_OFF) {
		if (virtual_led_is_active(&vc_data->active_leds, vled))
			list_del(&vled->list);

		active = list_first_entry_or_null(&vc_data->active_leds, struct virtual_led, list);
		if (active) {
			virtual_set_monochromatic_brightness(active, active->cdev.brightness);
			/* Restore each color to its saved brightness */
			virtual_set_rgb_independent(active,
						    active->colors[LED_RED].brightness,
						    active->colors[LED_GREEN].brightness,
						    active->colors[LED_BLUE].brightness);
		}
	} else {
		if (!virtual_led_is_active(&vc_data->active_leds, vled))
			list_add(&vled->list, &vc_data->active_leds);

		active = list_first_entry_or_null(&vc_data->active_leds, struct virtual_led, list);
		if (active) {
			virtual_set_monochromatic_brightness(active, brightness);
			virtual_set_rgb_brightness(active, brightness);
		}
	}

out_unlock:
	mutex_unlock(&vc_data->lock);

	return 0;
}

/**
 * Parse monochromatic LEDs from device tree
 */
static int parse_monochromatic_leds(struct device *dev, struct device_node *child,
				    struct virtual_led *vled)
{
	u32 mono_count;
	int ret, i;

	mono_count = of_property_count_elems_of_size(child, "leds", sizeof(u32));
	if (mono_count <= 0) {
		vled->num_monochromatics = 0;
		return 0;
	}

	vled->num_monochromatics = mono_count;
	vled->monochromatics = devm_kcalloc(dev, vled->num_monochromatics,
					    sizeof(*vled->monochromatics), GFP_KERNEL);
	if (!vled->monochromatics)
		return -ENOMEM;

	for (i = 0; i < vled->num_monochromatics; i++) {
		struct led_classdev *led_cdev;

		led_cdev = devm_of_led_get(dev, i);
		if (IS_ERR(led_cdev)) {
			ret = PTR_ERR(led_cdev);
			return dev_err_probe(dev, ret,
					     "Failed to get monochromatic LED %d\n", i);
		}

		vled->monochromatics[i] = led_cdev;
	}

	return 0;
}

/**
 * Parse color-grouped PWM LEDs from device tree
 * Format:
 *   pwm-leds-red = <&pwm_r0 &pwm_r1>;
 *   pwm-leds-green = <&pwm_g0 &pwm_g1>;
 *   pwm-leds-blue = <&pwm_b0>;
 */
static int parse_pwm_color_leds(struct device *dev, struct device_node *child,
				struct virtual_led *vled)
{
	const char *color_props[] = {"pwm-leds-red", "pwm-leds-green", "pwm-leds-blue"};
	const char *color_names[] = {"Red", "Green", "Blue"};
	int ret, c, i, count;

	for (c = 0; c < LED_COLOR_MAX; c++) {
		count = of_property_count_elems_of_size(child, color_props[c], sizeof(u32));
		if (count < 0) {
			vled->colors[c].num_leds = 0;
			continue;
		}

		if (count > MAX_LEDS_PER_COLOR) {
			return dev_err_probe(dev, -EINVAL,
					     "Too many %s LEDs (%d), max %d\n",
					     color_names[c], count, MAX_LEDS_PER_COLOR);
		}

		vled->colors[c].num_leds = count;
		vled->colors[c].color_name = (char *)color_names[c];
		vled->colors[c].brightness = LED_OFF;

		vled->colors[c].leds = devm_kcalloc(dev, count,
						    sizeof(*vled->colors[c].leds), GFP_KERNEL);
		if (!vled->colors[c].leds)
			return -ENOMEM;

		for (i = 0; i < count; i++) {
			struct led_classdev *led_cdev;

			led_cdev = devm_of_led_get(dev, i);
			if (IS_ERR(led_cdev)) {
				ret = PTR_ERR(led_cdev);
				return dev_err_probe(dev, ret,
						     "Failed to get %s LED %d\n",
						     color_names[c], i);
			}

			vled->colors[c].leds[i] = led_cdev;
		}

		dev_dbg(dev, "Parsed %d %s LEDs\n", count, color_names[c]);
	}

	return 0;
}

static int leds_virtualcolor_init_vled(struct device *dev, struct device_node *child,
				       struct virtual_led *vled,
				       struct leds_virtualcolor *vc_data)
{
	struct led_init_data init_data = {};
	u32 max_brightness;
	int ret, total_rgb_leds = 0, c;

	ret = of_property_read_u32(child, "priority", &vled->priority);
	if (ret)
		vled->priority = 0;

	INIT_LIST_HEAD(&vled->list);

	/* Parse monochromatic LEDs */
	ret = parse_monochromatic_leds(dev, child, vled);
	if (ret)
		return ret;

	/* Parse color-grouped PWM LEDs */
	ret = parse_pwm_color_leds(dev, child, vled);
	if (ret)
		return ret;

	/* Count total RGB LEDs */
	for (c = 0; c < LED_COLOR_MAX; c++)
		total_rgb_leds += vled->colors[c].num_leds;

	/* At least one type of LED must be present */
	if (vled->num_monochromatics == 0 && total_rgb_leds == 0) {
		return dev_err_probe(dev, -EINVAL,
				     "No LEDs specified for virtual LED\n");
	}

	ret = of_property_read_u32(child, "max-brightness", &max_brightness);
	if (ret)
		vled->cdev.max_brightness = LED_FULL;
	else
		vled->cdev.max_brightness = max_brightness;

	vled->cdev.brightness_set_blocking = virtual_led_brightness_set;
	vled->cdev.flags = LED_CORE_SUSPENDRESUME;

	ret = devm_led_classdev_register_ext(dev, &vled->cdev, &init_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register virtual LED\n");

	dev_set_drvdata(vled->cdev.dev, vled);

	ret = devm_device_add_group(vled->cdev.dev, &virtual_led_attr_group);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to create sysfs attributes\n");

	vled->vc_data = vc_data;

	dev_info(dev, "Virtual LED: %d mono, %d red, %d green, %d blue\n",
		 vled->num_monochromatics, vled->colors[LED_RED].num_leds,
		 vled->colors[LED_GREEN].num_leds, vled->colors[LED_BLUE].num_leds);

	return 0;
}

static int leds_virtualcolor_probe(struct platform_device *pdev)
{
	struct leds_virtualcolor *vc_data;
	struct device *dev = &pdev->dev;
	int count = 0;
	int ret;

	vc_data = devm_kzalloc(dev, sizeof(*vc_data), GFP_KERNEL);
	if (!vc_data)
		return -ENOMEM;

	mutex_init(&vc_data->lock);

	ret = devm_add_action_or_reset(dev, (void (*)(void *))mutex_destroy,
				       &vc_data->lock);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&vc_data->active_leds);

	vc_data->num_vleds = of_get_child_count(dev->of_node);
	if (vc_data->num_vleds == 0) {
		dev_err(dev, "No virtual LEDs defined\n");
		return -EINVAL;
	}

	vc_data->vleds = devm_kcalloc(dev, vc_data->num_vleds, sizeof(*vc_data->vleds), GFP_KERNEL);
	if (!vc_data->vleds)
		return -ENOMEM;

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		struct virtual_led *vled = &vc_data->vleds[count];

		ret = leds_virtualcolor_init_vled(dev, child, vled, vc_data);
		if (ret)
			return ret;

		count++;
	}

	platform_set_drvdata(pdev, vc_data);

	return 0;
}

static const struct of_device_id leds_virtualcolor_of_match[] = {
	{ .compatible = "leds-group-virtualcolor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, leds_virtualcolor_of_match);

static struct platform_driver leds_virtualcolor_driver = {
	.probe  = leds_virtualcolor_probe,
	.driver = {
		.name           = "leds_virtualcolor",
		.of_match_table = leds_virtualcolor_of_match,
	},
};

module_platform_driver(leds_virtualcolor_driver);

MODULE_AUTHOR("Radoslav Tsvetkov <rtsvetkov@gradotech.eu>");
MODULE_DESCRIPTION("Virtual Color LED Driver with RGB Color Grouping");
MODULE_LICENSE("GPL");
