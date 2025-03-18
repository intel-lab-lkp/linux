// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include "nv_param.h"
#include "sh_devlink.h"
#include "mlx5_core.h"

enum {
	MLX5_CLASS_0_CTRL_ID_NV_GLOBAL_PCI_CONF	       = 0x80,
	MLX5_CLASS_0_CTRL_ID_NV_GLOBAL_PCI_CAP		= 0x81,
};

struct mlx5_ifc_configuration_item_type_class_global_bits {
	u8	 type_class[0x8];
	u8	 parameter_index[0x18];
};

union mlx5_ifc_config_item_type_auto_bits {
	struct mlx5_ifc_configuration_item_type_class_global_bits
				configuration_item_type_class_global;
	u8 reserved_at_0[0x40];
};

struct mlx5_ifc_config_item_bits {
	u8	 valid[0x2];
	u8	 priority[0x2];
	u8	 header_type[0x2];
	u8	 ovr_en[0x1];
	u8	 rd_en[0x1];
	u8	 access_mode[0x2];
	u8	 reserved_at_a[0x1];
	u8	 writer_id[0x5];
	u8	 version[0x4];
	u8	 reserved_at_14[0x2];
	u8	 host_id_valid[0x1];
	u8	 length[0x9];

	union mlx5_ifc_config_item_type_auto_bits type;

	u8	 reserved_at_40[0x10];
	u8	 crc16[0x10];
};

struct mlx5_ifc_mnvda_reg_bits {
	struct mlx5_ifc_config_item_bits configuration_item_header;

	u8	 configuration_item_data[64][0x20];
};

struct mlx5_ifc_nv_global_pci_conf_bits {
	u8	 sriov_valid[0x1];
	u8	 reserved_at_1[0x10];
	u8	 per_pf_total_vf[0x1];
	u8	 reserved_at_12[0xe];

	u8	 sriov_en[0x1];
	u8	 reserved_at_21[0xf];
	u8	 total_vfs[0x10];

	u8	 reserved_at_40[0x20];
};

struct mlx5_ifc_nv_global_pci_cap_bits {
	u8	 max_vfs_per_pf_valid[0x1];
	u8	 reserved_at_1[0x13];
	u8	 per_pf_total_vf_supported[0x1];
	u8	 reserved_at_15[0xb];

	u8	 sriov_support[0x1];
	u8	 reserved_at_21[0xf];
	u8	 max_vfs_per_pf[0x10];

	u8	 reserved_at_40[0x60];
};

#define MNVDA_HDR_SZ \
	(MLX5_ST_SZ_BYTES(mnvda_reg) - MLX5_BYTE_OFF(mnvda_reg, configuration_item_data))

#define MLX5_SET_CONFIG_ITEM_TYPE(_cls_name, _mnvda_ptr, _field, _val) \
	MLX5_SET(mnvda_reg, _mnvda_ptr, \
		 configuration_item_header.type.configuration_item_type_class_##_cls_name._field, \
		 _val)

#define MLX5_SET_CONFIG_HDR_LEN(_mnvda_ptr, _cls_name) \
	MLX5_SET(mnvda_reg, _mnvda_ptr, configuration_item_header.length, \
		 MLX5_ST_SZ_BYTES(_cls_name))

#define MLX5_GET_CONFIG_HDR_LEN(_mnvda_ptr) \
	MLX5_GET(mnvda_reg, _mnvda_ptr, configuration_item_header.length)

static int mlx5_nv_param_read(struct mlx5_core_dev *dev, void *mnvda, size_t len)
{
	u32 param_idx, type_class;
	u32 header_len;
	void *cls_ptr;
	int err;

	if (WARN_ON(len > MLX5_ST_SZ_BYTES(mnvda_reg)) || len < MNVDA_HDR_SZ)
		return -EINVAL; /* A caller bug */

	err = mlx5_core_access_reg(dev, mnvda, len, mnvda, len, MLX5_REG_MNVDA, 0, 0);
	if (!err)
		return 0;

	cls_ptr = MLX5_ADDR_OF(mnvda_reg, mnvda,
			       configuration_item_header.type.configuration_item_type_class_global);

	type_class = MLX5_GET(configuration_item_type_class_global, cls_ptr, type_class);
	param_idx = MLX5_GET(configuration_item_type_class_global, cls_ptr, parameter_index);
	header_len = MLX5_GET_CONFIG_HDR_LEN(mnvda);

	mlx5_core_warn(dev, "Failed to read mnvda reg: type_class 0x%x, param_idx 0x%x, header_len %u, err %d\n",
		       type_class, param_idx, header_len, err);

	/* Let devlink skip this one if it fails, kernel log will have the failure */
	return -EOPNOTSUPP;
}

static int mlx5_nv_param_write(struct mlx5_core_dev *dev, void *mnvda, size_t len)
{
	if (WARN_ON(len > MLX5_ST_SZ_BYTES(mnvda_reg)) || len < MNVDA_HDR_SZ)
		return -EINVAL;

	if (WARN_ON(MLX5_GET_CONFIG_HDR_LEN(mnvda) == 0))
		return -EINVAL;

	return mlx5_core_access_reg(dev, mnvda, len, mnvda, len, MLX5_REG_MNVDA, 0, 1);
}

static int
mlx5_nv_param_read_global_pci_conf(struct mlx5_core_dev *dev, void *mnvda, size_t len)
{
	MLX5_SET_CONFIG_ITEM_TYPE(global, mnvda, type_class, 0);
	MLX5_SET_CONFIG_ITEM_TYPE(global, mnvda, parameter_index,
				  MLX5_CLASS_0_CTRL_ID_NV_GLOBAL_PCI_CONF);
	MLX5_SET_CONFIG_HDR_LEN(mnvda, nv_global_pci_conf);

	return mlx5_nv_param_read(dev, mnvda, len);
}

static int
mlx5_nv_param_read_global_pci_cap(struct mlx5_core_dev *dev, void *mnvda, size_t len)
{
	MLX5_SET_CONFIG_ITEM_TYPE(global, mnvda, type_class, 0);
	MLX5_SET_CONFIG_ITEM_TYPE(global, mnvda, parameter_index,
				  MLX5_CLASS_0_CTRL_ID_NV_GLOBAL_PCI_CAP);
	MLX5_SET_CONFIG_HDR_LEN(mnvda, nv_global_pci_cap);

	return mlx5_nv_param_read(dev, mnvda, len);
}

static int mlx5_shd_enable_sriov_get(struct devlink *devlink, u32 id,
				     struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = mlx5_shd_dev(devlink_priv(devlink));
	u32 mnvda[MLX5_ST_SZ_DW(mnvda_reg)] = {};
	void *cap, *data;
	int err;

	err = mlx5_nv_param_read_global_pci_cap(dev, mnvda, sizeof(mnvda));
	if (err)
		return err;

	cap = MLX5_ADDR_OF(mnvda_reg, mnvda, configuration_item_data);
	if (!MLX5_GET(nv_global_pci_cap, cap, sriov_support))
		return -EOPNOTSUPP;

	memset(mnvda, 0, sizeof(mnvda));
	err = mlx5_nv_param_read_global_pci_conf(dev, mnvda, sizeof(mnvda));
	if (err)
		return err;

	data = MLX5_ADDR_OF(mnvda_reg, mnvda, configuration_item_data);
	ctx->val.vbool = MLX5_GET(nv_global_pci_conf, data, sriov_en);
	return 0;
}

static int mlx5_shd_enable_sriov_set(struct devlink *devlink, u32 id,
				     struct devlink_param_gset_ctx *ctx,
				     struct netlink_ext_ack *extack)
{
	struct mlx5_core_dev *dev = mlx5_shd_dev(devlink_priv(devlink));
	u32 mnvda[MLX5_ST_SZ_DW(mnvda_reg)] = {};
	void *cap, *data;
	int err;

	err = mlx5_nv_param_read_global_pci_cap(dev, mnvda, sizeof(mnvda));
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Failed to read global PCI capability");
		return err;
	}

	cap = MLX5_ADDR_OF(mnvda_reg, mnvda, configuration_item_data);

	if (!MLX5_GET(nv_global_pci_cap, cap, sriov_support)) {
		NL_SET_ERR_MSG_MOD(extack, "Not configurable on this device");
		return -EOPNOTSUPP;
	}

	memset(mnvda, 0, sizeof(mnvda));
	err = mlx5_nv_param_read_global_pci_conf(dev, mnvda, sizeof(mnvda));
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Unable to read global PCI configuration");
		return err;
	}

	data = MLX5_ADDR_OF(mnvda_reg, mnvda, configuration_item_data);
	MLX5_SET(nv_global_pci_conf, data, sriov_valid, 1);
	MLX5_SET(nv_global_pci_conf, data, sriov_en, ctx->val.vbool);

	err = mlx5_nv_param_write(dev, mnvda, sizeof(mnvda));
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Unable to write global PCI configuration");
		return err;
	}

	return 0;
}

static const struct devlink_param mlx5_shd_nv_param_devlink_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_SRIOV, BIT(DEVLINK_PARAM_CMODE_PERMANENT),
			      mlx5_shd_enable_sriov_get,
			      mlx5_shd_enable_sriov_set, NULL),
};

int mlx5_shd_nv_param_register_dl_params(struct devlink *devlink)
{
	return devl_params_register(devlink, mlx5_shd_nv_param_devlink_params,
				    ARRAY_SIZE(mlx5_shd_nv_param_devlink_params));
}

void mlx5_shd_nv_param_unregister_dl_params(struct devlink *devlink)
{
	devl_params_unregister(devlink, mlx5_shd_nv_param_devlink_params,
			       ARRAY_SIZE(mlx5_shd_nv_param_devlink_params));
}
