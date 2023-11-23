// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Intel Corporation.
 * Author: Michal Michalik <michal.michalik@intel.com>
 */
#include "netdevsim.h"

#define EEC_DPLL_DEV 0
#define EEC_DPLL_TEMPERATURE 20
#define PPS_DPLL_DEV 1
#define PPS_DPLL_TEMPERATURE 30

#define PIN_GNSS 0
#define PIN_GNSS_CAPABILITIES DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE
#define PIN_GNSS_PRIORITY 5

#define PIN_PPS 1
#define PIN_PPS_CAPABILITIES                          \
	(DPLL_PIN_CAPABILITIES_DIRECTION_CAN_CHANGE | \
	 DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE |  \
	 DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE)
#define PIN_PPS_PRIORITY 6

#define PIN_RCLK 2
#define PIN_RCLK_CAPABILITIES                        \
	(DPLL_PIN_CAPABILITIES_PRIORITY_CAN_CHANGE | \
	 DPLL_PIN_CAPABILITIES_STATE_CAN_CHANGE)
#define PIN_RCLK_PRIORITY 7

#define EEC_PINS_NUMBER 3
#define PPS_PINS_NUMBER 2

static int nsim_fill_pin_properties(struct dpll_pin_properties *pp,
				    const char *label, enum dpll_pin_type type,
				    unsigned long caps, u32 freq_supp_num,
				    u64 fmin, u64 fmax)
{
	struct dpll_pin_frequency *freq_supp;

	freq_supp = kzalloc(sizeof(*freq_supp), GFP_KERNEL);
	if (!freq_supp)
		goto freq_supp;
	freq_supp->min = fmin;
	freq_supp->max = fmax;

	pp->board_label = kasprintf(GFP_KERNEL, "%s_brd", label);
	if (!pp->board_label)
		goto board_label;
	pp->panel_label = kasprintf(GFP_KERNEL, "%s_pnl", label);
	if (!pp->panel_label)
		goto panel_label;
	pp->package_label = kasprintf(GFP_KERNEL, "%s_pcg", label);
	if (!pp->package_label)
		goto package_label;
	pp->freq_supported_num = freq_supp_num;
	pp->freq_supported = freq_supp;
	pp->capabilities = caps;
	pp->type = type;

	return 0;

package_label:
	kfree(pp->panel_label);
panel_label:
	kfree(pp->board_label);
board_label:
	kfree(freq_supp);
freq_supp:
	return -ENOMEM;
}

static void nsim_fill_pin_pd(struct nsim_pin_priv_data *pd, u64 frequency,
			     u32 prio, enum dpll_pin_direction direction)
{
	pd->state_dpll = DPLL_PIN_STATE_DISCONNECTED;
	pd->state_pin = DPLL_PIN_STATE_DISCONNECTED;
	pd->frequency = frequency;
	pd->direction = direction;
	pd->prio = prio;
}

static int nsim_dds_ops_mode_get(const struct dpll_device *dpll,
				 void *dpll_priv, enum dpll_mode *mode,
				 struct netlink_ext_ack *extack)
{
	struct nsim_dpll_priv_data *pd = dpll_priv;
	*mode = pd->mode;
	return 0;
};

static bool nsim_dds_ops_mode_supported(const struct dpll_device *dpll,
					void *dpll_priv,
					const enum dpll_mode mode,
					struct netlink_ext_ack *extack)
{
	return true;
};

static int nsim_dds_ops_lock_status_get(const struct dpll_device *dpll,
					void *dpll_priv,
					enum dpll_lock_status *status,
					struct netlink_ext_ack *extack)
{
	struct nsim_dpll_priv_data *pd = dpll_priv;

	*status = pd->status;
	return 0;
};

static int nsim_dds_ops_temp_get(const struct dpll_device *dpll,
				 void *dpll_priv, s32 *temp,
				 struct netlink_ext_ack *extack)
{
	struct nsim_dpll_priv_data *pd = dpll_priv;

	*temp = pd->temperature;
	return 0;
};

static int nsim_pin_frequency_set(const struct dpll_pin *pin, void *pin_priv,
				  const struct dpll_device *dpll,
				  void *dpll_priv, const u64 frequency,
				  struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	pd->frequency = frequency;
	return 0;
};

static int nsim_pin_frequency_get(const struct dpll_pin *pin, void *pin_priv,
				  const struct dpll_device *dpll,
				  void *dpll_priv, u64 *frequency,
				  struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	*frequency = pd->frequency;
	return 0;
};

static int nsim_pin_direction_set(const struct dpll_pin *pin, void *pin_priv,
				  const struct dpll_device *dpll,
				  void *dpll_priv,
				  const enum dpll_pin_direction direction,
				  struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	pd->direction = direction;
	return 0;
};

static int nsim_pin_direction_get(const struct dpll_pin *pin, void *pin_priv,
				  const struct dpll_device *dpll,
				  void *dpll_priv,
				  enum dpll_pin_direction *direction,
				  struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	*direction = pd->direction;
	return 0;
};

static int nsim_pin_state_on_pin_get(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_pin *parent_pin,
				     void *parent_priv,
				     enum dpll_pin_state *state,
				     struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	*state = pd->state_pin;
	return 0;
};

static int nsim_pin_state_on_dpll_get(const struct dpll_pin *pin,
				      void *pin_priv,
				      const struct dpll_device *dpll,
				      void *dpll_priv,
				      enum dpll_pin_state *state,
				      struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	*state = pd->state_dpll;
	return 0;
};

static int nsim_pin_state_on_pin_set(const struct dpll_pin *pin, void *pin_priv,
				     const struct dpll_pin *parent_pin,
				     void *parent_priv,
				     const enum dpll_pin_state state,
				     struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	pd->state_pin = state;
	return 0;
};

static int nsim_pin_state_on_dpll_set(const struct dpll_pin *pin,
				      void *pin_priv,
				      const struct dpll_device *dpll,
				      void *dpll_priv,
				      const enum dpll_pin_state state,
				      struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	pd->state_dpll = state;
	return 0;
};

static int nsim_pin_prio_get(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     u32 *prio, struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	*prio = pd->prio;
	return 0;
};

static int nsim_pin_prio_set(const struct dpll_pin *pin, void *pin_priv,
			     const struct dpll_device *dpll, void *dpll_priv,
			     const u32 prio, struct netlink_ext_ack *extack)
{
	struct nsim_pin_priv_data *pd = pin_priv;

	pd->prio = prio;
	return 0;
};

static void nsim_free_pin_properties(struct dpll_pin_properties *pp)
{
	kfree(pp->board_label);
	kfree(pp->panel_label);
	kfree(pp->package_label);
	kfree(pp->freq_supported);
}

static struct dpll_device_ops nsim_dds_ops = {
	.mode_get = nsim_dds_ops_mode_get,
	.mode_supported = nsim_dds_ops_mode_supported,
	.lock_status_get = nsim_dds_ops_lock_status_get,
	.temp_get = nsim_dds_ops_temp_get,
};

static struct dpll_pin_ops nsim_pin_ops = {
	.frequency_set = nsim_pin_frequency_set,
	.frequency_get = nsim_pin_frequency_get,
	.direction_set = nsim_pin_direction_set,
	.direction_get = nsim_pin_direction_get,
	.state_on_pin_get = nsim_pin_state_on_pin_get,
	.state_on_dpll_get = nsim_pin_state_on_dpll_get,
	.state_on_pin_set = nsim_pin_state_on_pin_set,
	.state_on_dpll_set = nsim_pin_state_on_dpll_set,
	.prio_get = nsim_pin_prio_get,
	.prio_set = nsim_pin_prio_set,
};

int nsim_dpll_init_owner(struct nsim_dpll *dpll, unsigned int ports_count)
{
	u64 clock_id;
	int err;

	get_random_bytes(&clock_id, sizeof(clock_id));

	/* Create EEC DPLL */
	dpll->dpll_e = dpll_device_get(clock_id, EEC_DPLL_DEV, THIS_MODULE);
	if (IS_ERR(dpll->dpll_e))
		return -EFAULT;

	dpll->dpll_e_pd.temperature = EEC_DPLL_TEMPERATURE;
	dpll->dpll_e_pd.mode = DPLL_MODE_AUTOMATIC;
	dpll->dpll_e_pd.clock_id = clock_id;
	dpll->dpll_e_pd.status = DPLL_LOCK_STATUS_UNLOCKED;

	err = dpll_device_register(dpll->dpll_e, DPLL_TYPE_EEC, &nsim_dds_ops,
				   &dpll->dpll_e_pd);
	if (err)
		goto e_reg;

	/* Create PPS DPLL */
	dpll->dpll_p = dpll_device_get(clock_id, PPS_DPLL_DEV, THIS_MODULE);
	if (IS_ERR(dpll->dpll_p))
		goto dpll_p;

	dpll->dpll_p_pd.temperature = PPS_DPLL_TEMPERATURE;
	dpll->dpll_p_pd.mode = DPLL_MODE_MANUAL;
	dpll->dpll_p_pd.clock_id = clock_id;
	dpll->dpll_p_pd.status = DPLL_LOCK_STATUS_UNLOCKED;

	err = dpll_device_register(dpll->dpll_p, DPLL_TYPE_PPS, &nsim_dds_ops,
				   &dpll->dpll_p_pd);
	if (err)
		goto p_reg;

	/* Create first pin (GNSS) */
	err = nsim_fill_pin_properties(&dpll->pp_gnss, "GNSS",
				       DPLL_PIN_TYPE_GNSS,
				       PIN_GNSS_CAPABILITIES, 1,
				       DPLL_PIN_FREQUENCY_1_HZ,
				       DPLL_PIN_FREQUENCY_1_HZ);
	if (err)
		goto pp_gnss;
	dpll->p_gnss =
		dpll_pin_get(clock_id, PIN_GNSS, THIS_MODULE, &dpll->pp_gnss);
	if (IS_ERR(dpll->p_gnss))
		goto p_gnss;
	nsim_fill_pin_pd(&dpll->p_gnss_pd, DPLL_PIN_FREQUENCY_1_HZ,
			 PIN_GNSS_PRIORITY, DPLL_PIN_DIRECTION_INPUT);
	err = dpll_pin_register(dpll->dpll_e, dpll->p_gnss, &nsim_pin_ops,
				&dpll->p_gnss_pd);
	if (err)
		goto e_gnss_reg;

	/* Create second pin (PPS) */
	err = nsim_fill_pin_properties(&dpll->pp_pps, "PPS", DPLL_PIN_TYPE_EXT,
				       PIN_PPS_CAPABILITIES, 1,
				       DPLL_PIN_FREQUENCY_1_HZ,
				       DPLL_PIN_FREQUENCY_1_HZ);
	if (err)
		goto pp_pps;
	dpll->p_pps =
		dpll_pin_get(clock_id, PIN_PPS, THIS_MODULE, &dpll->pp_pps);
	if (IS_ERR(dpll->p_pps)) {
		err = -EFAULT;
		goto p_pps;
	}
	nsim_fill_pin_pd(&dpll->p_pps_pd, DPLL_PIN_FREQUENCY_1_HZ,
			 PIN_PPS_PRIORITY, DPLL_PIN_DIRECTION_INPUT);
	err = dpll_pin_register(dpll->dpll_e, dpll->p_pps, &nsim_pin_ops,
				&dpll->p_pps_pd);
	if (err)
		goto e_pps_reg;
	err = dpll_pin_register(dpll->dpll_p, dpll->p_pps, &nsim_pin_ops,
				&dpll->p_pps_pd);
	if (err)
		goto p_pps_reg;

	dpll->pp_rclk =
		kcalloc(ports_count, sizeof(*dpll->pp_rclk), GFP_KERNEL);
	dpll->p_rclk = kcalloc(ports_count, sizeof(*dpll->p_rclk), GFP_KERNEL);
	dpll->p_rclk_pd =
		kcalloc(ports_count, sizeof(*dpll->p_rclk_pd), GFP_KERNEL);

	return 0;

p_pps_reg:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_pps, &nsim_pin_ops,
			    &dpll->p_pps_pd);
e_pps_reg:
	dpll_pin_put(dpll->p_pps);
p_pps:
	nsim_free_pin_properties(&dpll->pp_pps);
pp_pps:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_gnss, &nsim_pin_ops,
			    &dpll->p_gnss_pd);
e_gnss_reg:
	dpll_pin_put(dpll->p_gnss);
p_gnss:
	nsim_free_pin_properties(&dpll->pp_gnss);
pp_gnss:
	dpll_device_unregister(dpll->dpll_p, &nsim_dds_ops, &dpll->dpll_p_pd);
p_reg:
	dpll_device_put(dpll->dpll_p);
dpll_p:
	dpll_device_unregister(dpll->dpll_e, &nsim_dds_ops, &dpll->dpll_e_pd);
e_reg:
	dpll_device_put(dpll->dpll_e);
	return err;
}

void nsim_dpll_free_owner(struct nsim_dpll *dpll)
{
	/* Free GNSS pin */
	dpll_pin_unregister(dpll->dpll_e, dpll->p_gnss, &nsim_pin_ops,
			    &dpll->p_gnss_pd);
	dpll_pin_put(dpll->p_gnss);
	nsim_free_pin_properties(&dpll->pp_gnss);

	/* Free PPS pin */
	dpll_pin_unregister(dpll->dpll_e, dpll->p_pps, &nsim_pin_ops,
			    &dpll->p_pps_pd);
	dpll_pin_unregister(dpll->dpll_p, dpll->p_pps, &nsim_pin_ops,
			    &dpll->p_pps_pd);
	dpll_pin_put(dpll->p_pps);
	nsim_free_pin_properties(&dpll->pp_pps);

	/* Free DPLL EEC */
	dpll_device_unregister(dpll->dpll_e, &nsim_dds_ops, &dpll->dpll_e_pd);
	dpll_device_put(dpll->dpll_e);

	/* Free DPLL PPS */
	dpll_device_unregister(dpll->dpll_p, &nsim_dds_ops, &dpll->dpll_p_pd);
	dpll_device_put(dpll->dpll_p);

	kfree(dpll->pp_rclk);
	kfree(dpll->p_rclk);
	kfree(dpll->p_rclk_pd);
}

int nsim_rclk_init(struct netdevsim *ns)
{
	struct nsim_dpll *dpll;
	unsigned int index;
	char *name;
	int err;

	index = ns->nsim_dev_port->port_index;
	dpll = &ns->nsim_dev->dpll;
	err = -ENOMEM;

	name = kasprintf(GFP_KERNEL, "RCLK_%i", index);
	if (!name)
		goto err;

	/* Get EEC DPLL */
	if (IS_ERR(dpll->dpll_e))
		goto dpll;

	/* Get PPS DPLL */
	if (IS_ERR(dpll->dpll_p))
		goto dpll;

	/* Create Recovered clock pin (RCLK) */
	nsim_fill_pin_properties(&dpll->pp_rclk[index], name,
				 DPLL_PIN_TYPE_SYNCE_ETH_PORT,
				 PIN_RCLK_CAPABILITIES, 1, 1e6, 125e6);
	dpll->p_rclk[index] = dpll_pin_get(dpll->dpll_e_pd.clock_id,
					   PIN_RCLK + index, THIS_MODULE,
					   &dpll->pp_rclk[index]);
	if (IS_ERR(dpll->p_rclk[index]))
		goto p_rclk;
	nsim_fill_pin_pd(&dpll->p_rclk_pd[index], DPLL_PIN_FREQUENCY_10_MHZ,
			 PIN_RCLK_PRIORITY, DPLL_PIN_DIRECTION_INPUT);
	err = dpll_pin_register(dpll->dpll_e, dpll->p_rclk[index],
				&nsim_pin_ops, &dpll->p_rclk_pd[index]);
	if (err)
		goto dpll_e_reg;
	err = dpll_pin_register(dpll->dpll_p, dpll->p_rclk[index],
				&nsim_pin_ops, &dpll->p_rclk_pd[index]);
	if (err)
		goto dpll_p_reg;

	netdev_dpll_pin_set(ns->netdev, dpll->p_rclk[index]);

	kfree(name);
	return 0;

dpll_p_reg:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_rclk[index], &nsim_pin_ops,
			    &dpll->p_rclk_pd[index]);
dpll_e_reg:
	dpll_pin_put(dpll->p_rclk[index]);
p_rclk:
	nsim_free_pin_properties(&dpll->pp_rclk[index]);
dpll:
	kfree(name);
err:
	return err;
}

void nsim_rclk_free(struct netdevsim *ns)
{
	struct nsim_dpll *dpll;
	unsigned int index;

	index = ns->nsim_dev_port->port_index;
	dpll = &ns->nsim_dev->dpll;

	if (IS_ERR(dpll->dpll_e))
		return;

	if (IS_ERR(dpll->dpll_p))
		return;

	/* Free RCLK pin */
	netdev_dpll_pin_clear(ns->netdev);
	dpll_pin_unregister(dpll->dpll_e, dpll->p_rclk[index], &nsim_pin_ops,
			    &dpll->p_rclk_pd[index]);
	dpll_pin_unregister(dpll->dpll_p, dpll->p_rclk[index], &nsim_pin_ops,
			    &dpll->p_rclk_pd[index]);
	dpll_pin_put(dpll->p_rclk[index]);
	nsim_free_pin_properties(&dpll->pp_rclk[index]);
}
