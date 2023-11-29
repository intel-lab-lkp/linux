// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2023 Hewlett Packard Enterprise, Inc. All rights reserved.
 */

#include "rxe.h"

int rxe_max_mcast_grp = RXE_MAX_MCAST_GRP;
module_param_named(max_mcast_grp, rxe_max_mcast_grp, int, 0444);
MODULE_PARM_DESC(max_mcast_grp,
	"Maximum number of multicast groups per device");

int rxe_max_mcast_qp_attach = RXE_MAX_MCAST_QP_ATTACH;
module_param_named(max_mcast_qp_attach, rxe_max_mcast_qp_attach,
		int, 0444);
MODULE_PARM_DESC(max_mcast_qp_attach,
	"Maximum number of QPs attached to a multicast group");

int rxe_max_tot_mcast_qp_attach = RXE_MAX_TOT_MCAST_QP_ATTACH;
module_param_named(max_tot_mcast_qp_attach, rxe_max_tot_mcast_qp_attach,
		int, 0444);
MODULE_PARM_DESC(max_tot_mcast_qp_attach,
	"Maximum total number of QPs attached to multicast groups per device");
