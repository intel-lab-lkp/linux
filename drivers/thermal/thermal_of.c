// SPDX-License-Identifier: GPL-2.0
/*
 *  of-thermal.c - Generic Thermal Management device tree support.
 *
 *  Copyright (C) 2013 Texas Instruments
 *  Copyright (C) 2013 Eduardo Valentin <eduardo.valentin@ti.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/string.h>

#include "thermal_core.h"

/***   functions parsing device tree nodes   ***/

/*
 * It maps 'enum thermal_trip_type' found in include/linux/thermal.h
 * into the device tree binding of 'trip', property type.
 */
static const char * const trip_types[] = {
	[THERMAL_TRIP_ACTIVE]	= "active",
	[THERMAL_TRIP_PASSIVE]	= "passive",
	[THERMAL_TRIP_HOT]	= "hot",
	[THERMAL_TRIP_CRITICAL]	= "critical",
};

/**
 * thermal_of_get_trip_type - Get phy mode for given device_node
 * @np:	Pointer to the given device_node
 * @type: Pointer to resulting trip type
 *
 * The function gets trip type string from property 'type',
 * and store its index in trip_types table in @type,
 *
 * Return: 0 on success, or errno in error case.
 */
static int thermal_of_get_trip_type(struct device_node *np,
				    enum thermal_trip_type *type)
{
	const char *t;
	int err, i;

	err = of_property_read_string(np, "type", &t);
	if (err < 0)
		return err;

	for (i = 0; i < ARRAY_SIZE(trip_types); i++)
		if (!strcasecmp(t, trip_types[i])) {
			*type = i;
			return 0;
		}

	return -ENODEV;
}

/*
 * thermal_of_match_hw_bin_group - test one thermal-hw-bin sub-group
 * @np:     device node containing the property
 * @hw:     hardware binning info (must be non-NULL)
 * @levels: number of words per sub-group, equal to hw->supported_hw_bin_count
 * @group:  sub-group index to test
 *
 * Returns true if every word in sub-group @group has a non-zero bitwise
 * AND with the corresponding hw->supported_hw_bin[] entry.  The caller is
 * responsible for ensuring @group < total-cells / @levels.
 */
static bool thermal_of_match_hw_bin_group(struct device_node *np,
					  const struct thermal_hw_bin_info *hw,
					  unsigned int levels, unsigned int group)
{
	unsigned int j;
	u32 val;

	for (j = 0; j < levels; j++) {
		if (of_property_read_u32_index(np, "thermal-hw-bin",
					       group * levels + j, &val))
			return false;
		if (!(val & hw->supported_hw_bin[j]))
			return false;
	}

	return true;
}

/**
 * thermal_of_trip_is_supported - check if a trip node is valid for this hardware
 * @np:  device node for the trip point
 * @hw:  hardware binning info (may be NULL)
 *
 * If @hw is NULL or the trip node has no thermal-hw-bin property, the trip is
 * considered valid for all hardware.  Otherwise, the property is read as groups
 * of hw->supported_hw_bin_count 32-bit masks; the trip is valid if any group
 * matches (bitwise AND non-zero for every word in the group).
 *
 * Return: true if the trip is valid for the running hardware, false otherwise.
 */
static bool thermal_of_trip_is_supported(struct device_node *np,
					 const struct thermal_hw_bin_info *hw)
{
	unsigned int levels, ngroups, i;
	int count;

	if (!hw)
		return true;

	levels = hw->supported_hw_bin_count;

	count = of_property_count_u32_elems(np, "thermal-hw-bin");
	if (count == -EINVAL)
		return true;
	if (count < 0) {
		pr_warn_once("%pOF: error reading thermal-hw-bin: %d\n", np, count);
		return false;
	}
	if (count == 0)
		return true;

	if (count % levels) {
		pr_warn("%pOF: thermal-hw-bin length %d not a multiple of supported_hw_bin_count %u; disabling trip\n",
			np, count, levels);
		return false;
	}

	ngroups = count / levels;
	for (i = 0; i < ngroups; i++)
		if (thermal_of_match_hw_bin_group(np, hw, levels, i))
			return true;

	return false;
}

/**
 * thermal_of_trip_read_temperature - read trip temperature, with bin support
 * @np:   device node for the trip point
 * @hw:   hardware binning info (may be NULL)
 * @temp: output temperature in millicelsius
 *
 * Tries the scalar 'temperature' property first.  If absent, falls back to
 * 'temperature-bin', selecting the entry whose corresponding thermal-hw-bin
 * sub-group matches the running hardware.  Returns -ENODEV if temperature-bin
 * is present but no sub-group matches (caller should skip this trip).
 *
 * Return: 0 on success, negative errno on failure.
 */
static int thermal_of_trip_read_temperature(struct device_node *np,
					    const struct thermal_hw_bin_info *hw,
					    int *temp)
{
	unsigned int levels, ngroups, i;
	int count_hw, count_temp;
	s32 t;

	if (!of_property_read_s32(np, "temperature", &t)) {
		*temp = t;
		return 0;
	}

	count_temp = of_property_count_elems_of_size(np, "temperature-bin",
						     sizeof(u32));
	if (count_temp < 0)
		return -EINVAL;

	if (!hw) {
		u32 utmp;

		pr_warn("%pOF: temperature-bin requires platform hw binning info; using first entry\n",
			np);
		if (of_property_read_u32_index(np, "temperature-bin", 0, &utmp))
			return -EINVAL;
		*temp = (s32)utmp;
		return 0;
	}

	levels   = hw->supported_hw_bin_count;
	count_hw = of_property_count_u32_elems(np, "thermal-hw-bin");
	if (count_hw <= 0 || count_hw % levels)
		return -EINVAL;

	ngroups = count_hw / levels;
	if (count_temp != (int)ngroups)
		return -EINVAL;

	for (i = 0; i < ngroups; i++) {
		if (thermal_of_match_hw_bin_group(np, hw, levels, i)) {
			u32 utmp;

			if (of_property_read_u32_index(np, "temperature-bin",
						       i, &utmp))
				return -EINVAL;
			*temp = (s32)utmp;
			return 0;
		}
	}

	pr_warn("%pOF: no thermal-hw-bin sub-group matched for temperature-bin\n", np);
	return -ENODEV;
}

static int thermal_of_populate_trip(struct device_node *np,
				    struct thermal_trip *trip,
				    const struct thermal_hw_bin_info *hw)
{
	u32 hysteresis;
	s32 temperature;
	int ret;

	if (!thermal_of_trip_is_supported(np, hw)) {
		pr_debug("%pOF: trip skipped: no thermal-hw-bin match\n", np);
		return -ENODEV;
	}

	ret = thermal_of_trip_read_temperature(np, hw, &temperature);
	if (ret)
		return ret;
	trip->temperature = temperature;

	ret = of_property_read_u32(np, "hysteresis", &hysteresis);
	if (ret < 0) {
		pr_err("missing hysteresis property\n");
		return ret;
	}
	trip->hysteresis = hysteresis;

	ret = thermal_of_get_trip_type(np, &trip->type);
	if (ret < 0) {
		pr_err("wrong trip type property\n");
		return ret;
	}

	trip->flags = THERMAL_TRIP_FLAG_RW_TEMP;

	trip->priv = np;

	return 0;
}

static struct thermal_trip *thermal_of_trips_init(struct device_node *np,
						  int *ntrips,
						  const struct thermal_hw_bin_info *hw)
{
	int ret, count;

	*ntrips = 0;

	struct device_node *trips __free(device_node) = of_get_child_by_name(np, "trips");
	if (!trips)
		return NULL;

	count = of_get_child_count(trips);
	if (!count)
		return NULL;

	struct thermal_trip *tt __free(kfree) = kzalloc_objs(*tt, count);
	if (!tt)
		return ERR_PTR(-ENOMEM);

	count = 0;
	for_each_child_of_node_scoped(trips, trip) {
		ret = thermal_of_populate_trip(trip, &tt[count], hw);
		if (ret == -ENODEV)
			continue;
		if (ret)
			return ERR_PTR(ret);
		count++;
	}

	*ntrips = count;

	return no_free_ptr(tt);
}

static struct device_node *of_thermal_zone_find(struct device_node *sensor, int id)
{
	struct of_phandle_args sensor_specs;

	struct device_node *np __free(device_node) = of_find_node_by_name(NULL, "thermal-zones");
	if (!np) {
		pr_debug("No thermal zones description\n");
		return ERR_PTR(-ENODEV);
	}

	/*
	 * Search for each thermal zone, a defined sensor
	 * corresponding to the one passed as parameter
	 */
	for_each_available_child_of_node_scoped(np, child) {

		int count, i;

		count = of_count_phandle_with_args(child, "thermal-sensors",
						   "#thermal-sensor-cells");
		if (count <= 0) {
			pr_err("%pOFP: missing thermal sensor\n", child);
			return ERR_PTR(-EINVAL);
		}

		for (i = 0; i < count; i++) {

			int ret;

			ret = of_parse_phandle_with_args(child, "thermal-sensors",
							 "#thermal-sensor-cells",
							 i, &sensor_specs);
			if (ret < 0) {
				pr_err("%pOFP: Failed to read thermal-sensors cells: %d\n", child, ret);
				return ERR_PTR(ret);
			}

			of_node_put(sensor_specs.np);
			if ((sensor == sensor_specs.np) && id == (sensor_specs.args_count ?
								  sensor_specs.args[0] : 0)) {
				pr_debug("sensor %pOFP id=%d belongs to %pOFP\n", sensor, id, child);
				return no_free_ptr(child);
			}
		}
	}

	return ERR_PTR(-ENODEV);
}

static int thermal_of_monitor_init(struct device_node *np, int *delay, int *pdelay)
{
	int ret;

	ret = of_property_read_u32(np, "polling-delay-passive", pdelay);
	if (ret == -EINVAL) {
		*pdelay = 0;
	} else if (ret < 0) {
		pr_err("%pOFP: Couldn't get polling-delay-passive: %d\n", np, ret);
		return ret;
	}

	ret = of_property_read_u32(np, "polling-delay", delay);
	if (ret == -EINVAL) {
		*delay = 0;
	} else if (ret < 0) {
		pr_err("%pOFP: Couldn't get polling-delay: %d\n", np, ret);
		return ret;
	}

	return 0;
}

static void thermal_of_parameters_init(struct device_node *np,
				       struct thermal_zone_params *tzp)
{
	int coef[2];
	int ncoef = ARRAY_SIZE(coef);
	int prop, ret;

	tzp->no_hwmon = true;

	if (!of_property_read_u32(np, "sustainable-power", &prop))
		tzp->sustainable_power = prop;

	/*
	 * For now, the thermal framework supports only one sensor per
	 * thermal zone. Thus, we are considering only the first two
	 * values as slope and offset.
	 */
	ret = of_property_read_u32_array(np, "coefficients", coef, ncoef);
	if (ret) {
		coef[0] = 1;
		coef[1] = 0;
	}

	tzp->slope = coef[0];
	tzp->offset = coef[1];
}

static struct device_node *thermal_of_zone_get_by_name(struct thermal_zone_device *tz)
{
	struct device_node *np, *tz_np;

	np = of_find_node_by_name(NULL, "thermal-zones");
	if (!np)
		return ERR_PTR(-ENODEV);

	tz_np = of_get_child_by_name(np, tz->type);

	of_node_put(np);

	if (!tz_np)
		return ERR_PTR(-ENODEV);

	return tz_np;
}

static bool thermal_of_get_cooling_spec(struct device_node *map_np, int index,
					struct thermal_cooling_device *cdev,
					struct cooling_spec *c)
{
	struct of_phandle_args cooling_spec;
	int ret, weight = THERMAL_WEIGHT_DEFAULT;

	of_property_read_u32(map_np, "contribution", &weight);

	ret = of_parse_phandle_with_args(map_np, "cooling-device", "#cooling-cells",
					 index, &cooling_spec);

	if (ret < 0) {
		pr_err("Invalid cooling-device entry\n");
		return false;
	}

	of_node_put(cooling_spec.np);

	/*
	 * There are two formats:
	 * - Legacy format :	<&cdev lower upper>
	 * - New format    :	<&cdev cdev_id lower upper>
	 *
	 * With the new format, along with the device node pointer,
	 * the cdev_id must match with the cooling device cdev_id in
	 * order to bind
	 */
	if (cooling_spec.args_count < 2 || cooling_spec.args_count > 3) {
		pr_err("Invalid number of cooling device parameters\n");
		return false;
	}

	if (cooling_spec.np != cdev->np)
		return false;

	if (cooling_spec.args_count == 3 &&
	    cooling_spec.args[0] != cdev->cdev_id)
		return false;

	if (cooling_spec.args_count != 3) {
		c->lower = cooling_spec.args[0];
		c->upper = cooling_spec.args[1];
	} else {
		c->lower = cooling_spec.args[1];
		c->upper = cooling_spec.args[2];
	}
	c->weight = weight;

	return true;
}

static bool thermal_of_cm_lookup(struct device_node *cm_np,
				 const struct thermal_trip *trip,
				 struct thermal_cooling_device *cdev,
				 struct cooling_spec *c)
{
	for_each_child_of_node_scoped(cm_np, child) {
		int count, i;

		struct device_node *tr_np __free(device_node) =
			of_parse_phandle(child, "trip", 0);
		if (tr_np != trip->priv)
			continue;

		/* The trip has been found, look up the cdev. */
		count = of_count_phandle_with_args(child, "cooling-device",
						   "#cooling-cells");
		if (count <= 0)
			pr_err("Add a cooling_device property with at least one device\n");

		for (i = 0; i < count; i++) {
			if (thermal_of_get_cooling_spec(child, i, cdev, c))
				return true;
		}
	}

	return false;
}

static bool thermal_of_should_bind(struct thermal_zone_device *tz,
				   const struct thermal_trip *trip,
				   struct thermal_cooling_device *cdev,
				   struct cooling_spec *c)
{
	struct device_node *tz_np, *cm_np;
	bool result = false;

	tz_np = thermal_of_zone_get_by_name(tz);
	if (IS_ERR(tz_np)) {
		pr_err("Failed to get node tz by name\n");
		return false;
	}

	cm_np = of_get_child_by_name(tz_np, "cooling-maps");
	if (!cm_np)
		goto out;

	/* Look up the trip and the cdev in the cooling maps. */
	result = thermal_of_cm_lookup(cm_np, trip, cdev, c);

	of_node_put(cm_np);
out:
	of_node_put(tz_np);

	return result;
}

/**
 * thermal_of_zone_unregister - Cleanup the specific allocated ressources
 *
 * This function disables the thermal zone and frees the different
 * ressources allocated specific to the thermal OF.
 *
 * @tz: a pointer to the thermal zone structure
 */
static void thermal_of_zone_unregister(struct thermal_zone_device *tz)
{
	thermal_zone_device_disable(tz);
	thermal_zone_device_unregister(tz);
}

/**
 * thermal_of_zone_register_with_bin - Register a thermal zone with device node
 * sensor and optional hardware binning info
 *
 * @sensor:   A device node pointer corresponding to the sensor in the device tree
 * @id:       An integer as sensor identifier
 * @data:     A private data to be stored in the thermal zone dedicated private area
 * @ops:      A set of thermal sensor ops
 * @hw_bin_info: Optional hardware binning info for trip filtering (may be NULL)
 *
 * Return: a valid thermal zone structure pointer on success.
 *	- EINVAL: if the device tree thermal description is malformed
 *	- ENOMEM: if one structure can not be allocated
 *	- Other negative errors are returned by the underlying called functions
 */
static struct thermal_zone_device *
thermal_of_zone_register_with_bin(struct device_node *sensor, int id, void *data,
				  const struct thermal_zone_device_ops *ops,
				  const struct thermal_hw_bin_info *hw_bin_info)
{
	struct thermal_zone_device_ops of_ops = *ops;
	struct thermal_zone_device *tz;
	struct thermal_trip *trips;
	struct thermal_zone_params tzp = {};
	struct device_node *np;
	const char *action;
	int delay, pdelay;
	int ntrips;
	int ret;

	np = of_thermal_zone_find(sensor, id);
	if (IS_ERR(np)) {
		if (PTR_ERR(np) != -ENODEV)
			pr_err("Failed to find thermal zone for %pOFP id=%d\n", sensor, id);
		return ERR_CAST(np);
	}

	trips = thermal_of_trips_init(np, &ntrips, hw_bin_info);
	if (IS_ERR(trips)) {
		pr_err("Failed to parse trip points for %pOFP id=%d\n", sensor, id);
		ret = PTR_ERR(trips);
		goto out_of_node_put;
	}

	if (!trips)
		pr_info("No trip points found for %pOFP id=%d\n", sensor, id);

	ret = thermal_of_monitor_init(np, &delay, &pdelay);
	if (ret) {
		pr_err("Failed to initialize monitoring delays from %pOFP\n", np);
		goto out_kfree_trips;
	}

	thermal_of_parameters_init(np, &tzp);

	of_ops.should_bind = thermal_of_should_bind;

	ret = of_property_read_string(np, "critical-action", &action);
	if (!ret && !of_ops.critical) {
		if (!strcasecmp(action, "reboot"))
			of_ops.critical = thermal_zone_device_critical_reboot;
		else if (!strcasecmp(action, "shutdown"))
			of_ops.critical = thermal_zone_device_critical_shutdown;
	}

	tz = thermal_zone_device_register_with_trips(np->name, trips, ntrips,
						     data, &of_ops, &tzp,
						     pdelay, delay);
	if (IS_ERR(tz)) {
		ret = PTR_ERR(tz);
		pr_err("Failed to register thermal zone %pOFP: %d\n", np, ret);
		goto out_kfree_trips;
	}

	of_node_put(np);
	kfree(trips);

	ret = thermal_zone_device_enable(tz);
	if (ret) {
		pr_err("Failed to enable thermal zone '%s', id=%d: %d\n",
		       tz->type, tz->id, ret);
		thermal_of_zone_unregister(tz);
		return ERR_PTR(ret);
	}

	return tz;

out_kfree_trips:
	kfree(trips);
out_of_node_put:
	of_node_put(np);

	return ERR_PTR(ret);
}

/**
 * thermal_of_zone_register - Register a thermal zone with device node sensor
 *
 * @sensor: A device node pointer corresponding to the sensor in the device tree
 * @id:     An integer as sensor identifier
 * @data:   A private data to be stored in the thermal zone dedicated private area
 * @ops:    A set of thermal sensor ops
 *
 * Return: a valid thermal zone structure pointer on success.
 *	- EINVAL: if the device tree thermal description is malformed
 *	- ENOMEM: if one structure can not be allocated
 *	- Other negative errors are returned by the underlying called functions
 */
static struct thermal_zone_device *
thermal_of_zone_register(struct device_node *sensor, int id, void *data,
			 const struct thermal_zone_device_ops *ops)
{
	return thermal_of_zone_register_with_bin(sensor, id, data, ops, NULL);
}

static void devm_thermal_of_zone_release(struct device *dev, void *res)
{
	thermal_of_zone_unregister(*(struct thermal_zone_device **)res);
}

static int devm_thermal_of_zone_match(struct device *dev, void *res,
				      void *data)
{
	struct thermal_zone_device **r = res;

	if (WARN_ON(!r || !*r))
		return 0;

	return *r == data;
}

/**
 * devm_thermal_of_zone_register - register a thermal tied with the sensor life cycle
 *
 * This function is the device version of the thermal_of_zone_register() function.
 *
 * @dev: a device structure pointer to sensor to be tied with the thermal zone OF life cycle
 * @sensor_id: the sensor identifier
 * @data: a pointer to a private data to be stored in the thermal zone 'devdata' field
 * @ops: a pointer to the ops structure associated with the sensor
 */
struct thermal_zone_device *devm_thermal_of_zone_register(struct device *dev, int sensor_id, void *data,
							  const struct thermal_zone_device_ops *ops)
{
	struct thermal_zone_device **ptr, *tzd;

	ptr = devres_alloc(devm_thermal_of_zone_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	tzd = thermal_of_zone_register(dev->of_node, sensor_id, data, ops);
	if (IS_ERR(tzd)) {
		devres_free(ptr);
		return tzd;
	}

	*ptr = tzd;
	devres_add(dev, ptr);

	return tzd;
}
EXPORT_SYMBOL_GPL(devm_thermal_of_zone_register);

/**
 * devm_thermal_of_zone_register_with_bin - register a thermal zone with hardware binning info
 *
 * Like devm_thermal_of_zone_register(), but passes @hw_bin_info to the trip-point
 * parser so that trips can be filtered and per-bin temperatures selected at
 * zone registration time.  Use this when fuse values are read before zone
 * registration (e.g. in a platform driver's probe function).
 *
 * @dev:         device structure pointer to sensor
 * @id:          the sensor identifier
 * @data:        private data stored in the thermal zone 'devdata' field
 * @ops:         ops structure associated with the sensor
 * @hw_bin_info: hardware binning info for trip filtering (may be NULL)
 */
struct thermal_zone_device *
devm_thermal_of_zone_register_with_bin(struct device *dev, int id, void *data,
				       const struct thermal_zone_device_ops *ops,
				       const struct thermal_hw_bin_info *hw_bin_info)
{
	struct thermal_zone_device **ptr, *tzd;

	ptr = devres_alloc(devm_thermal_of_zone_release, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return ERR_PTR(-ENOMEM);

	tzd = thermal_of_zone_register_with_bin(dev->of_node, id, data,
						ops, hw_bin_info);
	if (IS_ERR(tzd)) {
		devres_free(ptr);
		return tzd;
	}

	*ptr = tzd;
	devres_add(dev, ptr);

	return tzd;
}
EXPORT_SYMBOL_GPL(devm_thermal_of_zone_register_with_bin);

/**
 * devm_thermal_of_zone_unregister - Resource managed version of
 *				thermal_of_zone_unregister().
 * @dev: Device for which resource was allocated.
 * @tz: a pointer to struct thermal_zone where the sensor is registered.
 *
 * This function removes the sensor callbacks and private data from the
 * thermal zone device registered with devm_thermal_zone_of_sensor_register()
 * API. It will also silent the zone by remove the .get_temp() and .get_trend()
 * thermal zone device callbacks.
 * Normally this function will not need to be called and the resource
 * management code will ensure that the resource is freed.
 */
void devm_thermal_of_zone_unregister(struct device *dev, struct thermal_zone_device *tz)
{
	WARN_ON(devres_release(dev, devm_thermal_of_zone_release,
			       devm_thermal_of_zone_match, tz));
}
EXPORT_SYMBOL_GPL(devm_thermal_of_zone_unregister);

/**
 * thermal_of_cooling_device_register() - register an OF thermal cooling device
 * @np:		a pointer to a device tree node.
 * @cdev_id:	a cooling device id in the cooling controller
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:	standard thermal cooling devices callbacks.
 *
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 * It also gives the opportunity to link the cooling device to a device tree
 * node, so that it can be bound to a thermal zone created out of device tree.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
thermal_of_cooling_device_register(struct device_node *np, u32 cdev_id,
				   const char *type, void *devdata,
				   const struct thermal_cooling_device_ops *ops)
{
	struct thermal_cooling_device *cdev;
	int ret;

	cdev = thermal_cooling_device_alloc(type, ops);
	if (IS_ERR(cdev))
		return cdev;

	cdev->np = np;
	cdev->cdev_id = cdev_id;

	ret = thermal_cooling_device_add(cdev, NULL, devdata);
	if (ret)
		return ERR_PTR(ret);

	return cdev;
}
EXPORT_SYMBOL_GPL(thermal_of_cooling_device_register);

static void thermal_of_cooling_device_release(void *data)
{
	struct thermal_cooling_device *cdev = data;

	thermal_cooling_device_unregister(cdev);
}

static struct thermal_cooling_device *
__devm_thermal_of_cooling_device_register(struct device *dev, struct device_node *np,
					  u32 cdev_id, const char *type, void *devdata,
					  const struct thermal_cooling_device_ops *ops)
{
	struct thermal_cooling_device *cdev;
	int ret;

	cdev = thermal_of_cooling_device_register(np, cdev_id, type, devdata, ops);
	if (IS_ERR(cdev))
		return cdev;

	ret = devm_add_action_or_reset(dev, thermal_of_cooling_device_release, cdev);
	if (ret)
		return ERR_PTR(ret);

	return cdev;
}

/**
 * devm_thermal_of_cooling_device_register() - register an OF thermal cooling device
 * @dev:	a valid struct device pointer of a sensor device.
 * @cdev_id:	a cooling device index in the cooling controller
 * @type:	the thermal cooling device type.
 * @devdata:	device private data.
 * @ops:	standard thermal cooling devices callbacks.
 *
 * This function will register a cooling device with device tree node reference.
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
devm_thermal_of_cooling_device_register(struct device *dev, u32 cdev_id,
					const char *type, void *devdata,
					const struct thermal_cooling_device_ops *ops)
{
	return __devm_thermal_of_cooling_device_register(dev, dev->of_node, cdev_id,
							 type, devdata, ops);
}
EXPORT_SYMBOL_GPL(devm_thermal_of_cooling_device_register);

/**
 * devm_thermal_of_child_cooling_device_register() - register an OF thermal cooling
 *                                             device
 * @dev:        a valid struct device pointer of a sensor device.
 * @np:         a pointer to a device tree node.
 * @type:       the thermal cooling device type.
 * @devdata:    device private data.
 * @ops:        standard thermal cooling devices callbacks.
 *
 * This function will register a cooling device with device tree node reference.
 * This interface function adds a new thermal cooling device (fan/processor/...)
 * to /sys/class/thermal/ folder as cooling_device[0-*]. It tries to bind itself
 * to all the thermal zone devices registered at the same time.
 *
 * This function should be used when a cooling controller has child
 * nodes which are referenced in the thermal zone cooling map.
 *
 * Return: a pointer to the created struct thermal_cooling_device or an
 * ERR_PTR. Caller must check return value with IS_ERR*() helpers.
 */
struct thermal_cooling_device *
devm_thermal_of_child_cooling_device_register(struct device *dev,
					      struct device_node *np,
					      const char *type, void *devdata,
					      const struct thermal_cooling_device_ops *ops)
{
	return __devm_thermal_of_cooling_device_register(dev, np, 0, type, devdata, ops);
}
EXPORT_SYMBOL_GPL(devm_thermal_of_child_cooling_device_register);
