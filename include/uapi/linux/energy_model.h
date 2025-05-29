/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_ENERGY_MODEL_H
#define _UAPI_LINUX_ENERGY_MODEL_H

/* Adding event notification support elements */
#define EM_GENL_FAMILY_NAME		"energy_model"
#define EM_GENL_VERSION			0x01
#define EM_GENL_EVENT_GROUP_NAME	"event"

/* Attributes of em_genl_family */
enum em_genl_attr {
	EM_GENL_ATTR_UNSPEC,
	EM_GENL_ATTR_PAD = EM_GENL_ATTR_UNSPEC,
	EM_GENL_ATTR_PD,	/* Performance domain */
	EM_GENL_ATTR_PD_TBL,	/* Performance table of a performance domain */
	__EM_GENL_ATTR_MAX,
};
#define EM_GENL_ATTR_MAX (__EM_GENL_ATTR_MAX - 1)

enum em_pd_entry_genl_attr {
	EM_PD_ENTRY_GENL_ATTR_UNSPEC,
	EM_PD_ENTRY_GENL_ATTR_PAD = EM_PD_ENTRY_GENL_ATTR_UNSPEC,
	EM_PD_ENTRY_GENL_ATTR_PD,
	__EM_PD_ENTRY_GENL_ATTR_MAX,
};
#define EM_PD_ENTRY_GENL_ATTR_MAX (__EM_PD_ENTRY_GENL_ATTR_MAX - 1)

enum em_pd_genl_attr {
	EM_PD_GENL_ATTR_UNSPEC,
	EM_PD_GENL_ATTR_PAD = EM_PD_GENL_ATTR_UNSPEC,

	/* Performance domain */
	EM_PD_GENL_ATTR_ID,
	EM_PD_GENL_ATTR_FLAGS,
	EM_PD_GENL_ATTR_CPUS,

	__EM_PD_GENL_ATTR_MAX,
};
#define EM_PD_GENL_ATTR_MAX (__EM_PD_GENL_ATTR_MAX - 1)

#define EM_PD_CPUS_LENGTH		256

enum em_tbl_entry_genl_attr {
	EM_TBL_ENTRY_GENL_ATTR_UNSPEC,
	EM_TBL_ENTRY_GENL_ATTR_PAD = EM_TBL_ENTRY_GENL_ATTR_UNSPEC,
	EM_TBL_ENTRY_GENL_ATTR_PD,
	__EM_TBL_ENTRY_GENL_ATTR_MAX,
};
#define EM_TBL_ENTRY_GENL_ATTR_MAX (__EM_TBL_ENTRY_GENL_ATTR_MAX - 1)

enum em_tbl_genl_attr {
	EM_TBL_GENL_ATTR_UNSPEC,
	EM_TBL_GENL_ATTR_PAD = EM_TBL_GENL_ATTR_UNSPEC,

	/* Performance table of a performance domain */
	EM_TBL_GENL_ATTR_PS_PERFORMANCE,
	EM_TBL_GENL_ATTR_PS_FREQUENCY,
	EM_TBL_GENL_ATTR_PS_POWER,
	EM_TBL_GENL_ATTR_PS_COST,
	EM_TBL_GENL_ATTR_PS_FLAGS,

	__EM_TBL_GENL_ATTR_MAX,
};
#define EM_TBL_GENL_ATTR_MAX (__EM_TBL_GENL_ATTR_MAX - 1)

/* Events of em_genl_family */
enum em_genl_event {
	EM_GENL_EVENT_UNSPEC,
	EM_GENL_EVENT_PD_CREATE,	/* Performance domain creation */
	EM_GENL_EVENT_PD_DELETE,	/* Performance domain deletion */
	EM_GENL_EVENT_PD_UPDATE,	/* The runtime EM table for the
					   performance domain is updated */
	__EM_GENL_EVENT_MAX,
};
#define EM_GENL_EVENT_MAX (__EM_GENL_EVENT_MAX - 1)

/* Commands supported by the em_genl_family */
enum em_genl_cmd {
	EM_GENL_CMD_UNSPEC,
	EM_GENL_CMD_PD_GET_ID,		/* Get the list of information
					   for all performance domains */
	EM_GENL_CMD_PD_GET_TBL,		/* Get the energy model table
					   of a performance domain */
	__EM_GENL_CMD_MAX,
};
#define EM_GENL_CMD_MAX (__EM_GENL_CMD_MAX - 1)


#endif /* _UAPI_LINUX_ENERGY_MODEL_H */
