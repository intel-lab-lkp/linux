/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2026 Hisilicon Limited. */

#ifndef __HCLGE_VF_REP_H
#define __HCLGE_VF_REP_H

struct hnae3_ae_dev;
struct hclge_dev;
struct hclge_vport;
struct net_device;

struct hclge_vf_rep {
	struct hclge_dev	*hdev;
	struct hclge_vport	*vport;
	struct net_device	*netdev;
};

int hclge_create_vf_reps(struct hnae3_ae_dev *ae_dev, int num_vfs);
void hclge_destroy_vf_reps(struct hnae3_ae_dev *ae_dev);

#endif
