/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2025 ARM Ltd.
 */
#ifndef _UAPI_LINUX_SCMI_H
#define _UAPI_LINUX_SCMI_H

/*
 * Userspace interface SCMI Telemetry
 */

#include <linux/types.h>
#include <linux/ioctl.h>

#define SCMI_TLM_DE_IMPL_VERS		4
#define SCMI_TLM_GRP_INVALID            0xFFFFFFFF

/**
 * scmi_tlm_info  - Basic info about an instance
 *
 * @version: SCMI Telemetry protocol version
 * @de_impl_version: SCMI Telemetry DE implementation revision
 * @num_desi: Number of defined DEs
 * @num_groups Number of defined DEs groups
 * @num_intervals: Number of update intervals available (instance-level)
 * @flags: Instance specific feature-support bitmap
 *
 * Used by:
 *	RO - SCMI_TLM_GET_INFO
 */
struct scmi_tlm_info {
	__u32 version;
	__u32 de_impl_version[SCMI_TLM_DE_IMPL_VERS];
	__u32 num_des;
	__u32 num_groups;
	__u32 num_intervals;
	__u32 flags;
#define SCMI_TLM_CAN_RESET	(1 << 0)
};

/**
 * scmi_tlm_config  - Basic instance configuration
 *
 * @enable: Enable/Disable Telemetry for the whole instance
 * @current_update_interval: Get/Set currently active update interval
 *			     (periodic tick for SHMTIs and Notifications)
 *
 * Used by:
 *	RO - SCMI_TLM_GET_CFG
 *	WO - SCMI_TLM_SET_CFG
 */
struct scmi_tlm_config {
	__u32 enable;
	__u32 current_update_interval;
};

/**
 * scmi_tlm_intervals  - Update intervals descriptor
 *
 * @grp_id: Group identifier (ignored by SCMI_TLM_GET_INTRVS)
 * @discrete: Flag to indicate the nature of the intervals described in
 *	      @available. When 'false' @available is a triplet: min/max/step
 * @num: Number of entries of @available
 * @available: A variably-sized array containing the update intervals
 *
 * Used by:
 *	RO - SCMI_TLM_GET_INTRVS
 *	RW - SCMI_TLM_GET_GRP_INTRVS
 */
struct scmi_tlm_intervals {
	__u32 grp_id;
	__u32 discrete;
	__u32 num;
	__u32 available[];
};

/**
 * scmi_tlm_de_config  - DE configuration
 *
 * @id: Identifier of the DE to act upon (ignored by SCMI_TLM_SET_ALL_CFG)
 * @enable: A boolean to enable/disable the DE
 * @t_enable: A boolean to enable/disable the timestamp for this DE
 *	      (if supported)
 *
 * Used by:
 *	RW - SCMI_TLM_GET_DE_CFG
 *	RW - SCMI_TLM_SET_DE_CFG
 *	WO - SCMI_TLM_SET_ALL_CFG
 */
struct scmi_tlm_de_config {
	__u32 id;
	__u32 enable;
	__u32 t_enable;
};

/**
 * scmi_tlm_de_info  - DE Descriptor
 *
 * @id: DE identifier
 * @grp_id: Identifier of the group which this DE belongs to; reported as
 *	    SCMI_TLM_GRP_INVALID when not part of any group
 * @data_sz: DE data size in bytes
 * @type: DE type
 * @unit: DE unit of measurements
 * @unit_exp: Power-of-10 multiplier for DE unit
 * @tstamp_exp: Power-of-10 multiplier for DE timestamp (if supported)
 * @instance_id: DE instance ID
 * @compo_instance_id: DE component instance ID
 * @compo_type: Type of component which is associated to this DE
 * @peristent: Data value for this DE survives reboot (non-cold ones)
 * @name: Optional name of this DE
 *
 * Used to get the full description of a DE: it reflects DE Descriptors
 * definitions in 3.12.4.6.
 *
 * Used by:
 *	RW - SCMI_TLM_GET_DE_INFO
 *	RO - SCMI_TLM_GET_DE_LIST
 */
struct scmi_tlm_de_info {
	__u32 id;
	__u32 grp_id;
	__u32 data_sz;
	__u32 type;
	__u32 unit;
	__s32 unit_exp;
	__s32 tstamp_exp;
	__u32 instance_id;
	__u32 compo_instance_id;
	__u32 compo_type;
	__u32 persistent;
	__u8 name[16];
};

/**
 * scmi_tlm_des_list  - List of all defined DEs
 *
 * @num_des: Number of entries in @des
 * @des: An array containing descriptors for all defined DEs
 *
 * Used by:
 *	RO - SCMI_TLM_GET_DE_LIST
 */
struct scmi_tlm_des_list {
	__u32 num_des;
	struct scmi_tlm_de_info des[];
};

/**
 * scmi_tlm_de_sample - A DE reading
 *
 * @id: DE identifier
 * @tstamp: DE reading timestamp (equal 0 is NOT supported)
 * @val: Reading of the DE data value
 *
 * Used by:
 *	RW - SCMI_TLM_GET_DE_VALUE
 *	RO - SCMI_TLM_SINGLE_READ
 */
struct scmi_tlm_de_sample {
	__u32 id;
	__u64 tstamp;
	__u64 val;
};

/**
 * scmi_tlm_bulk_read - Bulk read of multiple DEs
 *
 * @grp_id: The identifier of the group to query with a single asynchronous
 *	    sample read. Set to SCMI_TLM_GRP_INVALID to ignore.
 * @num_samples: Number of entries returned in @samples
 * @samples: An array of samples containing an entry for each DE that was
 *	     enabled when the single sample read request was issued.
 *
 * Used by:
 *	RW - SCMI_TLM_SINGLE_SAMPLE
 *	RW - SCMI_TLM_BULK_READ
 */
struct scmi_tlm_bulk_read {
	__u32 grp_id;
	__u32 num_samples;
	struct scmi_tlm_de_sample samples[];
};

/**
 * scmi_tlm_grp_info  - DE-group descriptor
 *
 * @id: Group identifier
 * @flags: Group capabilities
 * @num_intervals: Number of update intervals supported
 * @num_des: Number of DEs part of this group
 *
 * Used by:
 *	WR - SCMI_TLM_GET_GRP_INFO
 */
struct scmi_tlm_grp_info {
	__u32 id;
	__u32 flags;
#define SCMI_TLM_GRP_HAS_UPDATE		(1 << 0)
	__u32 num_intervals;
	__u32 num_des;
};

/**
 * scmi_tlm_grps_list  - DE-groups List
 *
 * @num_grps: Number of entries returned in @grps
 * @grps: An array containing descriptors for all defined DE Groups
 */
struct scmi_tlm_grps_list {
	__u32 num_grps;
	struct scmi_tlm_grp_info grps[];
};

/**
 * scmi_tlm_grp_config  - Group config
 *
 * @id: Identifier of the DEs-group to act upon
 * @enable: A boolean to enable/disable the group
 * @t_enable: A boolean to enable/disable the timestamp for this group
 *
 * Used by:
 *	RW - SCMI_TLM_GET_GRP_CFG
 *	WO - SCMI_TLM_SET_GRP_CFG
 */
struct scmi_tlm_grp_config {
	__u32 id;
	__u32 enable;
	__u32 t_enable;
	__u32 current_update_interval;
};

#define SCMI 0xF1

#define SCMI_TLM_GET_INFO	_IOR(SCMI,  0x00, struct scmi_tlm_info)
#define SCMI_TLM_GET_CFG	_IOR(SCMI,  0x01, struct scmi_tlm_config)
#define SCMI_TLM_SET_CFG	_IOW(SCMI,  0x02, struct scmi_tlm_config)
#define SCMI_TLM_GET_INTRVS	_IOR(SCMI,  0x03, struct scmi_tlm_intervals)
#define SCMI_TLM_GET_DE_CFG	_IOWR(SCMI, 0x04, struct scmi_tlm_de_config)
#define SCMI_TLM_SET_DE_CFG	_IOW(SCMI,  0x05, struct scmi_tlm_de_config)
#define SCMI_TLM_GET_DE_INFO	_IOWR(SCMI, 0x06, struct scmi_tlm_de_info)
#define SCMI_TLM_GET_DE_LIST	_IOWR(SCMI, 0x07, struct scmi_tlm_des_list)
#define SCMI_TLM_GET_DE_VALUE	_IOWR(SCMI, 0x08, struct scmi_tlm_de_sample)
#define SCMI_TLM_GET_GRP_CFG	_IOWR(SCMI, 0x09, struct scmi_tlm_grp_config)
#define SCMI_TLM_SET_GRP_CFG	_IOW(SCMI,  0x0A, struct scmi_tlm_grp_config)
#define SCMI_TLM_GET_GRP_INTRVS	_IOWR(SCMI, 0x0B, struct scmi_tlm_intervals)
#define SCMI_TLM_GET_GRP_INFO	_IOWR(SCMI, 0x0C, struct scmi_tlm_grp_info)
#define SCMI_TLM_GET_GRP_LIST	_IOR(SCMI,  0x0D, struct scmi_tlm_grps_list)
#define SCMI_TLM_SINGLE_SAMPLE	_IOWR(SCMI, 0x0E, struct scmi_tlm_bulk_read)
#define SCMI_TLM_BULK_READ	_IOWR(SCMI, 0x0F, struct scmi_tlm_bulk_read)
#define SCMI_TLM_SET_ALL_CFG	_IOW(SCMI,  0x10, struct scmi_tlm_de_config)

#endif /* _UAPI_LINUX_SCMI_H */
