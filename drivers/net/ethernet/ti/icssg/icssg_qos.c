// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments ICSSG PRUETH QoS submodule
 * Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#include "icssg_prueth.h"
#include "icssg_switch_map.h"

static int icssg_prueth_iet_fpe_enable(struct prueth_emac *emac);
static void icssg_prueth_iet_fpe_disable(struct prueth_qos_iet *iet);
static void icssg_qos_enable_ietfpe(struct work_struct *work);

void icssg_qos_init(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	if (!iet->fpe_configured)
		return;

	/* Init work queue for IET MAC verify process */
	iet->emac = emac;
	INIT_WORK(&iet->fpe_config_task, icssg_qos_enable_ietfpe);
	init_completion(&iet->fpe_config_compl);

	/* As worker may be sleeping, check this flag to abort
	 * as soon as it comes of out of sleep and cancel the
	 * fpe config task.
	 */
	atomic_set(&iet->cancel_fpe_config, 0);
}

static void icssg_iet_set_preempt_mask(struct prueth_emac *emac, u8 preemptible_tcs)
{
	void __iomem *config = emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio = &emac->qos.mqprio;
	struct tc_mqprio_qopt *qopt = &p_mqprio->mqprio.qopt;
	u8 tc;
	int i;

	/* Configure highest queue as express. Set Bit 4 for FPE,
	 * Reset for express
	 */

	/* first set all 8 queues as Preemptive */
	for (i = 0; i < PRUETH_MAX_TX_QUEUES * PRUETH_NUM_MACS; i++)
		writeb(BIT(4), config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);

	/* set highest priority channel queue as express as default configuration */
	writeb(0, config + EXPRESS_PRE_EMPTIVE_Q_MAP + emac->tx_ch_num - 1);

	/* set up queue mask for FPE. 1 means express */
	writeb(BIT(emac->tx_ch_num - 1), config + EXPRESS_PRE_EMPTIVE_Q_MASK);

	/* Overwrite the express queue mapping based on the tc map set by the user */
	for (tc = 0; tc < p_mqprio->mqprio.qopt.num_tc; tc++) {
		/* check if the tc is express or not */
		if (!(p_mqprio->preemptible_tcs & BIT(tc))) {
			for (i = qopt->offset[tc]; i < qopt->offset[tc] + qopt->count[tc]; i++) {
				/* Set all the queues in this tc as express queues */
				writeb(0, config + EXPRESS_PRE_EMPTIVE_Q_MAP + i);
				writeb(BIT(i), config + EXPRESS_PRE_EMPTIVE_Q_MASK);
			}
		}
		netdev_set_tc_queue(emac->ndev, tc, qopt->count[tc], qopt->offset[tc]);
	}
}

static int prueth_mqprio_validate(struct net_device *ndev,
				  struct tc_mqprio_qopt_offload *mqprio)
{
	int num_tc = mqprio->qopt.num_tc;
	int queue_count = 0;
	int i;

	/* Always start tc-queue mapping from queue 0 */
	if (mqprio->qopt.offset[0] != 0)
		return -EINVAL;

	/* Check for valid number of traffic classes */
	if (num_tc < 1 || num_tc > PRUETH_MAX_TX_QUEUES)
		return -EINVAL;

	/* Only channel mode is supported */
	if (mqprio->mode != TC_MQPRIO_MODE_CHANNEL) {
		netdev_err(ndev, "Unsupported mode: %d\n", mqprio->mode);
		return -EINVAL;
	}

	for (i = 0; i < num_tc; i++) {
		if (!mqprio->qopt.count[i]) {
			netdev_err(ndev, "TC %d has zero size queue count: %d\n",
				   i, mqprio->qopt.count[i]);
			return -EINVAL;
		}
		if (mqprio->min_rate[i] || mqprio->max_rate[i]) {
			netdev_err(ndev, "Min/Max tx rate is not supported\n");
			return -EINVAL;
		}
		if (mqprio->qopt.offset[i] != queue_count) {
			netdev_err(ndev, "Discontinuous queues config is not supported\n");
			return -EINVAL;
		}
		queue_count += mqprio->qopt.count[i];
	}

	if (queue_count > PRUETH_MAX_TX_QUEUES) {
		netdev_err(ndev, "Total queues %d exceed max %d\n",
			   queue_count, PRUETH_MAX_TX_QUEUES);
		return -EINVAL;
	}

	return 0;
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
	struct prueth_qos_mqprio *p_mqprio;
	int ret;

	if (mqprio->qopt.hw == TC_MQPRIO_HW_OFFLOAD_TCS)
		return -EOPNOTSUPP;

	if (!mqprio->qopt.num_tc) {
		netdev_reset_tc(ndev);
		p_mqprio->preemptible_tcs = 0;
		return 0;
	}

	ret = prueth_mqprio_validate(ndev, mqprio);
	if (ret)
		return ret;

	p_mqprio = &emac->qos.mqprio;
	memcpy(&p_mqprio->mqprio, mqprio, sizeof(*mqprio));
	netdev_set_num_tc(ndev, mqprio->qopt.num_tc);

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
EXPORT_SYMBOL_GPL(icssg_qos_ndo_setup_tc);

void icssg_qos_link_up(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	if (!iet->fpe_configured)
		return;

	icssg_prueth_iet_fpe_enable(emac);
}

void icssg_qos_link_down(struct net_device *ndev)
{
	struct prueth_emac *emac = netdev_priv(ndev);
	struct prueth_qos_iet *iet = &emac->qos.iet;

	if (iet->fpe_configured)
		icssg_prueth_iet_fpe_disable(iet);
}

static int icssg_config_ietfpe(struct prueth_qos_iet *iet, bool enable)
{
	void __iomem *config = iet->emac->dram.va + ICSSG_CONFIG_OFFSET;
	struct prueth_qos_mqprio *p_mqprio =  &iet->emac->qos.mqprio;
	int ret;
	u8 val;

	/* If FPE is to be enabled, first configure MAC Verify state
	 * machine in firmware as firmware kicks the Verify process
	 * as soon as ICSSG_EMAC_PORT_PREMPT_TX_ENABLE command is
	 * received.
	 */
	if (enable && iet->mac_verify_configured) {
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
		return ret;
	}

	/* Update FPE Tx enable bit. Assume firmware use this bit
	 * and enable PRE_EMPTION_ACTIVE_TX if everything looks
	 * good at firmware
	 */
	writeb(enable ? 1 : 0, config + PRE_EMPTION_ENABLE_TX);

	if (enable && iet->mac_verify_configured) {
		ret = readb_poll_timeout(config + PRE_EMPTION_VERIFY_STATUS, val,
					 (val == ICSSG_IETFPE_STATE_SUCCEEDED),
					 USEC_PER_MSEC, 5 * USEC_PER_SEC);
		if (ret) {
			netdev_err(iet->emac->ndev,
				   "timeout for MAC Verify: status %x\n",
				   val);
			return ret;
		}
	} else {
		/* Give f/w some time to update PRE_EMPTION_ACTIVE_TX state */
		usleep_range(100, 200);
	}

	if (enable) {
		val = readb(config + PRE_EMPTION_ACTIVE_TX);
		if (val != 1) {
			netdev_err(iet->emac->ndev,
				   "F/w fails to activate IET/FPE\n");
			writeb(0, config + PRE_EMPTION_ENABLE_TX);
			return -ENODEV;
		}
	} else {
		return 0;
	}

	icssg_iet_set_preempt_mask(iet->emac, p_mqprio->preemptible_tcs);

	iet->fpe_enabled = true;

	return ret;
}

static void icssg_qos_enable_ietfpe(struct work_struct *work)
{
	struct prueth_qos_iet *iet =
		container_of(work, struct prueth_qos_iet, fpe_config_task);
	int ret;

	/* Set the required flag and send a command to ICSSG firmware to
	 * enable FPE and start MAC verify
	 */
	ret = icssg_config_ietfpe(iet, true);

	/* if verify configured, poll for the status and complete.
	 * Or just do completion
	 */
	if (!ret)
		netdev_err(iet->emac->ndev, "IET FPE configured successfully\n");
	else
		netdev_err(iet->emac->ndev, "IET FPE config error\n");
	complete(&iet->fpe_config_compl);
}

static void icssg_prueth_iet_fpe_disable(struct prueth_qos_iet *iet)
{
	int ret;

	atomic_set(&iet->cancel_fpe_config, 1);
	cancel_work_sync(&iet->fpe_config_task);
	ret = icssg_config_ietfpe(iet, false);
	if (!ret)
		netdev_err(iet->emac->ndev, "IET FPE disabled successfully\n");
	else
		netdev_err(iet->emac->ndev, "IET FPE disable failed\n");
}

static int icssg_prueth_iet_fpe_enable(struct prueth_emac *emac)
{
	struct prueth_qos_iet *iet = &emac->qos.iet;
	int ret;

	/* Schedule MAC Verify and enable IET FPE if configured */
	atomic_set(&iet->cancel_fpe_config, 0);
	reinit_completion(&iet->fpe_config_compl);
	schedule_work(&iet->fpe_config_task);
	/* By trial, found it takes about 1.5s. So
	 * wait for 10s
	 */
	ret = wait_for_completion_timeout(&iet->fpe_config_compl,
					  msecs_to_jiffies(10000));
	if (!ret) {
		netdev_err(emac->ndev,
			   "IET verify completion timeout\n");
		/* cancel verify in progress */
		atomic_set(&iet->cancel_fpe_config, 1);
		cancel_work_sync(&iet->fpe_config_task);
	}

	return ret;
}
