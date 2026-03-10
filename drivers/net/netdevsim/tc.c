// SPDX-License-Identifier: GPL-2.0

#include <linux/netdevice.h>
#include <net/pkt_sched.h>

#include "netdevsim.h"

static int
nsim_setup_tc_block_cb(enum tc_setup_type type, void *type_data, void *cb_priv)
{
	return nsim_bpf_setup_tc_block_cb(type, type_data, cb_priv);
}

static LIST_HEAD(nsim_block_cb_list);

#define QDISC_OFFLOAD_HANDLERS(X)					\
	X(taprio, tc_taprio_qopt_offload, cmd, TAPRIO_CMD_REPLACE,	\
	  TAPRIO_CMD_DESTROY, TAPRIO_CMD_STATS, stats)			\

#define QH(NAME, OL_TYPE, CMD_FLD, O_REPLACE, O_DESTROY, O_STATS, STATS_FLD) \
static int handle_##NAME(struct net_device *dev, struct OL_TYPE *offload) \
{									\
	switch (offload->CMD_FLD) {					\
	case O_REPLACE:							\
	case O_DESTROY:							\
		/* Do nothing, accept offload */			\
		return 0;						\
	case O_STATS:							\
		/* Zero out the requested stats block */		\
		memset(&offload->STATS_FLD, 0, sizeof(offload->STATS_FLD)); \
		return 0;						\
	default:							\
		return -EOPNOTSUPP;					\
	}								\
}

QDISC_OFFLOAD_HANDLERS(QH)
#undef QH

int
nsim_setup_tc(struct net_device *dev, enum tc_setup_type type, void *type_data)
{
	struct netdevsim *ns = netdev_priv(dev);

	switch (type) {
#define TC_QDISC_SETUP_CASES(X)						\
	X(TC_SETUP_QDISC_TAPRIO, tc_taprio_qopt_offload, taprio)	\

#define SC(SETUP_LABEL, OL_TYPE, NAME)					\
	case SETUP_LABEL:						\
	{								\
		return handle_##NAME(dev, (struct OL_TYPE *)type_data); \
	}

	TC_QDISC_SETUP_CASES(SC)
#undef SC

	case TC_SETUP_BLOCK:
		return flow_block_cb_setup_simple(type_data,
						  &nsim_block_cb_list,
						  nsim_setup_tc_block_cb,
						  ns, ns, true);
	default:
		return -EOPNOTSUPP;
	}
}
