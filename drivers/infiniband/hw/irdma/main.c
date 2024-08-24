// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2015 - 2021 Intel Corporation */
#include "main.h"

MODULE_ALIAS("i40iw");
MODULE_AUTHOR("Intel Corporation, <e1000-rdma@lists.sourceforge.net>");
MODULE_DESCRIPTION("Intel(R) Ethernet Protocol Driver for RDMA");
MODULE_LICENSE("Dual BSD/GPL");

int irdma_vchnl_send_sync(struct irdma_sc_dev *dev, u8 *msg, u16 len,
			  u8 *recv_msg, u16 *recv_len)
{
	struct idc_rdma_core_dev_info *cdev_info = dev_to_rf(dev)->cdev;
	int ret;

	ret = cdev_info->ops->vc_send_sync(cdev_info, msg, len, recv_msg,
					   recv_len);
	if (ret == -ETIMEDOUT) {
		ibdev_err(&(dev_to_rf(dev)->iwdev->ibdev),
			  "Virtual channel Req <-> Resp completion timeout\n");
		dev->vchnl_up = false;
	}

	return ret;
}

static struct notifier_block irdma_inetaddr_notifier = {
	.notifier_call = irdma_inetaddr_event
};

static struct notifier_block irdma_inetaddr6_notifier = {
	.notifier_call = irdma_inet6addr_event
};

static struct notifier_block irdma_net_notifier = {
	.notifier_call = irdma_net_event
};

static struct notifier_block irdma_netdevice_notifier = {
	.notifier_call = irdma_netdevice_event
};

static void irdma_register_notifiers(void)
{
	register_inetaddr_notifier(&irdma_inetaddr_notifier);
	register_inet6addr_notifier(&irdma_inetaddr6_notifier);
	register_netevent_notifier(&irdma_net_notifier);
	register_netdevice_notifier(&irdma_netdevice_notifier);
}

static void irdma_unregister_notifiers(void)
{
	unregister_netevent_notifier(&irdma_net_notifier);
	unregister_inetaddr_notifier(&irdma_inetaddr_notifier);
	unregister_inet6addr_notifier(&irdma_inetaddr6_notifier);
	unregister_netdevice_notifier(&irdma_netdevice_notifier);
}

void irdma_log_invalid_mtu(u16 mtu, struct irdma_sc_dev *dev)
{
	if (mtu < IRDMA_MIN_MTU_IPV4)
		ibdev_warn(to_ibdev(dev), "MTU setting [%d] too low for RDMA traffic. Minimum MTU is 576 for IPv4\n", mtu);
	else if (mtu < IRDMA_MIN_MTU_IPV6)
		ibdev_warn(to_ibdev(dev), "MTU setting [%d] too low for RDMA traffic. Minimum MTU is 1280 for IPv6\\n", mtu);
}

void irdma_fill_qos_info(struct irdma_l2params *l2params,
			 struct iidc_rdma_qos_params *qos_info)
{
	int i;

	l2params->num_tc = qos_info->num_tc;
	l2params->vsi_prio_type = qos_info->vport_priority_type;
	l2params->vsi_rel_bw = qos_info->vport_relative_bw;
	for (i = 0; i < l2params->num_tc; i++) {
		l2params->tc_info[i].egress_virt_up =
			qos_info->tc_info[i].egress_virt_up;
		l2params->tc_info[i].ingress_virt_up =
			qos_info->tc_info[i].ingress_virt_up;
		l2params->tc_info[i].prio_type = qos_info->tc_info[i].prio_type;
		l2params->tc_info[i].rel_bw = qos_info->tc_info[i].rel_bw;
		l2params->tc_info[i].tc_ctx = qos_info->tc_info[i].tc_ctx;
	}
	for (i = 0; i < IIDC_MAX_USER_PRIORITY; i++)
		l2params->up2tc[i] = qos_info->up2tc[i];
	if (qos_info->pfc_mode == IIDC_DSCP_PFC_MODE) {
		l2params->dscp_mode = true;
		memcpy(l2params->dscp_map, qos_info->dscp_map, sizeof(l2params->dscp_map));
	}
}

/**
 * irdma_request_reset - Request a reset
 * @rf: RDMA PCI function
 */
void irdma_request_reset(struct irdma_pci_f *rf)
{
	struct idc_rdma_core_dev_info *cdev_info = rf->cdev;

	ibdev_warn(&rf->iwdev->ibdev, "Requesting a reset\n");
	cdev_info->ops->request_reset(rf->cdev, IDC_FUNC_RESET);
}

static int __init irdma_init_module(void)
{
	int ret;

	ret = auxiliary_driver_register(&i40iw_auxiliary_drv);
	if (ret) {
		pr_err("Failed i40iw(gen_1) auxiliary_driver_register() ret=%d\n",
		       ret);
		return ret;
	}

	ret = auxiliary_driver_register(&icrdma_core_auxiliary_drv.adrv);
	if (ret) {
		auxiliary_driver_unregister(&i40iw_auxiliary_drv);
		pr_err("Failed icrdma(gen_2) auxiliary_driver_register() ret=%d\n",
		       ret);
		return ret;
	}

	ret = auxiliary_driver_register(&ig3rdma_core_auxiliary_drv.adrv);
	if (ret) {
		auxiliary_driver_unregister(&icrdma_core_auxiliary_drv.adrv);
		auxiliary_driver_unregister(&i40iw_auxiliary_drv);
		pr_err("Failed ig3rdma(gen_3) core auxiliary_driver_register() ret=%d\n",
		       ret);

		return ret;
	}

	ret = auxiliary_driver_register(&ig3rdma_vport_auxiliary_drv.adrv);
	if (ret) {
		auxiliary_driver_unregister(&ig3rdma_core_auxiliary_drv.adrv);
		auxiliary_driver_unregister(&icrdma_core_auxiliary_drv.adrv);
		auxiliary_driver_unregister(&i40iw_auxiliary_drv);
		pr_err("Failed ig3rdma vport auxiliary_driver_register() ret=%d\n",
		       ret);

		return ret;
	}
	irdma_register_notifiers();

	return 0;
}

int irdma_vchnl_init(struct irdma_pci_f *rf,
		     struct idc_rdma_core_dev_info *cdev_info, u8 *rdma_ver)
{
	struct irdma_vchnl_init_info virt_info;
	u8 gen = rf->rdma_ver;
	int ret;

	rf->vchnl_wq = alloc_ordered_workqueue("irdma-virtchnl-wq", 0);
	if (!rf->vchnl_wq)
		return -ENOMEM;

	mutex_init(&rf->sc_dev.vchnl_mutex);

	virt_info.is_pf = !cdev_info->ftype;
	virt_info.hw_rev = gen;
	virt_info.privileged = gen == IRDMA_GEN_2;
	virt_info.vchnl_wq = rf->vchnl_wq;
	ret = irdma_sc_vchnl_init(&rf->sc_dev, &virt_info);
	if (ret) {
		destroy_workqueue(rf->vchnl_wq);
		return ret;
	}

	*rdma_ver = rf->sc_dev.hw_attrs.uk_attrs.hw_rev;

	return 0;
}

static void __exit irdma_exit_module(void)
{
	irdma_unregister_notifiers();
	auxiliary_driver_unregister(&icrdma_core_auxiliary_drv.adrv);
	auxiliary_driver_unregister(&i40iw_auxiliary_drv);
	auxiliary_driver_unregister(&ig3rdma_core_auxiliary_drv.adrv);
	auxiliary_driver_unregister(&ig3rdma_vport_auxiliary_drv.adrv);
}

module_init(irdma_init_module);
module_exit(irdma_exit_module);
