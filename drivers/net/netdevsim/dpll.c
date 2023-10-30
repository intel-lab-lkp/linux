// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023, Intel Corporation.
 * Author: Michal Michalik <michal.michalik@intel.com>
 */
#include "dpll.h"

static struct dpll_pin_properties *
create_pin_properties(const char *label, enum dpll_pin_type type,
		      unsigned long caps, u32 freq_supp_num, u64 fmin, u64 fmax)
{
	struct dpll_pin_frequency *freq_supp;
	struct dpll_pin_properties *pp;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp)
		return ERR_PTR(-ENOMEM);

	freq_supp = kzalloc(sizeof(*freq_supp), GFP_KERNEL);
	if (!freq_supp)
		goto err;
	*freq_supp =
		(struct dpll_pin_frequency)DPLL_PIN_FREQUENCY_RANGE(fmin, fmax);

	pp->board_label = kasprintf(GFP_KERNEL, "%s_brd", label);
	pp->panel_label = kasprintf(GFP_KERNEL, "%s_pnl", label);
	pp->package_label = kasprintf(GFP_KERNEL, "%s_pcg", label);
	pp->freq_supported_num = freq_supp_num;
	pp->freq_supported = freq_supp;
	pp->capabilities = caps;
	pp->type = type;

	return pp;
err:
	kfree(pp);
	return ERR_PTR(-ENOMEM);
}

static struct dpll_pd *create_dpll_pd(int temperature, enum dpll_mode mode)
{
	struct dpll_pd *pd;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->temperature = temperature;
	pd->mode = mode;

	return pd;
}

static struct pin_pd *create_pin_pd(u64 frequency, u32 prio,
				    enum dpll_pin_direction direction)
{
	struct pin_pd *pd;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->state_dpll = DPLL_PIN_STATE_DISCONNECTED;
	pd->state_pin = DPLL_PIN_STATE_DISCONNECTED;
	pd->frequency = frequency;
	pd->direction = direction;
	pd->prio = prio;

	return pd;
}

static int
dds_ops_mode_get(const struct dpll_device *dpll, void *dpll_priv,
		 enum dpll_mode *mode, struct netlink_ext_ack *extack)
{
	*mode = ((struct dpll_pd *)(dpll_priv))->mode;
	return 0;
};

static bool
dds_ops_mode_supported(const struct dpll_device *dpll, void *dpll_priv,
		       const enum dpll_mode mode,
		       struct netlink_ext_ack *extack)
{
	return true;
};

static int
dds_ops_lock_status_get(const struct dpll_device *dpll, void *dpll_priv,
			enum dpll_lock_status *status,
			struct netlink_ext_ack *extack)
{
	if (((struct dpll_pd *)dpll_priv)->mode == DPLL_MODE_MANUAL)
		*status = DPLL_LOCK_STATUS_LOCKED;
	else
		*status = DPLL_LOCK_STATUS_UNLOCKED;
	return 0;
};

static int
dds_ops_temp_get(const struct dpll_device *dpll, void *dpll_priv, s32 *temp,
		 struct netlink_ext_ack *extack)
{
	*temp = ((struct dpll_pd *)dpll_priv)->temperature;
	return 0;
};

static int
pin_frequency_set(const struct dpll_pin *pin, void *pin_priv,
		  const struct dpll_device *dpll, void *dpll_priv,
		  const u64 frequency, struct netlink_ext_ack *extack)
{
	((struct pin_pd *)pin_priv)->frequency = frequency;
	return 0;
};

static int
pin_frequency_get(const struct dpll_pin *pin, void *pin_priv,
		  const struct dpll_device *dpll, void *dpll_priv,
		  u64 *frequency, struct netlink_ext_ack *extack)
{
	*frequency = ((struct pin_pd *)pin_priv)->frequency;
	return 0;
};

static int
pin_direction_set(const struct dpll_pin *pin, void *pin_priv,
		  const struct dpll_device *dpll, void *dpll_priv,
		  const enum dpll_pin_direction direction,
		  struct netlink_ext_ack *extack)
{
	((struct pin_pd *)pin_priv)->direction = direction;
	return 0;
};

static int
pin_direction_get(const struct dpll_pin *pin, void *pin_priv,
		  const struct dpll_device *dpll, void *dpll_priv,
		  enum dpll_pin_direction *direction,
		  struct netlink_ext_ack *extack)
{
	*direction = ((struct pin_pd *)pin_priv)->direction;
	return 0;
};

static int
pin_state_on_pin_get(const struct dpll_pin *pin, void *pin_priv,
		     const struct dpll_pin *parent_pin, void *parent_priv,
		     enum dpll_pin_state *state,
		     struct netlink_ext_ack *extack)
{
	*state = ((struct pin_pd *)pin_priv)->state_pin;
	return 0;
};

static int
pin_state_on_dpll_get(const struct dpll_pin *pin, void *pin_priv,
		      const struct dpll_device *dpll, void *dpll_priv,
		      enum dpll_pin_state *state,
		      struct netlink_ext_ack *extack)
{
	*state = ((struct pin_pd *)pin_priv)->state_dpll;
	return 0;
};

static int
pin_state_on_pin_set(const struct dpll_pin *pin, void *pin_priv,
		     const struct dpll_pin *parent_pin, void *parent_priv,
		     const enum dpll_pin_state state,
		     struct netlink_ext_ack *extack)
{
	((struct pin_pd *)pin_priv)->state_pin = state;
	return 0;
};

static int
pin_state_on_dpll_set(const struct dpll_pin *pin, void *pin_priv,
		      const struct dpll_device *dpll, void *dpll_priv,
		      const enum dpll_pin_state state,
		      struct netlink_ext_ack *extack)
{
	((struct pin_pd *)pin_priv)->state_dpll = state;
	return 0;
};

static int
pin_prio_get(const struct dpll_pin *pin, void *pin_priv,
	     const struct dpll_device *dpll, void *dpll_priv,
	     u32 *prio, struct netlink_ext_ack *extack)
{
	*prio = ((struct pin_pd *)pin_priv)->prio;
	return 0;
};

static int
pin_prio_set(const struct dpll_pin *pin, void *pin_priv,
	     const struct dpll_device *dpll, void *dpll_priv,
	     const u32 prio, struct netlink_ext_ack *extack)
{
	((struct pin_pd *)pin_priv)->prio = prio;
	return 0;
};

static void
free_pin_properties(struct dpll_pin_properties *pp)
{
	if (pp) {
		kfree(pp->board_label);
		kfree(pp->panel_label);
		kfree(pp->package_label);
		kfree(pp->freq_supported);
		kfree(pp);
	}
}

static struct dpll_device_ops dds_ops = {
	.mode_get = dds_ops_mode_get,
	.mode_supported = dds_ops_mode_supported,
	.lock_status_get = dds_ops_lock_status_get,
	.temp_get = dds_ops_temp_get,
};

static struct dpll_pin_ops pin_ops = {
	.frequency_set = pin_frequency_set,
	.frequency_get = pin_frequency_get,
	.direction_set = pin_direction_set,
	.direction_get = pin_direction_get,
	.state_on_pin_get = pin_state_on_pin_get,
	.state_on_dpll_get = pin_state_on_dpll_get,
	.state_on_pin_set = pin_state_on_pin_set,
	.state_on_dpll_set = pin_state_on_dpll_set,
	.prio_get = pin_prio_get,
	.prio_set = pin_prio_set,
};

int nsim_dpll_init_owner(struct nsim_dpll_info *dpll, int devid)
{
	/* Create EEC DPLL */
	dpll->dpll_e = dpll_device_get(DPLLS_CLOCK_ID + devid, EEC_DPLL_DEV,
				       THIS_MODULE);
	if (IS_ERR(dpll->dpll_e))
		goto dpll_e;
	dpll->dpll_e_pd = create_dpll_pd(EEC_DPLL_TEMPERATURE,
					 DPLL_MODE_AUTOMATIC);
	if (IS_ERR(dpll->dpll_e))
		goto dpll_e_pd;
	if (dpll_device_register(dpll->dpll_e, DPLL_TYPE_EEC, &dds_ops,
				 (void *)dpll->dpll_e_pd))
		goto e_reg;

	/* Create PPS DPLL */
	dpll->dpll_p = dpll_device_get(DPLLS_CLOCK_ID + devid, PPS_DPLL_DEV,
				       THIS_MODULE);
	if (IS_ERR(dpll->dpll_p))
		goto dpll_p;
	dpll->dpll_p_pd = create_dpll_pd(PPS_DPLL_TEMPERATURE,
					 DPLL_MODE_MANUAL);
	if (IS_ERR(dpll->dpll_p_pd))
		goto dpll_p_pd;
	if (dpll_device_register(dpll->dpll_p, DPLL_TYPE_PPS, &dds_ops,
				 (void *)dpll->dpll_p_pd))
		goto p_reg;

	/* Create first pin (GNSS) */
	dpll->pp_gnss = create_pin_properties("GNSS", DPLL_PIN_TYPE_GNSS,
					      PIN_GNSS_CAPABILITIES,
					      1, DPLL_PIN_FREQUENCY_1_HZ,
					      DPLL_PIN_FREQUENCY_1_HZ);
	if (IS_ERR(dpll->pp_gnss))
		goto pp_gnss;
	dpll->p_gnss = dpll_pin_get(DPLLS_CLOCK_ID + devid, PIN_GNSS,
				    THIS_MODULE,
				    dpll->pp_gnss);
	if (IS_ERR(dpll->p_gnss))
		goto p_gnss;
	dpll->p_gnss_pd = create_pin_pd(DPLL_PIN_FREQUENCY_1_HZ,
					PIN_GNSS_PRIORITY,
					DPLL_PIN_DIRECTION_INPUT);
	if (IS_ERR(dpll->p_gnss_pd))
		goto p_gnss_pd;
	if (dpll_pin_register(dpll->dpll_e, dpll->p_gnss, &pin_ops,
			      (void *)dpll->p_gnss_pd))
		goto e_gnss_reg;

	/* Create second pin (PPS) */
	dpll->pp_pps = create_pin_properties("PPS", DPLL_PIN_TYPE_EXT,
					     PIN_PPS_CAPABILITIES,
					     1, DPLL_PIN_FREQUENCY_1_HZ,
					     DPLL_PIN_FREQUENCY_1_HZ);
	if (IS_ERR(dpll->pp_pps))
		goto pp_pps;
	dpll->p_pps = dpll_pin_get(DPLLS_CLOCK_ID + devid, PIN_PPS, THIS_MODULE,
				   dpll->pp_pps);
	if (IS_ERR(dpll->p_pps))
		goto p_pps;
	dpll->p_pps_pd = create_pin_pd(DPLL_PIN_FREQUENCY_1_HZ,
				       PIN_PPS_PRIORITY,
				       DPLL_PIN_DIRECTION_INPUT);
	if (IS_ERR(dpll->p_pps_pd))
		goto p_pps_pd;
	if (dpll_pin_register(dpll->dpll_e, dpll->p_pps, &pin_ops,
			      (void *)dpll->p_pps_pd))
		goto e_pps_reg;
	if (dpll_pin_register(dpll->dpll_p, dpll->p_pps, &pin_ops,
			      (void *)dpll->p_pps_pd))
		goto p_pps_reg;

	return 0;

p_pps_reg:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_pps, &pin_ops,
			    (void *)dpll->p_pps_pd);
e_pps_reg:
	kfree(dpll->p_pps_pd);
p_pps_pd:
	dpll_pin_put(dpll->p_pps);
p_pps:
	free_pin_properties(dpll->pp_pps);
pp_pps:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_gnss, &pin_ops,
			    (void *)dpll->p_gnss_pd);
e_gnss_reg:
	kfree(dpll->p_gnss_pd);
p_gnss_pd:
	dpll_pin_put(dpll->p_gnss);
p_gnss:
	free_pin_properties(dpll->pp_gnss);
pp_gnss:
	dpll_device_unregister(dpll->dpll_p, &dds_ops, (void *)dpll->dpll_p_pd);
p_reg:
	kfree(dpll->dpll_p_pd);
dpll_p_pd:
	dpll_device_put(dpll->dpll_p);
dpll_p:
	dpll_device_unregister(dpll->dpll_e, &dds_ops, (void *)dpll->dpll_e_pd);
e_reg:
	kfree(dpll->dpll_e_pd);
dpll_e_pd:
	dpll_device_put(dpll->dpll_e);
dpll_e:
	return -1;
}

void nsim_dpll_free_owner(struct nsim_dpll_info *dpll)
{
	/* Free GNSS pin */
	dpll_pin_unregister(dpll->dpll_e, dpll->p_gnss, &pin_ops,
			    (void *)dpll->p_gnss_pd);
	dpll_pin_put(dpll->p_gnss);
	free_pin_properties(dpll->pp_gnss);
	kfree(dpll->p_gnss_pd);

	/* Free PPS pin */
	dpll_pin_unregister(dpll->dpll_e, dpll->p_pps, &pin_ops,
			    (void *)dpll->p_pps_pd);
	dpll_pin_unregister(dpll->dpll_p, dpll->p_pps, &pin_ops,
			    (void *)dpll->p_pps_pd);
	dpll_pin_put(dpll->p_pps);
	free_pin_properties(dpll->pp_pps);
	kfree(dpll->p_pps_pd);

	/* Free DPLL EEC */
	dpll_device_unregister(dpll->dpll_e, &dds_ops, (void *)dpll->dpll_e_pd);
	dpll_device_put(dpll->dpll_e);
	kfree(dpll->dpll_e_pd);

	/* Free DPLL PPS */
	dpll_device_unregister(dpll->dpll_p, &dds_ops, (void *)dpll->dpll_p_pd);
	dpll_device_put(dpll->dpll_p);
	kfree(dpll->dpll_p_pd);
}

int nsim_rclk_init(struct nsim_dpll_info *dpll, int devid, unsigned int index)
{
	char *name = kasprintf(GFP_KERNEL, "RCLK_%i", index);

	/* Get EEC DPLL */
	dpll->dpll_e = dpll_device_get(DPLLS_CLOCK_ID + devid, EEC_DPLL_DEV,
				       THIS_MODULE);
	if (IS_ERR(dpll->dpll_e))
		goto dpll;

	/* Get PPS DPLL */
	dpll->dpll_p = dpll_device_get(DPLLS_CLOCK_ID + devid, PPS_DPLL_DEV,
				       THIS_MODULE);
	if (IS_ERR(dpll->dpll_p))
		goto dpll;

	/* Create Recovered clock pin (RCLK) */
	dpll->pp_rclk = create_pin_properties(name,
					      DPLL_PIN_TYPE_SYNCE_ETH_PORT,
					      PIN_RCLK_CAPABILITIES, 1, 1e6,
					      125e6);
	if (IS_ERR(dpll->pp_rclk))
		goto dpll;
	dpll->p_rclk = dpll_pin_get(DPLLS_CLOCK_ID + devid, PIN_RCLK + index,
				    THIS_MODULE, dpll->pp_rclk);
	if (IS_ERR(dpll->p_rclk))
		goto p_rclk;
	dpll->p_rclk_pd = create_pin_pd(DPLL_PIN_FREQUENCY_10_MHZ,
					PIN_RCLK_PRIORITY,
					DPLL_PIN_DIRECTION_INPUT);
	if (IS_ERR(dpll->p_rclk_pd))
		goto p_rclk_pd;
	if (dpll_pin_register(dpll->dpll_e, dpll->p_rclk, &pin_ops,
			      (void *)dpll->p_rclk_pd))
		goto dpll_e_reg;
	if (dpll_pin_register(dpll->dpll_p, dpll->p_rclk, &pin_ops,
			      (void *)dpll->p_rclk_pd))
		goto dpll_p_reg;

	return 0;

dpll_p_reg:
	dpll_pin_unregister(dpll->dpll_e, dpll->p_rclk, &pin_ops,
			    (void *)dpll->p_rclk_pd);
dpll_e_reg:
	kfree(dpll->p_rclk_pd);
p_rclk_pd:
	dpll_pin_put(dpll->p_rclk);
p_rclk:
	free_pin_properties(dpll->pp_rclk);
dpll:
	return -1;
}

void nsim_rclk_free(struct nsim_dpll_info *dpll)
{
	/* Free RCLK pin */
	dpll_pin_unregister(dpll->dpll_e, dpll->p_rclk, &pin_ops,
			    (void *)dpll->p_rclk_pd);
	dpll_pin_unregister(dpll->dpll_p, dpll->p_rclk, &pin_ops,
			    (void *)dpll->p_rclk_pd);
	dpll_pin_put(dpll->p_rclk);
	free_pin_properties(dpll->pp_rclk);
	kfree(dpll->p_rclk_pd);
	dpll_device_put(dpll->dpll_e);
	dpll_device_put(dpll->dpll_p);
}
