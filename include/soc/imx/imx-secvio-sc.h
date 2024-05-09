/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2019, 2024 NXP
 */

#ifndef _MISC_IMX_SECVIO_SC_H_
#define _MISC_IMX_SECVIO_SC_H_

#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/notifier.h>

/* Bitmask of the security violation status bit in the HPSVS register */
#define HPSVS_LP_SEC_VIO_MASK BIT(31)
#define HPSVS_SW_LPSV_MASK    BIT(15)
#define HPSVS_SW_FSV_MASK     BIT(14)
#define HPSVS_SW_SV_MASK      BIT(13)
#define HPSVS_SV5_MASK        BIT(5)
#define HPSVS_SV4_MASK        BIT(4)
#define HPSVS_SV3_MASK        BIT(3)
#define HPSVS_SV2_MASK        BIT(2)
#define HPSVS_SV1_MASK        BIT(1)
#define HPSVS_SV0_MASK        BIT(0)

/* Bitmask of all security violation status bit in the HPSVS register */
#define HPSVS_ALL_SV_MASK (HPSVS_LP_SEC_VIO_MASK | \
			     HPSVS_SW_LPSV_MASK | \
			     HPSVS_SW_FSV_MASK | \
			     HPSVS_SW_SV_MASK | \
			     HPSVS_SV5_MASK | \
			     HPSVS_SV4_MASK | \
			     HPSVS_SV3_MASK | \
			     HPSVS_SV2_MASK | \
			     HPSVS_SV1_MASK | \
			     HPSVS_SV0_MASK)

/*
 * Bitmask of the security violation and tampers status bit in the LPS register
 */
#define LPS_ESVD_MASK  BIT(16)
#define LPS_ET2D_MASK  BIT(10)
#define LPS_ET1D_MASK  BIT(9)
#define LPS_WMT2D_MASK BIT(8)
#define LPS_WMT1D_MASK BIT(7)
#define LPS_VTD_MASK   BIT(6)
#define LPS_TTD_MASK   BIT(5)
#define LPS_CTD_MASK   BIT(4)
#define LPS_PGD_MASK   BIT(3)
#define LPS_MCR_MASK   BIT(2)
#define LPS_SRTCR_MASK BIT(1)
#define LPS_LPTA_MASK  BIT(0)

/*
 * Bitmask of all security violation and tampers status bit in the LPS register
 */
#define LPS_ALL_TP_MASK (LPS_ESVD_MASK | \
			   LPS_ET2D_MASK | \
			   LPS_ET1D_MASK | \
			   LPS_WMT2D_MASK | \
			   LPS_WMT1D_MASK | \
			   LPS_VTD_MASK | \
			   LPS_TTD_MASK | \
			   LPS_CTD_MASK | \
			   LPS_PGD_MASK | \
			   LPS_MCR_MASK | \
			   LPS_SRTCR_MASK | \
			   LPS_LPTA_MASK)

/*
 * Bitmask of the security violation and tampers status bit in the LPTDS
 * register
 */
#define LPTDS_ET10D_MASK  BIT(7)
#define LPTDS_ET9D_MASK   BIT(6)
#define LPTDS_ET8D_MASK   BIT(5)
#define LPTDS_ET7D_MASK   BIT(4)
#define LPTDS_ET6D_MASK   BIT(3)
#define LPTDS_ET5D_MASK   BIT(2)
#define LPTDS_ET4D_MASK   BIT(1)
#define LPTDS_ET3D_MASK   BIT(0)

/*
 * Bitmask of all security violation and tampers status bit in the LPTDS
 * register
 */
#define LPTDS_ALL_TP_MASK (LPTDS_ET10D_MASK | \
			     LPTDS_ET9D_MASK | \
			     LPTDS_ET8D_MASK | \
			     LPTDS_ET7D_MASK | \
			     LPTDS_ET6D_MASK | \
			     LPTDS_ET5D_MASK | \
			     LPTDS_ET4D_MASK | \
			     LPTDS_ET3D_MASK)

/* Access for sc_seco_secvio_config API */
#define SECVIO_CONFIG_READ  0
#define SECVIO_CONFIG_WRITE 1

/* Internal Structure */
struct imx_secvio_sc_data {
	struct device *dev;

	struct imx_sc_ipc *ipc_handle;

	struct notifier_block irq_nb;
	struct notifier_block report_nb;

	struct nvmem_device *nvmem;

	struct miscdevice miscdev;

#ifdef CONFIG_DEBUG_FS
	struct dentry *dfs;
#endif

	u32 version;
};

/* Struct for notification */
/**
 * struct secvio_sc_notifier_info - Information about the status of the SNVS
 * @hpsvs: status from register HPSVS
 * @lps:   status from register LPS
 * @lptds: status from register LPTDS
 */
struct secvio_sc_notifier_info {
	u32 hpsvs;
	u32 lps;
	u32 lptds;
};

/**
 * register_imx_secvio_sc_notifier() - Register a notifier
 *
 * @nb: The notifier block structure
 *
 * Register a function to notify to the imx-secvio-sc module. The function
 * will be notified when a check of the state of the SNVS happens: called by
 * a user or triggered by an interruption form the SNVS.
 *
 * The struct secvio_sc_notifier_info is passed as data to the notifier.
 *
 * Return: 0 in case of success
 */
int register_imx_secvio_sc_notifier(struct notifier_block *nb);

/**
 * unregister_imx_secvio_sc_notifier() - Unregister a notifier
 *
 * @nb: The notifier block structure
 *
 * Return: 0 in case of success
 */
int unregister_imx_secvio_sc_notifier(struct notifier_block *nb);

/**
 * imx_secvio_sc_get_state() - Get the state of the SNVS
 *
 * @dev:  Pointer to the struct device of secvio
 * @info: The structure containing the state of the SNVS
 *
 * Return: 0 in case of success
 */
int imx_secvio_sc_get_state(struct device *dev, struct secvio_sc_notifier_info *info);

/**
 * imx_secvio_sc_check_state() - Check the state of the SNVS
 *
 * If a security violation or a tamper is detected, the list of notifier
 * (registered using register_imx_secvio_sc_notifier() ) will be called
 *
 * @dev: Pointer to the struct device of secvio
 *
 * Return: 0 in case of success
 */
int imx_secvio_sc_check_state(struct device *dev);

/**
 * imx_secvio_sc_clear_state() - Clear the state of the SNVS
 *
 * @dev:   Pointer to the struct device of secvio
 * @hpsvs: Value to write to HPSVS register
 * @lps:   Value to write to LPS register
 * @lptds: Value to write to LPTDSregister
 *
 * The function will write the value provided to the corresponding register
 * which will clear the status of the bits set.
 *
 * Return: 0 in case of success
 */
int imx_secvio_sc_clear_state(struct device *dev, u32 hpsvs, u32 lps, u32 lptds);

/* Commands of the ioctl interface */
enum ioctl_cmd_t {
	GET_STATE,
	CHECK_STATE,
	CLEAR_STATE,
};

/* Definition for the ioctl interface */
#define IMX_SECVIO_SC_GET_STATE   _IOR('S', GET_STATE, \
				struct secvio_sc_notifier_info)
#define IMX_SECVIO_SC_CHECK_STATE _IO('S', CHECK_STATE)
#define IMX_SECVIO_SC_CLEAR_STATE _IOW('S', CLEAR_STATE, \
				struct secvio_sc_notifier_info)

#ifdef CONFIG_DEBUG_FS
int imx_secvio_sc_debugfs(struct device *dev);
#else
static inline
int imx_secvio_sc_debugfs(struct device *dev)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */
#endif /* _MISC_IMX_SECVIO_SC_H_ */
