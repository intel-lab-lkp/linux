// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG PRUETH QoS submodule
 * Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#include "icssg_prueth.h"
#include "icssg_switch_map.h"

static void icssg_iet_set_preempt_mask(struct prueth_emac *emac, u8 preemptible_tcs)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio = &emac->qos.mqprio;
	struct tc_mqprio_qopt *qopt = &p_mqprio->mqprio.qopt;
	int prempt_mask = 0, i;
	u8 tc;

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

static void icssg_config_ietfpe(struct work_struct *work)
{
	struct prueth_qos_iet *iet =
		container_of(work, struct prueth_qos_iet, fpe_config_task);
	void __iomem *config = iet->emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio =  &iet->emac->qos.mqprio;
	bool enable = !!atomic_read(&iet->enable_fpe_config);
	int ret;
	u8 val;

	if (!netif_running(iet->emac->ndev))
		return;

	mutex_lock(&iet->fpe_lock);

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
	}

	/* Send command to enable FPE Tx side. Rx is always enabled */
	ret = icssg_set_port_state(iet->emac,
				   enable ? ICSSG_EMAC_PORT_PREMPT_TX_ENABLE :
					    ICSSG_EMAC_PORT_PREMPT_TX_DISABLE);
	if (ret) {
		netdev_err(iet->emac->ndev, "TX preempt %s command failed\n",
			   str_enable_disable(enable));
		writeb(0, config + PRE_EMPTION_ENABLE_VERIFY);
		iet->verify_status = ICSSG_IETFPE_STATE_DISABLED;
		goto unlock;
	}

	if (enable && iet->mac_verify_configure) {
		ret = readb_poll_timeout(config + PRE_EMPTION_VERIFY_STATUS, iet->verify_status,
					 (iet->verify_status == ICSSG_IETFPE_STATE_SUCCEEDED),
					 USEC_PER_MSEC, 5 * USEC_PER_SEC);
		if (ret) {
			iet->verify_status = ICSSG_IETFPE_STATE_FAILED;
			netdev_err(iet->emac->ndev,
				   "timeout for MAC Verify: status %x\n",
				   iet->verify_status);
			goto unlock;
		}
	} else if (enable) {
		/* Give f/w some time to update PRE_EMPTION_ACTIVE_TX state */
		usleep_range(100, 200);
	}

	if (enable) {
		val = readb(config + PRE_EMPTION_ACTIVE_TX);
		if (val != 1) {
			netdev_err(iet->emac->ndev,
				   "F/w fails to activate IET/FPE\n");
			goto unlock;
		}
		iet->fpe_active = true;
	} else {
		iet->fpe_active = false;
	}

	netdev_info(iet->emac->ndev, "IET FPE %s successfully\n",
		    str_enable_disable(iet->fpe_active));
	icssg_iet_set_preempt_mask(iet->emac, p_mqprio->preemptible_tcs);

unlock:
	mutex_unlock(&iet->fpe_lock);
}

void icssg_qos_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* Init work queue for IET MAC verify process */
	iet->emac = emac;
	INIT_WORK(&iet->fpe_config_task, icssg_config_ietfpe);
	mutex_init(&iet->fpe_lock);
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
	mutex_lock(&emac->qos.iet.fpe_lock);
	icssg_iet_set_preempt_mask(emac, p_mqprio->preemptible_tcs);
	mutex_unlock(&emac->qos.iet.fpe_lock);

	return 0;
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

void icssg_qos_link_up(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* Enable FPE if not active but fpe_enabled is true
	 * and disable FPE if active but fpe_enabled is false
	 */
	if (!iet->fpe_active && iet->fpe_enabled) {
		/* Schedule IET FPE enable */
		atomic_set(&iet->enable_fpe_config, 1);
	} else if (iet->fpe_active && !iet->fpe_enabled) {
		/* Schedule IET FPE disable */
		atomic_set(&iet->enable_fpe_config, 0);
	} else {
		return;
	}
	schedule_work(&iet->fpe_config_task);
}

void icssg_qos_link_down(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	/* disable FPE if active during link down */
	if (iet->fpe_active) {
		/* Schedule IET FPE disable */
		atomic_set(&iet->enable_fpe_config, 0);
		schedule_work(&iet->fpe_config_task);
	}
}
