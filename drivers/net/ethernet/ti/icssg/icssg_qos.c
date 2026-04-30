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
	u8 preemptible_tcs = p_mqprio->preemptible_tcs;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int prempt_mask = 0, i;
	u8 tc;

	/* The preemptible traffic classes should only be committed to hardware
	 * once TX is active.
	 */
	if (!iet->fpe_active) {
		netdev_dbg(emac->ndev, "FPE not active, skipping preempt mask config\n");
		return;
	}

	/* Configure the queues based on the preemptible tc map set by the user */
	for (tc = 0; tc < p_mqprio->mqprio.qopt.num_tc; tc++) {
		/* check if the tc is preemptive or not */
		if (preemptible_tcs & BIT(tc)) {
			for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
				/* Set all the queues in this tc as preemptive queues */
				writeb(BIT(4), config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
				prempt_mask &= ~BIT(i);
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
void icssg_config_ietfpe(struct prueth_emac *emac, bool enable)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;
	u8 val;

	/* return early if FPE is not active and need not be enabled */
	if (!iet->fpe_enabled && !iet->fpe_active)
		return;

	if (!netif_running(emac->ndev)) {
		netdev_dbg(emac->ndev, "cannot change IET/FPE state when interface is down\n");
		return;
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
		writew(iet->tx_min_frag_size, config + PRE_EMPTION_ADD_FRAG_SIZE_LOCAL);
		writel(iet->verify_time_ms, config + PRE_EMPTION_VERIFY_TIME);
	} else {
		iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
	}

	/* Send command to enable FPE Tx side. Rx is always enabled */
	ret = icssg_set_port_state(emac,
				   enable ? ICSSG_EMAC_PORT_PREMPT_TX_ENABLE :
					    ICSSG_EMAC_PORT_PREMPT_TX_DISABLE);
	if (ret) {
		netdev_err(emac->ndev, "TX preempt %s command failed\n",
			   str_enable_disable(enable));
		writeb(0, config + PRE_EMPTION_ENABLE_VERIFY);
		iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
		return;
	}

	if (enable && iet->mac_verify_configure) {
		ret = icssg_iet_verify_wait(emac);
		if (ret) {
			netdev_err(emac->ndev, "MAC Verification failed with timeout\n");
			return;
		}
	} else if (enable) {
		/* Give firmware some time to update PRE_EMPTION_ACTIVE_TX state */
		usleep_range(100, 200);
	}

	if (enable) {
		val = readb(config + PRE_EMPTION_ACTIVE_TX);
		if (val != 1) {
			netdev_err(emac->ndev,
				   "Firmware fails to activate IET/FPE\n");
			return;
		}
		iet->fpe_active = true;
	} else {
		iet->fpe_active = false;
	}

	icssg_iet_set_preempt_mask(emac);
	netdev_err(emac->ndev, "IET FPE %s successfully\n",
		  str_enable_disable(iet->fpe_active));
}
EXPORT_SYMBOL_GPL(icssg_config_ietfpe);

void icssg_qos_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	iet->emac = emac;
	mutex_init(&iet->fpe_lock);
}

static void icssg_iet_change_preemptible_tcs(struct prueth_emac *emac)
{
	struct prueth_qos_iet *iet = &emac->qos.iet;

	mutex_lock(&iet->fpe_lock);
	icssg_config_ietfpe(emac, iet->fpe_enabled);
	mutex_unlock(&iet->fpe_lock);
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
	struct tc_mqprio_qopt_offload *mqprio = type_data;
	struct prueth_emac *emac = netdev_priv(ndev);
	struct tc_mqprio_qopt *qopt = &mqprio->qopt;
	struct prueth_qos_mqprio *p_mqprio;
	u8 num_tc = mqprio->qopt.num_tc;
	int tc, offset, count;

	p_mqprio = &emac->qos.mqprio;

	if (!num_tc) {
		netdev_reset_tc(ndev);
		p_mqprio->preemptible_tcs = 0;
		p_mqprio->mqprio.qopt.num_tc = 0;
		goto reset_tcs;
	}

	memcpy(&p_mqprio->mqprio, mqprio, sizeof(*mqprio));
	p_mqprio->preemptible_tcs = mqprio->preemptible_tcs;
	netdev_set_num_tc(ndev, mqprio->qopt.num_tc);

	for (tc = 0; tc < num_tc; tc++) {
		count = qopt->count[tc];
		offset = qopt->offset[tc];
		netdev_set_tc_queue(ndev, tc, count, offset);
	}

reset_tcs:
	icssg_iet_change_preemptible_tcs(emac);

	return 0;
}
EXPORT_SYMBOL_GPL(icssg_qos_init);

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

	icssg_iet_change_preemptible_tcs(emac);
}
EXPORT_SYMBOL_GPL(icssg_qos_link_state_update);
