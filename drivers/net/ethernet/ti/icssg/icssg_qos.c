// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG PRUETH QoS submodule
 * Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#include "icssg_prueth.h"
#include "icssg_switch_map.h"

static void icssg_iet_set_preempt_mask(struct prueth_emac *emac)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio = &emac->qos.mqprio;
	struct tc_mqprio_qopt *qopt = &p_mqprio->mqprio.qopt;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int prempt_mask = 0, i;
	u8 tc, num_tc;

	if (!iet->preemptible_tcs)
		goto reset_hw;

	if (iet->fpe_active) {
		/* Configure the queues based on the preemptible tc map set by the user */
		num_tc = p_mqprio->mqprio.qopt.num_tc;
		for (tc = 0; tc < num_tc; tc++) {
			/* check if the tc is preemptive or not */
			if (iet->preemptible_tcs & BIT(tc)) {
				for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
					/* Set all the queues in this tc as preemptive queues */
					writeb(BIT(4), config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
				}
			} else {
				/* Set all the queues in this tc as express queues */
				for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
					writeb(0, config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
					prempt_mask |= BIT(i);
				}
			}
			netdev_set_tc_queue(emac->ndev, tc, qopt->count[tc], qopt->offset[tc]);
		}
		writeb(prempt_mask, config + EXPRESS_PRE_EMPTIVE_Q_MASK);
		return;
	}

reset_hw:
	/* Reset to default: all queues as express */
	for (i = 0; i < ICSSG_MAX_TC_QUEUES; i++)
		writeb(0, config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
	writeb(ICSSG_EXPRESS_Q_MASK_ALL, config + EXPRESS_PRE_EMPTIVE_Q_MASK);
}

static int icssg_iet_verify_wait(struct prueth_emac *emac)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int try = 3;

	do {
		msleep(iet->verify_time_ms);
		iet->verify_status = readb(config + PRE_EMPTION_VERIFY_STATUS);
		if (iet->verify_status == ICSSG_IETFPE_STATE_SUCCEEDED)
			return 0;
	} while (--try > 0);

	netdev_err(emac->ndev, "MAC Verify timeout\n");
	return -ETIMEDOUT;
}

/* Direct synchronous configuration of IET FPE.
 * Caller must hold iet->fpe_lock.
 */
int icssg_config_ietfpe(struct net_device *ndev, bool enable)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;
	u8 val;

	lockdep_assert_held(&iet->fpe_lock);

	if (!netif_running(ndev)) {
		netdev_dbg(ndev, "cannot change IET/FPE state when interface is down\n");
		return 0;
	}

	/* Update FPE Tx enable bit (PRE_EMPTION_ENABLE_TX) if
	 * fpe_enabled is set to enable MM in Tx direction
	 */
	writeb(enable ? 1 : 0, config + PRE_EMPTION_ENABLE_TX);

	/* If FPE is to be enabled, first configure MAC Verify state
	 * machine in firmware as firmware kicks the Verify process
	 * as soon as ICSSG_EMAC_PORT_PREMPT_TX_ENABLE command is
	 * received.
	 */
	if (enable && iet->mac_verify_configure) {
		writeb(1, config + PRE_EMPTION_ENABLE_VERIFY);
		writew(iet->tx_min_frag_size + ETH_FCS_LEN,
		       config + PRE_EMPTION_ADD_FRAG_SIZE_LOCAL);
		writel(iet->verify_time_ms, config + PRE_EMPTION_VERIFY_TIME);
	} else {
		writeb(0, config + PRE_EMPTION_ENABLE_VERIFY);
		iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
	}

	/* Send command to enable FPE Tx side. Rx is always enabled */
	ret = icssg_set_port_state(emac,
				   enable ? ICSSG_EMAC_PORT_PREMPT_TX_ENABLE :
					    ICSSG_EMAC_PORT_PREMPT_TX_DISABLE);
	if (ret) {
		netdev_err(ndev, "TX preempt %s command failed\n",
			   str_enable_disable(enable));
		goto fallback;
	}

	if (enable && iet->mac_verify_configure) {
		ret = icssg_iet_verify_wait(emac);
		if (ret) {
			netdev_err(ndev, "MAC Verification failed with timeout\n");
			goto disable_tx;
		}
	} else if (enable) {
		/* Give firmware some time to update PRE_EMPTION_ACTIVE_TX state */
		usleep_range(100, 200);
	}

	if (enable) {
		val = readb(config + PRE_EMPTION_ACTIVE_TX);
		if (val != 1) {
			netdev_err(ndev,
				   "Firmware fails to activate IET/FPE\n");
			ret = -EIO;
			goto disable_tx;
		}
		iet->fpe_active = true;
	} else {
		iet->fpe_active = false;
	}

	icssg_iet_set_preempt_mask(emac);
	netdev_info(ndev, "IET FPE %s successfully\n",
		   str_enable_disable(iet->fpe_active));
	return ret;

disable_tx:
	icssg_set_port_state(emac, ICSSG_EMAC_PORT_PREMPT_TX_DISABLE);
fallback:
	writeb(0, config + PRE_EMPTION_ENABLE_TX);
	writeb(0, config + PRE_EMPTION_ENABLE_VERIFY);
	iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
	iet->fpe_active =  false;
	return ret;
}

void icssg_qos_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	mutex_init(&iet->fpe_lock);
	/* Set default values to prevent garbage values during .get_mm() */
	mutex_lock(&iet->fpe_lock);
	iet->verify_time_ms = ICSSG_IET_MAX_VERIFY_TIME;
	iet->tx_min_frag_size = ETH_ZLEN;
	mutex_unlock(&iet->fpe_lock);
}
EXPORT_SYMBOL_GPL(icssg_qos_init);

static int icssg_iet_change_preemptible_tcs(struct prueth_emac *emac)
{
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;

	mutex_lock(&iet->fpe_lock);
	ret = icssg_config_ietfpe(emac->ndev, iet->fpe_enabled);
	mutex_unlock(&iet->fpe_lock);

	return ret;
}

static int emac_tc_query_caps(struct net_device *ndev, void *type_data)
{
	struct tc_query_caps_base *base = type_data;

	switch (base->type) {
	case TC_SETUP_QDISC_MQPRIO: {
		struct tc_mqprio_caps *caps = base->caps;

		caps->validate_queue_counts = true;
		return 0;
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int emac_tc_setup_mqprio(struct net_device *ndev, void *type_data)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_mqprio *p_mqprio = &emac->qos.mqprio;
	struct tc_mqprio_qopt_offload *mqprio = type_data;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;

	/* Validate parameters */
	if (mqprio->qopt.num_tc > ICSSG_MAX_TC_QUEUES) {
		netdev_err(ndev, "Number of traffic classes (%u) exceeds hardware limit\n",
			   mqprio->qopt.num_tc);
		return -EINVAL;
	}

	if (mqprio->flags & TC_MQPRIO_F_SHAPER) {
		netdev_err(ndev, "traffic shaping is not supported\n");
		return -EINVAL;
	}

	if (mqprio->flags & (TC_MQPRIO_F_MIN_RATE | TC_MQPRIO_F_MAX_RATE)) {
		netdev_err(ndev, "per-queue rate limiting is not supported\n");
		return -EINVAL;
	}

	if (!mqprio->qopt.num_tc) {
		netdev_reset_tc(ndev);
	} else {
		netdev_set_num_tc(ndev, mqprio->qopt.num_tc);
	}

	mutex_lock(&iet->fpe_lock);
	if (!mqprio->qopt.num_tc) {
		iet->preemptible_tcs = 0;
	} else {
		memcpy(&p_mqprio->mqprio, mqprio, sizeof(*mqprio));
		iet->preemptible_tcs = mqprio->preemptible_tcs;
	}
	mutex_unlock(&iet->fpe_lock);

	netdev_dbg(ndev, "dev->num_tc %u dev->real_num_tx_queues %u\n",
		   ndev->num_tc, ndev->real_num_tx_queues);

	ret = icssg_iet_change_preemptible_tcs(emac);
	return ret;
}

int icssg_qos_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			   void *type_data)
{
	switch (type) {
	case TC_QUERY_CAPS:
		return emac_tc_query_caps(ndev, type_data);
	case TC_SETUP_QDISC_MQPRIO:
		return emac_tc_setup_mqprio(ndev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(icssg_qos_ndo_setup_tc);

void icssg_qos_link_state_update(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;

	ret = icssg_iet_change_preemptible_tcs(emac);
	if (ret)
		netdev_dbg(ndev, "IET FPE %s failed\n",
		   str_enable_disable(iet->fpe_active));
}
EXPORT_SYMBOL_GPL(icssg_qos_link_state_update);
