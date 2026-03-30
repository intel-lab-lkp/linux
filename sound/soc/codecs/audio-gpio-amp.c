// SPDX-License-Identifier: GPL-2.0-only
//
// GPIOs controlled amplifier ALSA SoC driver
//
// Copyright 2026 CS GROUP France
//
// Author: Herve Codina <herve.codina@bootlin.com>

#include <linux/bitmap.h>
#include <linux/gpio/consumer.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <sound/soc.h>
#include <sound/tlv.h>

struct audio_gpio_single {
	struct gpio_desc *gpio;
	bool is_inverted;
	int kctrl_val;
	const char *control_name;
};

struct audio_gpio_point {
	u32 gpio_val;
	int gain_db;
};

struct audio_gpio_points {
	unsigned int nb_points;
	struct audio_gpio_point *tab_points;
};

struct audio_gpio_range {
	int min_db;
	int max_db;
};

struct audio_gpio_labels {
	unsigned int nb_labels;
	const char **tab_labels;
};

enum audio_gpio_mode {
	AUDIO_GPIO_MODE_NONE,
	AUDIO_GPIO_MODE_RANGE,
	AUDIO_GPIO_MODE_LABELS,
	AUDIO_GPIO_MODE_POINTS,
};

struct audio_gpio_multi {
	struct gpio_descs *gpios;
	bool is_inverted;
	u32 kctrl_val;
	u32 kctrl_max;
	const char *control_name;
	unsigned int *tlv_array;
	enum audio_gpio_mode mode;
	union {
		struct audio_gpio_range range;
		struct audio_gpio_points points;
		struct audio_gpio_labels labels;
	};
};

struct audio_gpio_amp {
	struct audio_gpio_single mute;
	struct audio_gpio_single bypass;
	struct audio_gpio_multi gain;
};

static const struct snd_soc_dapm_widget audio_gpio_amp_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("IN"),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_PGA("PGA", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_REGULATOR_SUPPLY("vdd", 0, 0),
};

static const struct snd_soc_dapm_route audio_gpio_amp_dapm_routes[] = {
	{ "PGA", NULL, "IN" },
	{ "PGA", NULL, "vdd" },
	{ "OUT", NULL, "PGA" },
};

static int audio_gpio_amp_single_kctrl_write_gpio(struct audio_gpio_single *single,
						  int kctrl_val)
{
	int gpio_val;

	gpio_val = single->is_inverted ? !kctrl_val : kctrl_val;

	return gpiod_set_value_cansleep(single->gpio, gpio_val);
}

static int audio_gpio_amp_single_kctrl_info(struct snd_kcontrol *kcontrol,
					    struct snd_ctl_elem_info *uinfo)
{
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	return 0;
}

static int audio_gpio_amp_single_kctrl_get(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_single *single = (struct audio_gpio_single *)kcontrol->private_value;

	ucontrol->value.integer.value[0] = single->kctrl_val;

	return 0;
}

static int audio_gpio_amp_single_kctrl_put(struct snd_kcontrol *kcontrol,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_single *single = (struct audio_gpio_single *)kcontrol->private_value;
	int kctrl_val;
	int err;

	kctrl_val = ucontrol->value.integer.value[0];

	if (kctrl_val == single->kctrl_val)
		return 0;

	err = audio_gpio_amp_single_kctrl_write_gpio(single, kctrl_val);
	if (err)
		return err;

	single->kctrl_val = kctrl_val;

	return 1; /* The value changed */
}

static int audio_gpio_amp_single_add_kcontrol(struct snd_soc_component *component,
					      struct audio_gpio_single *single)
{
	struct snd_kcontrol_new control = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = single->control_name,
		.info = audio_gpio_amp_single_kctrl_info,
		.get = audio_gpio_amp_single_kctrl_get,
		.put = audio_gpio_amp_single_kctrl_put,
		.private_value = (unsigned long)single,
	};
	int ret;

	/* Be consistent between single->kctrl_val value and the GPIO value */
	ret = audio_gpio_amp_single_kctrl_write_gpio(single, single->kctrl_val);
	if (ret)
		return ret;

	return snd_soc_add_component_controls(component, &control, 1);
}

static int audio_gpio_amp_multi_kctrl_write_gpios(struct audio_gpio_multi *multi,
						  u32 kctrl_val)
{
	DECLARE_BITMAP(bm, 32);
	u32 gpio_val;

	if (multi->mode == AUDIO_GPIO_MODE_POINTS) {
		if (kctrl_val >= multi->points.nb_points)
			return -EINVAL;

		gpio_val = multi->points.tab_points[kctrl_val].gpio_val;
	} else {
		if (kctrl_val > multi->kctrl_max)
			return -EINVAL;

		gpio_val = multi->is_inverted ?
				multi->kctrl_max - kctrl_val :
				kctrl_val;
	}

	bitmap_from_arr32(bm, &gpio_val, multi->gpios->ndescs);

	return gpiod_multi_set_value_cansleep(multi->gpios, bm);
}

static int audio_gpio_amp_multi_kctrl_int_info(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_info *uinfo)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;

	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	if (multi->mode == AUDIO_GPIO_MODE_POINTS)
		uinfo->value.integer.max = multi->points.nb_points - 1;
	else
		uinfo->value.integer.max = multi->kctrl_max;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	return 0;
}

static int audio_gpio_amp_multi_kctrl_int_get(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;

	ucontrol->value.integer.value[0] = multi->kctrl_val;
	return 0;
}

static int audio_gpio_amp_multi_kctrl_int_put(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;
	u32 kctrl_val;
	int ret;

	kctrl_val = ucontrol->value.integer.value[0];

	if (kctrl_val == multi->kctrl_val)
		return 0;

	ret = audio_gpio_amp_multi_kctrl_write_gpios(multi, kctrl_val);
	if (ret)
		return ret;

	multi->kctrl_val = kctrl_val;

	return 1; /* The value changed */
}

static int audio_gpio_amp_multi_kctrl_enum_info(struct snd_kcontrol *kcontrol,
						struct snd_ctl_elem_info *uinfo)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;

	return snd_ctl_enum_info(uinfo, 1, multi->labels.nb_labels,
				 multi->labels.tab_labels);
}

static int audio_gpio_amp_multi_kctrl_enum_get(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;

	ucontrol->value.enumerated.item[0] = multi->kctrl_val;
	return 0;
}

static int audio_gpio_amp_multi_kctrl_enum_put(struct snd_kcontrol *kcontrol,
					       struct snd_ctl_elem_value *ucontrol)
{
	struct audio_gpio_multi *multi = (struct audio_gpio_multi *)kcontrol->private_value;
	u32 kctrl_val;
	int ret;

	kctrl_val = ucontrol->value.enumerated.item[0];

	if (kctrl_val == multi->kctrl_val)
		return 0;

	ret = audio_gpio_amp_multi_kctrl_write_gpios(multi, kctrl_val);
	if (ret)
		return ret;

	multi->kctrl_val = kctrl_val;

	return 1; /* The value changed */
}

static int *audio_gpio_amp_alloc_tlv_range(const struct audio_gpio_range *range)
{
	DECLARE_TLV_DB_MINMAX(tmp, range->min_db, range->max_db);

	return kmemdup(tmp, sizeof(tmp), GFP_KERNEL);
}

static int *audio_gpio_amp_alloc_tlv_points(struct audio_gpio_points *points)
{
	unsigned int *tlv;
	unsigned int *t;
	unsigned int i;

	tlv = kzalloc_objs(*tlv, 2 + points->nb_points * 6, GFP_KERNEL);
	if (!tlv)
		return NULL;

	t = tlv;

	/* Fill first TLV */
	*t++ = SNDRV_CTL_TLVT_DB_RANGE; /* Tag */
	*t++ = points->nb_points * 6 * sizeof(*tlv); /* Len */
	/* points are sorted from lower to higher value */
	for (i = 0; i < points->nb_points; i++) {
		/* Fill item i */
		*t++ = i;  /* min */
		*t++ = i;  /* max */
		*t++ = SNDRV_CTL_TLVT_DB_MINMAX; /* Tag */
		*t++ = 2 * sizeof(*tlv); /* Len */
		*t++ = points->tab_points[i].gain_db; /* min_dB */
		*t++ = points->tab_points[i].gain_db; /* max_dB */
	}

	return tlv;
}

static int audio_gpio_amp_multi_add_kcontrol(struct snd_soc_component *component,
					     struct audio_gpio_multi *multi)
{
	struct snd_kcontrol_new control = {
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = multi->control_name,
		.info = audio_gpio_amp_multi_kctrl_int_info,
		.get = audio_gpio_amp_multi_kctrl_int_get,
		.put = audio_gpio_amp_multi_kctrl_int_put,
		.private_value = (unsigned long)multi,
	};
	int ret;

	switch (multi->mode) {
	case AUDIO_GPIO_MODE_RANGE:
		multi->tlv_array = audio_gpio_amp_alloc_tlv_range(&multi->range);
		if (!multi->tlv_array)
			return -ENOMEM;

		control.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
				 SNDRV_CTL_ELEM_ACCESS_READWRITE;
		control.tlv.p = multi->tlv_array;
		break;

	case AUDIO_GPIO_MODE_POINTS:
		multi->tlv_array = audio_gpio_amp_alloc_tlv_points(&multi->points);
		if (!multi->tlv_array)
			return -ENOMEM;

		control.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |
				 SNDRV_CTL_ELEM_ACCESS_READWRITE;
		control.tlv.p = multi->tlv_array;
		break;

	case AUDIO_GPIO_MODE_LABELS:
		/* Use enumerated values */
		control.info = audio_gpio_amp_multi_kctrl_enum_info;
		control.get = audio_gpio_amp_multi_kctrl_enum_get;
		control.put = audio_gpio_amp_multi_kctrl_enum_put;
		break;

	case AUDIO_GPIO_MODE_NONE:
		/* Already set control configuration is enough */
		break;

	default:
		return -EINVAL;
	}

	/* Be consistent between multi->kctrl_val value and the GPIOs value */
	ret = audio_gpio_amp_multi_kctrl_write_gpios(multi, multi->kctrl_val);
	if (ret)
		return ret;

	return snd_soc_add_component_controls(component, &control, 1);
}

struct audio_gpio_amp_supply {
	const char *prop_name;
	const struct snd_soc_dapm_widget dapm_widget;
	const struct snd_soc_dapm_route dapm_route;
};

static const struct audio_gpio_amp_supply audio_gpio_amp_supplies[] = {
	{
		.prop_name = "vddio-supply",
		.dapm_widget = SND_SOC_DAPM_REGULATOR_SUPPLY("vddio", 0, 0),
		.dapm_route = { "PGA", NULL, "vddio" },
	}, {
		.prop_name = "vdda1-supply",
		.dapm_widget = SND_SOC_DAPM_REGULATOR_SUPPLY("vdda1", 0, 0),
		.dapm_route = { "PGA", NULL, "vdda1" },
	}, {
		.prop_name = "vdda2-supply",
		.dapm_widget = SND_SOC_DAPM_REGULATOR_SUPPLY("vdda2", 0, 0),
		.dapm_route = { "PGA", NULL, "vdda2" },
	},
	{ /* End of list */}
};

static int audio_gpio_amp_add_power_supplies(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	const struct audio_gpio_amp_supply *supply;
	struct device *dev = component->dev;
	int ret;

	supply = audio_gpio_amp_supplies;
	do {
		if (!of_property_present(dev->of_node, supply->prop_name))
			continue;

		ret = snd_soc_dapm_new_controls(dapm, &supply->dapm_widget, 1);
		if (ret) {
			dev_err(dev, "Failed to add control for '%s' (%d)\n",
				supply->prop_name, ret);
			return ret;
		}
		ret = snd_soc_dapm_add_routes(dapm, &supply->dapm_route, 1);
		if (ret) {
			dev_err(dev, "Failed to add route for '%s' (%d)\n",
				supply->prop_name, ret);
			return ret;
		}
	} while ((++supply)->prop_name);

	return 0;
}

static int audio_gpio_amp_component_probe(struct snd_soc_component *component)
{
	struct audio_gpio_amp *gpio_amp = snd_soc_component_get_drvdata(component);
	int ret;

	ret = audio_gpio_amp_add_power_supplies(component);
	if (ret)
		return ret;

	if (gpio_amp->mute.gpio) {
		/*
		 * The name of the GPIO used is mute. According to this name, 1
		 * means muted and 0 means un-muted.
		 *
		 * An inversion is expected by ALSA. Indeed from ALSA point of
		 * view, 1 means 'on' (un-muted) and 0 means 'off' (muted).
		 */
		gpio_amp->mute.is_inverted = true;
		gpio_amp->mute.kctrl_val = 1; /* Un-muted */
		ret = audio_gpio_amp_single_add_kcontrol(component, &gpio_amp->mute);
		if (ret)
			return ret;
	}

	if (gpio_amp->bypass.gpio) {
		ret = audio_gpio_amp_single_add_kcontrol(component, &gpio_amp->bypass);
		if (ret)
			return ret;
	}

	if (gpio_amp->gain.gpios) {
		ret = audio_gpio_amp_multi_add_kcontrol(component, &gpio_amp->gain);
		if (ret)
			return ret;
	}

	return 0;
}

static void audio_gpio_amp_component_remove(struct snd_soc_component *component)
{
	struct audio_gpio_amp *gpio_amp = snd_soc_component_get_drvdata(component);

	kfree(gpio_amp->gain.tlv_array);
	gpio_amp->gain.tlv_array = NULL;
}

static const struct snd_soc_component_driver audio_gpio_amp_component_driver = {
	.dapm_widgets		= audio_gpio_amp_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(audio_gpio_amp_dapm_widgets),
	.dapm_routes		= audio_gpio_amp_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(audio_gpio_amp_dapm_routes),
	.probe = audio_gpio_amp_component_probe,
	.remove = audio_gpio_amp_component_remove,
};

static int audio_gpio_amp_parse_labels(struct device *dev,
				       struct audio_gpio_multi *multi,
				       const char *labels_property)
{
	struct audio_gpio_labels *labels = &multi->labels;
	struct device_node *np = dev->of_node;
	int ret;

	ret = of_property_count_strings(np, labels_property);
	if (ret <= 0)
		return ret;

	labels->nb_labels = ret;
	if (labels->nb_labels > (1 << multi->gpios->ndescs))
		return -EINVAL;

	labels->tab_labels = devm_kcalloc(dev, labels->nb_labels,
					  sizeof(*labels->tab_labels),
					  GFP_KERNEL);
	if (!labels->tab_labels)
		return -ENOMEM;

	multi->kctrl_max = labels->nb_labels - 1;
	multi->kctrl_val = 0;
	multi->is_inverted = false;

	return of_property_read_string_array(np, labels_property, labels->tab_labels,
					     labels->nb_labels);
}

static int audio_gpio_amp_parse_range(struct device *dev,
				      struct audio_gpio_multi *multi,
				      const char *range_property)
{
	struct audio_gpio_range *range = &multi->range;
	struct device_node *np = dev->of_node;
	s32 tmp;
	int ret;

	ret = of_property_read_s32_index(np, range_property, 0, &tmp);
	if (ret)
		return ret;
	range->min_db = tmp;

	ret = of_property_read_s32_index(np, range_property, 1, &tmp);
	if (ret)
		return ret;
	range->max_db = tmp;

	multi->kctrl_max = (1 << multi->gpios->ndescs) - 1;
	multi->kctrl_val = 0;
	multi->is_inverted = false;

	if (range->min_db > range->max_db) {
		/* Invert range */
		swap(range->min_db, range->max_db);
		multi->is_inverted = 1;

		/*
		 * When the range is inverted, choose to have the initial
		 * amplification set to max_db (i.e. all GPIOs inactive).
		 */
		multi->kctrl_val = multi->kctrl_max;
	}

	return 0;
}

static int audio_gpio_amp_cmp_points(const void *a, const void *b)
{
	const struct audio_gpio_point *a_point = a;
	const struct audio_gpio_point *b_point = b;

	return a_point->gain_db - b_point->gain_db;
}

static int audio_gpio_amp_parse_points(struct device *dev,
				       struct audio_gpio_multi *multi,
				       const char *points_property)
{
	struct audio_gpio_points *points = &multi->points;
	struct device_node *np = dev->of_node;
	struct audio_gpio_point first_point;
	unsigned int max_gpio_val;
	unsigned int i;
	int ret;
	u32 u;
	s32 s;

	max_gpio_val = (1 << multi->gpios->ndescs) - 1;

	ret = of_property_count_u32_elems(np, points_property);
	if (ret <= 0)
		return ret;

	if (ret % 2)
		return -EINVAL;

	points->nb_points = ret / 2;
	if (points->nb_points > max_gpio_val + 1)
		return -EINVAL;

	points->tab_points = devm_kcalloc(dev, points->nb_points,
					  sizeof(*points->tab_points),
					  GFP_KERNEL);
	if (!points->tab_points)
		return -ENOMEM;

	for (i = 0; i < points->nb_points; i++) {
		/* Gpio value */
		ret = of_property_read_u32_index(np, points_property,
						 i * 2, &u);
		if (ret)
			return ret;
		if (u > max_gpio_val)
			return -EINVAL;

		points->tab_points[i].gpio_val = u;

		/* Gain value */
		ret = of_property_read_s32_index(np, points_property,
						 i * 2 + 1, &s);
		if (ret)
			return ret;

		points->tab_points[i].gain_db = s;
	}

	first_point = points->tab_points[0];

	/* Sort the tab_point array by gain_db value */
	sort(points->tab_points, points->nb_points, sizeof(*points->tab_points),
	     audio_gpio_amp_cmp_points, NULL);

	multi->kctrl_max = points->nb_points - 1;
	multi->is_inverted = false;

	/*
	 * multi->kctrl_val is the index in tab_points.
	 *
	 * Choose to have the initial amplification value set to the first item
	 * available in the tab_points array before sorting.
	 *
	 * This first point before sorting has been identified. Search for it in
	 * the sorted array in order to set the multi->kctrl_val initial value.
	 */
	for (i = 0; i < points->nb_points; i++) {
		if (points->tab_points[i].gpio_val == first_point.gpio_val &&
		    points->tab_points[i].gain_db == first_point.gain_db) {
			multi->kctrl_val = i;
			break;
		}
	}

	return 0;
}

static int audio_gpio_amp_parse_multi_gpio(struct device *dev,
					   struct audio_gpio_multi *multi,
					   const char *gpios_property,
					   const char *range_property,
					   const char *points_property,
					   const char *labels_property)
{
	struct device_node *np = dev->of_node;
	int ret;

	/* Start with the value 0 (GPIO inactive). Can be changed later */
	multi->is_inverted = false;
	multi->kctrl_val = 0;
	multi->gpios = devm_gpiod_get_array_optional(dev, gpios_property, GPIOD_OUT_LOW);
	if (IS_ERR(multi->gpios))
		return dev_err_probe(dev, PTR_ERR(multi->gpios),
				     "Failed to get '%s' gpios\n",
				     gpios_property);
	if (!multi->gpios)
		return 0;

	if (multi->gpios->ndescs > 32)
		return dev_err_probe(dev, -EINVAL,
				     "Number of '%s' gpios limited to 32\n",
				     labels_property);

	/* Set default value for the kctrl_max. Can be changed later */
	multi->kctrl_max = (1 << multi->gpios->ndescs) - 1;

	multi->mode = AUDIO_GPIO_MODE_NONE;
	if (of_property_present(np, points_property)) {
		ret = audio_gpio_amp_parse_points(dev, multi, points_property);
		if (ret < 0)
			return dev_err_probe(dev, ret, "Failed to parse '%s'\n",
					     points_property);
		multi->mode = AUDIO_GPIO_MODE_POINTS;

	} else if (of_property_present(np, range_property)) {
		ret = audio_gpio_amp_parse_range(dev, multi, range_property);
		if (ret < 0)
			return dev_err_probe(dev, ret, "Failed to parse '%s'\n",
					     range_property);
		multi->mode = AUDIO_GPIO_MODE_RANGE;

	} else if (of_property_present(np, labels_property)) {
		ret = audio_gpio_amp_parse_labels(dev, multi, labels_property);
		if (ret < 0)
			return dev_err_probe(dev, ret, "Failed to parse '%s'\n",
					     labels_property);

		multi->mode = AUDIO_GPIO_MODE_LABELS;
	}

	return 0;
}

static int audio_gpio_amp_parse_single_gpio(struct device *dev,
					    struct audio_gpio_single *single,
					    const char *gpio_property)
{
	/* Start with the inactive value */
	single->is_inverted = false;
	single->kctrl_val = 0;
	single->gpio = devm_gpiod_get_optional(dev, gpio_property, GPIOD_OUT_LOW);
	if (IS_ERR(single->gpio))
		return dev_err_probe(dev, PTR_ERR(single->gpio),
				     "Failed to get '%s' gpio\n",
				     gpio_property);
	return 0;
}

static int audio_gpio_amp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct audio_gpio_amp *gpio_amp;
	int ret;

	gpio_amp = devm_kzalloc(dev, sizeof(*gpio_amp), GFP_KERNEL);
	if (!gpio_amp)
		return -ENOMEM;

	ret = audio_gpio_amp_parse_single_gpio(dev, &gpio_amp->mute, "mute");
	if (ret)
		return ret;

	ret = audio_gpio_amp_parse_single_gpio(dev, &gpio_amp->bypass, "bypass");
	if (ret)
		return ret;

	ret = audio_gpio_amp_parse_multi_gpio(dev, &gpio_amp->gain, "gain",
					      "gain-range", "gain-points", "gain-labels");
	if (ret)
		return ret;

	/* Set controls name */
	gpio_amp->gain.control_name = "Volume";
	gpio_amp->mute.control_name = "Switch";
	gpio_amp->bypass.control_name = "Bypass Switch";

	if (gpio_amp->gain.mode == AUDIO_GPIO_MODE_LABELS) {
		/*
		 * The gain widget control will use enumerated values.
		 *
		 * Having just "Voltage" and "Switch" widget names with
		 * enumerated values and boolean value can confuse ALSA in terms
		 * of possible values (strings).
		 *
		 * Make things clear and avoid the just "Switch" name in that
		 * case.
		 */
		gpio_amp->mute.control_name = "Out Switch";
	}

	platform_set_drvdata(pdev, gpio_amp);

	return devm_snd_soc_register_component(dev,
					       &audio_gpio_amp_component_driver,
					       NULL, 0);
}

static const struct of_device_id audio_gpio_amp_ids[] = {
	{ .compatible = "audio-gpio-amp" },
	{ }
};
MODULE_DEVICE_TABLE(of, audio_gpio_amp_ids);

static struct platform_driver audio_gpio_amp_driver = {
	.driver = {
		.name = "audio-gpio-amp",
		.of_match_table = of_match_ptr(audio_gpio_amp_ids),
	},
	.probe = audio_gpio_amp_probe,
};
module_platform_driver(audio_gpio_amp_driver);

MODULE_AUTHOR("Herve Codina <herve.codina@bootlin.com>");
MODULE_DESCRIPTION("ASoC GPIOs controlled amplifier driver");
MODULE_LICENSE("GPL");
