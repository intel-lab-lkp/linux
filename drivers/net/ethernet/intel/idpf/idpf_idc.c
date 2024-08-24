// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2022 Intel Corporation */

#include "idpf.h"
#include "idpf_virtchnl.h"

static DEFINE_IDA(idpf_idc_ida);

#define IDPF_IDC_MAX_ADEV_NAME_LEN	15

/**
 * idpf_idc_init - Called to initialize IDC
 * @adapter: driver private data structure
 */
int idpf_idc_init(struct idpf_adapter *adapter)
{
	int err;

	if (!idpf_is_rdma_cap_ena(adapter) ||
	    !adapter->dev_ops.idc_init)
		return 0;

	err = adapter->dev_ops.idc_init(adapter);
	if (err)
		dev_err(&adapter->pdev->dev, "failed to initialize idc: %d\n",
			err);

	return err;
}

/**
 * idpf_core_adev_release - function to be mapped to aux dev's release op
 * @dev: pointer to device to free
 */
static void idpf_core_adev_release(struct device *dev)
{
	struct idc_rdma_core_auxiliary_dev *iadev;

	iadev = container_of(dev, struct idc_rdma_core_auxiliary_dev, adev.dev);
	kfree(iadev);
	iadev = NULL;
}

/* idpf_plug_core_aux_dev - allocate and register an Auxiliary device
 * @cdev_info: idc core device info pointer
 */
static int idpf_plug_core_aux_dev(struct idc_rdma_core_dev_info *cdev_info)
{
	struct idc_rdma_core_auxiliary_dev *iadev;
	char name[IDPF_IDC_MAX_ADEV_NAME_LEN];
	struct auxiliary_device *adev;
	int err;

	iadev = (struct idc_rdma_core_auxiliary_dev *)
		kzalloc(sizeof(*iadev), GFP_KERNEL);
	if (!iadev)
		return -ENOMEM;

	adev = &iadev->adev;
	cdev_info->adev = adev;
	iadev->cdev_info = cdev_info;

	adev->id = ida_alloc(&idpf_idc_ida, GFP_KERNEL);
	if (adev->id < 0) {
		pr_err("failed to allocate unique device ID for Auxiliary driver\n");
		err = -ENOMEM;
		goto err_ida_alloc;
	}
	adev->dev.release = idpf_core_adev_release;
	adev->dev.parent = &cdev_info->pdev->dev;
	sprintf(name, "%04x.rdma.core", cdev_info->pdev->vendor);
	adev->name = name;

	err = auxiliary_device_init(adev);
	if (err)
		goto err_aux_dev_init;

	err = auxiliary_device_add(adev);
	if (err)
		goto err_aux_dev_add;

	return 0;

err_aux_dev_add:
	cdev_info->adev = NULL;
	auxiliary_device_uninit(adev);
err_aux_dev_init:
	ida_free(&idpf_idc_ida, adev->id);
err_ida_alloc:
	kfree(iadev);

	return err;
}

/* idpf_unplug_aux_dev - unregister and free an Auxiliary device
 * @adev: auxiliary device struct
 */
static void idpf_unplug_aux_dev(struct auxiliary_device *adev)
{
	auxiliary_device_delete(adev);
	auxiliary_device_uninit(adev);

	ida_free(&idpf_idc_ida, adev->id);
}

/**
 * idpf_idc_vport_dev_ctrl - Called by an Auxiliary Driver
 * @cdev_info: idc core device info pointer
 * @up: RDMA core driver status
 *
 * This callback function is accessed by an Auxiliary Driver to indicate
 * whether core driver is ready to support vport driver load or if vport
 * drivers need to be taken down.
 */
static int
idpf_idc_vport_dev_ctrl(struct idc_rdma_core_dev_info *cdev_info,
			bool up)
{
	return -EOPNOTSUPP;
}

/**
 * idpf_idc_request_reset - Called by an Auxiliary Driver
 * @cdev_info: idc core device info pointer
 * @reset_type: function, core or other
 *
 * This callback function is accessed by an Auxiliary Driver to request a reset
 * on the Auxiliary Device
 */
static int
idpf_idc_request_reset(struct idc_rdma_core_dev_info *cdev_info,
		       enum idc_rdma_reset_type __always_unused reset_type)
{
	return -EOPNOTSUPP;
}

/* Implemented by the Auxiliary Device and called by the Auxiliary Driver */
static const struct idc_rdma_core_ops idc_ops = {
	.vport_dev_ctrl			= idpf_idc_vport_dev_ctrl,
	.request_reset                  = idpf_idc_request_reset,
	.vc_send_sync			= idpf_idc_rdma_vc_send_sync,
};

/**
 * idpf_idc_init_msix_data - initialize MSIX data for the cdev_info structure
 * @adapter: driver private data structure
 */
static void
idpf_idc_init_msix_data(struct idpf_adapter *adapter)
{
	struct idc_rdma_core_dev_info *cdev_info;

	if (!adapter->rdma_msix_entries)
		return;

	cdev_info = adapter->cdev_info;

	cdev_info->msix_entries = adapter->rdma_msix_entries;
	cdev_info->msix_count = adapter->num_rdma_msix_entries;
}

/**
 * idpf_idc_init_aux_core_dev - initialize Auxiliary Device(s)
 * @adapter: driver private data structure
 * @ftype: PF or VF
 */
int idpf_idc_init_aux_core_dev(struct idpf_adapter *adapter,
			       enum idc_function_type ftype)
{
	struct idc_rdma_core_dev_info *cdev_info;
	int err;

	adapter->cdev_info = (struct idc_rdma_core_dev_info *)
		kzalloc(sizeof(struct idc_rdma_core_dev_info), GFP_KERNEL);
	if (!adapter->cdev_info)
		return -ENOMEM;

	cdev_info = adapter->cdev_info;
	cdev_info->pdev = adapter->pdev;
	cdev_info->ops = &idc_ops;
	cdev_info->rdma_protocol = IDC_RDMA_PROTOCOL_ROCEV2;
	cdev_info->ftype = ftype;

	idpf_idc_init_msix_data(adapter);

	err = idpf_plug_core_aux_dev(cdev_info);
	if (err)
		goto err_plug_aux_dev;

	return 0;

err_plug_aux_dev:
	kfree(cdev_info);
	adapter->cdev_info = NULL;

	return err;
}

/**
 * idpf_idc_deinit_core_aux_device - de-initialize Auxiliary Device(s)
 * @cdev_info: idc core device info pointer
 */
void idpf_idc_deinit_core_aux_device(struct idc_rdma_core_dev_info *cdev_info)
{
	if (!cdev_info)
		return;

	idpf_unplug_aux_dev(cdev_info->adev);

	kfree(cdev_info->mapped_mem_regions);
	kfree(cdev_info);
}
