/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell RVU PF/VF Netdev Devlink
 *
 * Copyright (C) 2021 Marvell.
 *
 */

#ifndef	OTX2_DEVLINK_H
#define	OTX2_DEVLINK_H

#define OTX2_TRAP_DROP(_id, _group_id)					\
	DEVLINK_TRAP_GENERIC(DROP, DROP, _id,				\
			     DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id, \
			     DEVLINK_TRAP_METADATA_TYPE_F_IN_PORT)
struct otx2_trap {
	struct devlink_trap trap;
};

struct otx2_trap_item {
	enum devlink_trap_action action;
	void *trap_ctx;
};

struct otx2_trap_data {
	struct otx2_devlink *dl;
	struct otx2_trap_item *trap_items_arr;
	u32 traps_count;
};

struct otx2_devlink {
	struct devlink *dl;
	struct otx2_nic *pfvf;
	struct otx2_trap_data *trap_data;
};

/* Devlink APIs */
int otx2_register_dl(struct otx2_nic *pfvf);
void otx2_unregister_dl(struct otx2_nic *pfvf);
void otx2_devlink_traps_unregister(struct otx2_nic *pfvf);
int otx2_devlink_traps_register(struct otx2_nic *pfvf);
#endif /* RVU_DEVLINK_H */
