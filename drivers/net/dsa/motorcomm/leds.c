// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 David Yang
 */

#include <linux/uleds.h>

#include "chip.h"
#include "leds.h"
#include "smi.h"

#define to_yt921x_led(led_cdev) \
	container_of_const((led_cdev), struct yt921x_led, cdev)
#define to_yt921x_port(led) \
	((void *)((led) - (led)->group) - offsetof(struct yt921x_port, leds))
#define to_yt921x_priv(pp) ((pp)->priv)
#define to_device(priv) ((priv)->ds.dev)

static u32 yt921x_led_regaddr(struct yt921x_priv *priv, int port, int group)
{
	switch (group) {
	case 0:
	default:
		return YT921X_LED0_PORTn(port);
	case 1:
		return YT921X_LED1_PORTn(port);
	case 2:
		return YT921X_LED2_PORTn(port);
	}
}

static int
yt921x_led_force_get(struct yt921x_priv *priv, int port, int group, bool *onp)
{
	u32 val;
	int res;

	res = yt921x_reg_read(priv, YT921X_LED2_PORTn(port), &val);
	if (res)
		return res;

	*onp = (val & YT921X_LED2_PORT_FORCEn_M(group)) ==
	       YT921X_LED2_PORT_FORCEn_ON(group);
	return 0;
}

static int
yt921x_led_force_set(struct yt921x_priv *priv, int port, int group, bool on)
{
	struct yt921x_port *pp = priv->ports[port];
	struct yt921x_led *led;
	u32 ctrl;
	u32 mask;

	if (!pp)
		return -ENODEV;
	led = &pp->leds[group];

	led->use_cycle = false;
	led->use_duty = false;

	mask = YT921X_LED2_PORT_FORCEn_M(group);
	ctrl = on ? YT921X_LED2_PORT_FORCEn_ON(group) :
	       YT921X_LED2_PORT_FORCEn_OFF(group);
	return yt921x_reg_update_bits(priv, YT921X_LED2_PORTn(port), mask,
				      ctrl);
}

static int
yt921x_led_blink_select(const struct yt921x_priv *priv, unsigned long on,
			unsigned long off, unsigned short *cyclep,
			unsigned short *dutyp)
{
	static const unsigned short dutys[] = {
		YT921X_LED_DUTY(1, 6),
		YT921X_LED_DUTY(1, 4),
		YT921X_LED_DUTY(1, 3),
		YT921X_LED_DUTY(1, 2),
	};
	unsigned int cycle_upper;
	unsigned int cycle_req;
	unsigned int duty_req;
	unsigned int cycle;
	unsigned int duty;

	if (!on && !off) {
		*cyclep = YT921X_LED_BLINK_DEF;
		*dutyp = YT921X_LED_DUTY(1, 2);
		return 0;
	}

	cycle = YT921X_LED_BLINK_MAX;
	cycle_upper = cycle * 11585 / 8192 + 1;  /* M_SQRT2 * cycle */
	if (check_add_overflow(on, off, &cycle_req) || cycle_req >= cycle_upper)
		return -EOPNOTSUPP;

	for (; cycle > YT921X_LED_BLINK_MIN; cycle_upper >>= 1, cycle >>= 1)
		if (cycle_req >= cycle_upper >> 1)
			break;

	duty_req = YT921X_LED_DUTY(on > off ? off : on, cycle_req);
	for (unsigned int i = ARRAY_SIZE(dutys) - 1;; i--)
		if (i <= 0 || duty_req >= (dutys[i - 1] + dutys[i]) / 2) {
			duty = dutys[i];
			break;
		}
	if (on > off)
		duty = YT921X_LED_DUTY_DENOM - duty;

	*cyclep = cycle;
	*dutyp = duty;
	return 0;
}

static int
yt921x_led_blink_set(struct yt921x_priv *priv, int port, int group,
		     unsigned long *onp, unsigned long *offp)
{
	struct yt921x_port *pp = priv->ports[port];
	struct yt921x_led *led;
	unsigned short cycle;
	unsigned short duty;
	bool change_cycle;
	bool change_duty;
	bool use_cycle;
	u32 ctrl;
	u32 mask;
	u32 val;
	int res;

	if (!pp)
		return -ENODEV;
	led = &pp->leds[group];

	res = yt921x_led_blink_select(priv, *onp, *offp, &cycle, &duty);
	if (res)
		return res;

	use_cycle = cycle < YT921X_LED_BLINK_DEF;
	change_cycle = use_cycle && cycle != pp->led_cycle;
	change_duty = duty != pp->led_duty;
	if (change_cycle || change_duty)
		for (unsigned int i = 0; i < YT921X_LED_GROUP_NUM; i++) {
			if (i == group)
				continue;
			if ((change_cycle && pp->leds[i].use_cycle) ||
			    (change_duty && pp->leds[i].use_duty))
				return -EOPNOTSUPP;
		}

	/* The chip seems to jam a while if changing duty directly */
	res = yt921x_reg_read(priv, YT921X_LED2_PORTn(port), &val);
	if (res)
		return res;

	ctrl = val & ~YT921X_LED2_PORT_FORCEn_M(group);
	ctrl |= YT921X_LED2_PORT_FORCEn_DONTCARE(group);
	if (val != ctrl) {
		res = yt921x_reg_write(priv, YT921X_LED2_PORTn(port), ctrl);
		if (res)
			return res;
	}

	mask = YT921X_LED1_PORT_BLINK_DUTY_M | YT921X_LED1_PORT_BLINK_DUTY_COMP;
	switch (duty >= YT921X_LED_DUTY(1, 2) ? duty :
		YT921X_LED_DUTY_DENOM - duty) {
	default:
		duty = YT921X_LED_DUTY(1, 2);
		fallthrough;
	case YT921X_LED_DUTY(1, 2):
		ctrl = YT921X_LED1_PORT_BLINK_DUTY_1_2;
		break;
	case YT921X_LED_DUTY(2, 3):
		ctrl = YT921X_LED1_PORT_BLINK_DUTY_2_3;
		break;
	case YT921X_LED_DUTY(3, 4):
		ctrl = YT921X_LED1_PORT_BLINK_DUTY_3_4;
		break;
	case YT921X_LED_DUTY(5, 6):
		ctrl = YT921X_LED1_PORT_BLINK_DUTY_5_6;
		break;
	}
	if (duty < YT921X_LED_DUTY(1, 2))
		ctrl |= YT921X_LED1_PORT_BLINK_DUTY_COMP;
	if (use_cycle) {
		mask |= YT921X_LED1_PORT_OTHER_BLINK_M;
		ctrl |= YT921X_LED1_PORT_OTHER_BLINK(9 - __fls(cycle));
	}
	res = yt921x_reg_update_bits(priv, YT921X_LED1_PORTn(port), mask, ctrl);
	if (res)
		return res;

	ctrl = val & ~(YT921X_LED2_PORT_FORCEn_M(group) |
		       YT921X_LED2_PORT_FORCE_BLINKn_M(group));
	ctrl |= YT921X_LED2_PORT_FORCEn_BLINK(group);
	if (use_cycle)
		ctrl |= YT921X_LED2_PORT_FORCE_BLINKn_OTHER(group);
	else
		ctrl |= YT921X_LED2_PORT_FORCE_BLINKn(group, __fls(cycle) - 9);
	res = yt921x_reg_write(priv, YT921X_LED2_PORTn(port), ctrl);
	if (res)
		return res;

	led->use_cycle = use_cycle;
	if (use_cycle)
		pp->led_cycle = cycle;
	led->use_duty = true;
	pp->led_duty = duty;

	*onp = (duty * cycle + YT921X_LED_DUTY_DENOM / 2) /
	       YT921X_LED_DUTY_DENOM;
	*offp = cycle - *onp;
	return 0;
}

static const u32 yt921x_led_trigger_maps[__TRIGGER_NETDEV_MAX] = {
	[TRIGGER_NETDEV_LINK]		= YT921X_LEDx_PORT_ACT_DUPLEX_HALF |
					  YT921X_LEDx_PORT_ACT_DUPLEX_FULL,
	[TRIGGER_NETDEV_LINK_10]	= YT921X_LEDx_PORT_ACT_10M,
	[TRIGGER_NETDEV_LINK_100]	= YT921X_LEDx_PORT_ACT_100M,
	[TRIGGER_NETDEV_LINK_1000]	= YT921X_LEDx_PORT_ACT_1000M,
	[TRIGGER_NETDEV_HALF_DUPLEX]	= YT921X_LEDx_PORT_ACT_DUPLEX_HALF,
	[TRIGGER_NETDEV_FULL_DUPLEX]	= YT921X_LEDx_PORT_ACT_DUPLEX_FULL,
	[TRIGGER_NETDEV_TX]		= YT921X_LEDx_PORT_ACT_TX,
	[TRIGGER_NETDEV_RX]		= YT921X_LEDx_PORT_ACT_RX,
};

static bool yt921x_led_trigger_is_supported(int group, unsigned long flags)
{
	unsigned int i;

	for_each_set_bit(i, &flags, __TRIGGER_NETDEV_MAX)
		if (!yt921x_led_trigger_maps[i])
			return false;

	return true;
}

static int
yt921x_led_trigger_get(struct yt921x_priv *priv, int port, int group,
		       unsigned long *flagsp)
{
	u32 addr;
	u32 val;
	int res;

	addr = yt921x_led_regaddr(priv, port, group);
	res = yt921x_reg_read(priv, addr, &val);
	if (res)
		return res;

	*flagsp = 0;
	for (unsigned int i = 0; i < __TRIGGER_NETDEV_MAX; i++)
		if (val & yt921x_led_trigger_maps[i])
			*flagsp |= BIT(i);

	return 0;
}

static int
yt921x_led_trigger_set(struct yt921x_priv *priv, int port, int group,
		       unsigned long flags)
{
	struct yt921x_port *pp = priv->ports[port];
	struct yt921x_led *led;
	unsigned int i;
	u32 addr;
	u32 ctrl;
	u32 mask;
	int res;

	if (!pp)
		return -ENODEV;
	led = &pp->leds[group];

	ctrl = 0;
	for_each_set_bit(i, &flags, __TRIGGER_NETDEV_MAX) {
		if (!yt921x_led_trigger_maps[i])
			return -EOPNOTSUPP;

		ctrl |= yt921x_led_trigger_maps[i];
	}

	led->use_cycle = false;
	led->use_duty = false;

	mask = !group ? YT921X_LED0_PORT_ACT_M : YT921X_LEDx_PORT_ACT_M;
	if (group == 2) {
		mask |= YT921X_LED2_PORT_FORCEn_M(group);
		ctrl |= YT921X_LED2_PORT_FORCEn_DONTCARE(group);
	}
	addr = yt921x_led_regaddr(priv, port, group);
	res = yt921x_reg_update_bits(priv, addr, mask, ctrl);
	if (res)
		return res;

	if (group != 2) {
		mask = YT921X_LED2_PORT_FORCEn_M(group);
		ctrl = YT921X_LED2_PORT_FORCEn_DONTCARE(group);
		res = yt921x_reg_update_bits(priv, YT921X_LED2_PORTn(port),
					     mask, ctrl);
		if (res)
			return res;
	}

	return 0;
}

static int
yt921x_cled_brightness_set_blocking(struct led_classdev *led_cdev,
				    enum led_brightness brightness)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);
	struct yt921x_port *pp = to_yt921x_port(led);
	struct yt921x_priv *priv = to_yt921x_priv(pp);
	int res;

	mutex_lock(&priv->reg_lock);
	res = yt921x_led_force_set(priv, pp->index, led->group, brightness);
	mutex_unlock(&priv->reg_lock);

	return res;
}

static int
yt921x_cled_blink_set(struct led_classdev *led_cdev, unsigned long *delay_on,
		      unsigned long *delay_off)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);
	struct yt921x_port *pp = to_yt921x_port(led);
	struct yt921x_priv *priv = to_yt921x_priv(pp);
	int res;

	mutex_lock(&priv->reg_lock);
	res = yt921x_led_blink_set(priv, pp->index, led->group, delay_on,
				   delay_off);
	mutex_unlock(&priv->reg_lock);

	return res;
}

static struct device * __maybe_unused
yt921x_cled_hw_control_get_device(struct led_classdev *led_cdev)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);
	struct yt921x_port *pp = to_yt921x_port(led);
	struct yt921x_priv *priv = to_yt921x_priv(pp);
	struct dsa_port *dp;

	dp = dsa_to_port(&priv->ds, pp->index);
	if (!dp || !dp->user)
		return NULL;
	return &dp->user->dev;
}

static int __maybe_unused
yt921x_cled_hw_control_is_supported(struct led_classdev *led_cdev,
				    unsigned long flags)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);

	return yt921x_led_trigger_is_supported(led->group, flags) ? 0 :
	       -EOPNOTSUPP;
}

static int __maybe_unused
yt921x_cled_hw_control_get(struct led_classdev *led_cdev, unsigned long *flagsp)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);
	struct yt921x_port *pp = to_yt921x_port(led);
	struct yt921x_priv *priv = to_yt921x_priv(pp);
	int res;

	mutex_lock(&priv->reg_lock);
	res = yt921x_led_trigger_get(priv, pp->index, led->group, flagsp);
	mutex_unlock(&priv->reg_lock);

	return res;
}

static int __maybe_unused
yt921x_cled_hw_control_set(struct led_classdev *led_cdev, unsigned long flags)
{
	struct yt921x_led *led = to_yt921x_led(led_cdev);
	struct yt921x_port *pp = to_yt921x_port(led);
	struct yt921x_priv *priv = to_yt921x_priv(pp);
	int res;

	mutex_lock(&priv->reg_lock);
	res = yt921x_led_trigger_set(priv, pp->index, led->group, flags);
	mutex_unlock(&priv->reg_lock);

	return res;
}

static int
yt921x_led_setup(struct yt921x_priv *priv, int port,
		 struct fwnode_handle *fwnode, u32 *invp)
{
	struct yt921x_port *pp = priv->ports[port];
	struct device *dev = to_device(priv);
	struct led_init_data init_data;
	struct led_classdev *led_cdev;
	char name[LED_MAX_NAME_SIZE];
	enum led_default_state state;
	struct yt921x_led *led;
	u32 group;
	bool on;
	int res;

	if (!pp)
		return -ENODEV;
	if (port == YT921X_PORT_MCU) {
		dev_err(dev, "No LEDs for port %d\n", port);
		return -ENODEV;
	}

	res = fwnode_property_read_u32(fwnode, "reg", &group);
	if (res)
		return res;
	if (group >= YT921X_LED_GROUP_NUM) {
		dev_err(dev, "Invalid LED reg %u for port %d\n", group, port);
		return -EINVAL;
	}

	led = &pp->leds[group];
	led_cdev = &led->cdev;
	state = led_init_default_state_get(fwnode);
	switch (state) {
	case LEDS_DEFSTATE_OFF:
	case LEDS_DEFSTATE_ON:
		on = state != LEDS_DEFSTATE_OFF;
		res = yt921x_led_force_set(priv, port, group, on);
		break;
	case LEDS_DEFSTATE_KEEP:
		res = yt921x_led_force_get(priv, port, group, &on);
		break;
	}
	if (res)
		return res;
	led_cdev->brightness = on;
	led_cdev->max_brightness = 1;
	led_cdev->flags = LED_RETAIN_AT_SHUTDOWN;
	led_cdev->brightness_set_blocking = yt921x_cled_brightness_set_blocking;
	led_cdev->blink_set = yt921x_cled_blink_set;
#ifdef CONFIG_LEDS_TRIGGERS
	led_cdev->hw_control_trigger = "netdev";
	led_cdev->hw_control_get_device = yt921x_cled_hw_control_get_device;
	led_cdev->hw_control_is_supported = yt921x_cled_hw_control_is_supported;
	led_cdev->hw_control_get = yt921x_cled_hw_control_get;
	led_cdev->hw_control_set = yt921x_cled_hw_control_set;
#endif

	snprintf(name, sizeof(name), YT921X_NAME "-%u:%02d:%02u",
		 priv->ds.index, port, group);
	init_data = (typeof(init_data)){
		.fwnode = fwnode,
		.devicename = name,
		.devname_mandatory = true,
	};
	res = devm_led_classdev_register_ext(dev, led_cdev, &init_data);
	if (res)
		return res;

	if (fwnode_property_read_bool(fwnode, "active-high"))
		*invp |= YT921X_LED_PAR_INV_INVnm(group, port);

	return 0;
}

int yt921x_leds_setup(struct yt921x_priv *priv)
{
	struct dsa_switch *ds = &priv->ds;
	struct dsa_port *dp;
	u32 ctrl;
	u32 mask;
	u32 inv;
	int res;

	mask = YT921X_LED_CTRL_MODE_M | YT921X_LED_CTRL_PORT_NUM_M |
	       YT921X_LED_CTRL_EN;
	ctrl = YT921X_LED_CTRL_MODE_PARALLEL |
	       YT921X_LED_CTRL_PORT_NUM(YT921X_PORT_NUM - 1) |
	       YT921X_LED_CTRL_EN;
	res = yt921x_reg_update_bits(priv, YT921X_LED_CTRL, mask, ctrl);
	if (res)
		return res;

	for (int port = 0; port < YT921X_PORT_NUM; port++) {
		struct yt921x_port *pp = priv->ports[port];

		if (!pp)
			continue;

		for (int group = 0; group < YT921X_LED_GROUP_NUM; group++)
			pp->leds[group].group = group;
	}

	inv = 0;
	dsa_switch_for_each_port(dp, ds) {
		struct device_node *leds_np;

		if (!dp->dn)
			continue;

		leds_np = of_get_child_by_name(dp->dn, "leds");
		if (!leds_np)
			continue;

		for_each_child_of_node_scoped(leds_np, led_np) {
			res = yt921x_led_setup(priv, dp->index,
					       of_fwnode_handle(led_np), &inv);
			if (res)
				break;
		}

		of_node_put(leds_np);
		if (res)
			return res;
	}

	/* Inversion is internal - FORCEn_HIGH will give low logic.
	 * In the rest of the file, treat LEDs as if active-low.
	 */
	res = yt921x_reg_write(priv, YT921X_LED_PAR_INV, inv);
	if (res)
		return res;

	return 0;
}
