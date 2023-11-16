/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __LINUX_P4TC_H
#define __LINUX_P4TC_H

#include <linux/types.h>
#include <linux/pkt_sched.h>
#include <linux/pkt_cls.h>

/* pipeline header */
struct p4tcmsg {
	__u32 pipeid;
	__u32 obj;
};

#define P4TC_MAXPIPELINE_COUNT 32
#define P4TC_MAXTABLES_COUNT 32
#define P4TC_MINTABLES_COUNT 0
#define P4TC_MSGBATCH_SIZE 16

#define P4TC_MAX_KEYSZ 512
#define P4TC_DEFAULT_NUM_PREALLOC 16

#define TEMPLATENAMSZ 32
#define PIPELINENAMSIZ TEMPLATENAMSZ
#define ACTTMPLNAMSIZ TEMPLATENAMSZ
#define ACTPARAMNAMSIZ TEMPLATENAMSZ

/* Root attributes */
enum {
	P4TC_ROOT_UNSPEC,
	P4TC_ROOT, /* nested messages */
	P4TC_ROOT_PNAME, /* string */
	__P4TC_ROOT_MAX,
};

#define P4TC_ROOT_MAX (__P4TC_ROOT_MAX - 1)

/* P4 Object types */
enum {
	P4TC_OBJ_UNSPEC,
	P4TC_OBJ_PIPELINE,
	P4TC_OBJ_ACT,
	__P4TC_OBJ_MAX,
};

#define P4TC_OBJ_MAX (__P4TC_OBJ_MAX - 1)

/* P4 attributes */
enum {
	P4TC_UNSPEC,
	P4TC_PATH,
	P4TC_PARAMS,
	P4TC_COUNT,
	__P4TC_MAX,
};

#define P4TC_MAX (__P4TC_MAX - 1)

/* PIPELINE attributes */
enum {
	P4TC_PIPELINE_UNSPEC,
	P4TC_PIPELINE_NUMTABLES, /* u16 */
	P4TC_PIPELINE_STATE, /* u8 */
	P4TC_PIPELINE_NAME, /* string only used for pipeline dump */
	__P4TC_PIPELINE_MAX
};

#define P4TC_PIPELINE_MAX (__P4TC_PIPELINE_MAX - 1)

/* PIPELINE states */
enum {
	P4TC_STATE_NOT_READY,
	P4TC_STATE_READY,
};

enum {
	P4T_UNSPEC,
	P4T_U8,
	P4T_U16,
	P4T_U32,
	P4T_U64,
	P4T_STRING,
	P4T_S8,
	P4T_S16,
	P4T_S32,
	P4T_S64,
	P4T_MACADDR,
	P4T_IPV4ADDR,
	P4T_BE16,
	P4T_BE32,
	P4T_BE64,
	P4T_U128,
	P4T_S128,
	P4T_BOOL,
	P4T_DEV,
	P4T_KEY,
	__P4T_MAX,
};

#define P4T_MAX (__P4T_MAX - 1)

/* Action attributes */
enum {
	P4TC_ACT_UNSPEC,
	P4TC_ACT_NAME, /* string */
	P4TC_ACT_PARMS, /* nested params */
	P4TC_ACT_OPT, /* action opt */
	P4TC_ACT_TM, /* action tm */
	P4TC_ACT_ACTIVE, /* u8 */
	P4TC_ACT_NUM_PREALLOC, /* u32 num preallocated action instances */
	P4TC_ACT_PAD,
	__P4TC_ACT_MAX
};

#define P4TC_ACT_MAX (__P4TC_ACT_MAX - 1)

/* Action params attributes */
enum {
	P4TC_ACT_PARAMS_VALUE_UNSPEC,
	P4TC_ACT_PARAMS_VALUE_RAW, /* binary */
	__P4TC_ACT_PARAMS_VALUE_MAX
};

#define P4TC_ACT_VALUE_PARAMS_MAX (__P4TC_ACT_PARAMS_VALUE_MAX - 1)

enum {
	P4TC_ACT_PARAMS_TYPE_UNSPEC,
	P4TC_ACT_PARAMS_TYPE_BITEND, /* u16 */
	P4TC_ACT_PARAMS_TYPE_CONTAINER_ID, /* u32 */
	__P4TC_ACT_PARAMS_TYPE_MAX
};

#define P4TC_ACT_PARAMS_TYPE_MAX (__P4TC_ACT_PARAMS_TYPE_MAX - 1)

/* Action params attributes */
enum {
	P4TC_ACT_PARAMS_UNSPEC,
	P4TC_ACT_PARAMS_NAME, /* string */
	P4TC_ACT_PARAMS_ID, /* u32 */
	P4TC_ACT_PARAMS_VALUE, /* bytes */
	P4TC_ACT_PARAMS_MASK, /* bytes */
	P4TC_ACT_PARAMS_TYPE, /* nested type */
	__P4TC_ACT_PARAMS_MAX
};

#define P4TC_ACT_PARAMS_MAX (__P4TC_ACT_PARAMS_MAX - 1)

struct tc_act_dyna {
	tc_gen;
};

#define P4TC_RTA(r) \
	((struct rtattr *)(((char *)(r)) + NLMSG_ALIGN(sizeof(struct p4tcmsg))))

#endif
