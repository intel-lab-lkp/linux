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

#define SCMI_TLM_DE_IMPL_MAX_DWORDS	4

#define SCMI_TLM_GRP_INVALID            0xFFFFFFFF

/**
 * scmi_tlm_base_info - Basic info about an instance
 *
 * @version: SCMI Telemetry protocol version
 * @de_impl_version: SCMI Telemetry DE implementation revision
 * @num_de: Number of defined DEs
 * @num_groups Number of defined DEs groups
 * @num_intervals: Number of update intervals available (instance-level)
 * @flags: Instance specific feature-support bitmap
 *
 * Used by:
 *	RO - SCMI_TLM_GET_INFO
 *
 * Supported by:
 *	control/
 */
struct scmi_tlm_base_info {
	__u32 version;
	__u32 de_impl_version[SCMI_TLM_DE_IMPL_MAX_DWORDS];
	__u32 num_des;
	__u32 num_groups;
	__u32 num_intervals;
	__u32 flags;
#define SCMI_TLM_CAN_RESET	(1 << 0)
///TODO more flags
};

/**
 * scmi_tlm_config  - Whole instance or group configuration
 *
 * @enable: Enable/Disable Telemetry for the whole instance or the group
 * @t_enable: Enable/Disable timestamping for all the DEs belonging to a group.
 * @current_update_interval: Get/Set currently active update interval for the
 *			     whole instance or a group.
 *
 * Used by:
 *	RO - SCMI_TLM_GET_CFG
 *	WO - SCMI_TLM_SET_CFG
 *
 * Supported by:
 *	control/
 *	groups/<N>/control
 */
struct scmi_tlm_config {
	__u8 enable;
	__u8 t_enable;
	__u8 reserved[2];
	__u32 current_update_interval;
};

/**
 * scmi_tlm_intervals  - Update intervals descriptor
 *
 * @discrete: Flag to indicate the nature of the intervals described in
 *	      @update_intervals.
 *	      When 'false' @update_intervals is a triplet: min/max/step
 * @num: Number of entries of @available
 * @update_intervals: A variably-sized array containing the update intervals
 *
 * Used by:
 *	RW - SCMI_TLM_GET_INTRVS
 *
 * Supported by:
 *	control/
 *	groups/<N>/control
 */
struct scmi_tlm_intervals {
	__u8 discrete;
	__u8 reserved[3];
	__u32 num;
#define SCMI_TLM_UPDATE_INTVL_SEGMENT_LOW	0
#define SCMI_TLM_UPDATE_INTVL_SEGMENT_HIGH	1
#define SCMI_TLM_UPDATE_INTVL_SEGMENT_STEP	2
	__u32 update_intervals[];
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
 *
 * Supported by:
 *	control/
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
 *
 * Supported by:
 *	control/
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
 *	RW - SCMI_TLM_GET_DE_LIST
 *
 * Supported by:
 *	control/
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
 *
 * Supported by:
 *	control/
 */
struct scmi_tlm_de_sample {
	__u32 id;
	__u64 tstamp;
	__u64 val;
};

/**
 * scmi_tlm_data_read - Bulk read of multiple DEs
 *
 * @num_samples: Number of entries returned in @samples
 * @samples: An array of samples containing an entry for each DE that was
 *	     enabled when the single sample read request was issued.
 *
 * Used by:
 *	RW - SCMI_TLM_SINGLE_SAMPLE
 *	RW - SCMI_TLM_BULK_READ
 *
 * Supported by:
 *	control/
 *	groups/<N>/control
 */
struct scmi_tlm_data_read {
	__u32 num_samples;
	struct scmi_tlm_de_sample samples[];
};

/**
 * scmi_tlm_grp_info  - DE-group descriptor
 *
 * @id: Group ID number
 * @num_des: Number of DEs part of this group
 * @num_intervals: Number of update intervals supported. Zero if group does not
 *		   support per-group update interval configuration.
 *
 * Used by:
 *	RO - SCMI_TLM_GET_GRP_INFO
 *
 * Supported by:
 *	groups/<N>control/
 */
struct scmi_tlm_grp_info {
	__u32 id;
	__u32 num_des;
	__u32 num_intervals;
};

/**
 * scmi_tlm_grps_list  - DE-groups List
 *
 * @num_grps: Number of entries returned in @grps
 * @grps: An array containing descriptors for all defined DE Groups
 *
 * Used by:
 *	RW - SCMI_TLM_GET_GRP_LIST
 *
 * Supported by:
 *	control/
 */
struct scmi_tlm_grps_list {
	__u32 num_grps;
	struct scmi_tlm_grp_info grps[];
};

/**
 * scmi_tlm_grp_desc  - Group descriptor
 *
 * @num_des: Number of DEs part of this group
 * @composing_des: An array containing the DE IDs that belongs to this group.
 *
 * Used by:
 *	RW - SCMI_TLM_GET_GRP_DESC
 *
 * Supported by:
 *	groups/<N>control/
 */
struct scmi_tlm_grp_desc {
	__u32 num_des;
	__u32 composing_des[];
};

#define SCMI 0xF1

#define SCMI_TLM_GET_INFO	_IOR(SCMI,  0x00, struct scmi_tlm_base_info)
#define SCMI_TLM_GET_CFG	_IOR(SCMI,  0x01, struct scmi_tlm_config)
#define SCMI_TLM_SET_CFG	_IOW(SCMI,  0x02, struct scmi_tlm_config)
#define SCMI_TLM_GET_INTRVS	_IOWR(SCMI, 0x03, struct scmi_tlm_intervals)
#define SCMI_TLM_GET_DE_CFG	_IOWR(SCMI, 0x04, struct scmi_tlm_de_config)
#define SCMI_TLM_SET_DE_CFG	_IOWR(SCMI, 0x05, struct scmi_tlm_de_config)
#define SCMI_TLM_GET_DE_INFO	_IOWR(SCMI, 0x06, struct scmi_tlm_de_info)
#define SCMI_TLM_GET_DE_LIST	_IOWR(SCMI, 0x07, struct scmi_tlm_des_list)
#define SCMI_TLM_GET_DE_VALUE	_IOWR(SCMI, 0x08, struct scmi_tlm_de_sample)
#define SCMI_TLM_SET_ALL_CFG	_IOW(SCMI,  0x09, struct scmi_tlm_de_config)
#define SCMI_TLM_GET_GRP_LIST	_IOWR(SCMI, 0x0A, struct scmi_tlm_grps_list)
#define SCMI_TLM_GET_GRP_INFO	_IOR(SCMI,  0x0B, struct scmi_tlm_grp_info)
#define SCMI_TLM_GET_GRP_DESC	_IOWR(SCMI, 0x0C, struct scmi_tlm_grp_desc)
#define SCMI_TLM_SINGLE_SAMPLE	_IOWR(SCMI, 0x0D, struct scmi_tlm_data_read)
#define SCMI_TLM_BULK_READ	_IOWR(SCMI, 0x0E, struct scmi_tlm_data_read)

#endif /* _UAPI_LINUX_SCMI_H */
