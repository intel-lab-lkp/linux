/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_UTIL_PARSE_ACTION_H_
#define __PERF_UTIL_PARSE_ACTION_H_

#include <linux/types.h>

#include <subcmd/parse-options.h>

#include "evlist.h"

enum evtact_expr_type {
	EVTACT_EXPR_TYPE_CONST,
	EVTACT_EXPR_TYPE_CALL,
	EVTACT_EXPR_TYPE_BUILTIN,
	EVTACT_EXPR_TYPE_MAX,
};

enum evtact_expr_const_type {
	EVTACT_EXPR_CONST_TYPE_INT,
	EVTACT_EXPR_CONST_TYPE_STR,
	EVTACT_EXPR_CONST_TYPE_MAX,
};

enum evtact_expr_call_type {
	EVTACT_EXPR_CALL_TYPE_PRINT,
	EVTACT_EXPR_CALL_TYPE_MAX,
};

enum evtact_expr_builtin_type {
	EVTACT_EXPR_BUILTIN_TYPE_CPU,
	EVTACT_EXPR_BUILTIN_TYPE_PID,
	EVTACT_EXPR_BUILTIN_TYPE_TID,
	EVTACT_EXPR_BUILTIN_TYPE_MAX,
};

struct evtact_expr;
struct evtact_expr_ops {
	int (*new)(struct evtact_expr *expr, void *data, int size);
	int (*eval)(struct evtact_expr *expr,
		    void *in, int in_size, void **out, int *out_size);
	void (*free)(struct evtact_expr *expr);
};

struct evtact_expr_class {
	int (*set_ops)(struct evtact_expr *expr, u32 opcode);
};

struct evtact_expr {
	struct list_head list;
	u64 id;
	struct evtact_expr_ops *ops;
	struct list_head opnds;
	void *priv;
};

/*
 * The expr id contains two fileds:
 * |--------------|----------------|
 * |     type     |     opcode     |
 * |--------------|----------------|
 *      32-bit           32-bit
 */
#define EVTACT_EXPR_ID_TYPE_BITS_SHIFT 32
static inline u64 evtact_expr_id_encode(u32 type, u32 opcode)
{
	return (u64)type << EVTACT_EXPR_ID_TYPE_BITS_SHIFT | opcode;
}

static inline void evtact_expr_id_decode(u64 id, u32 *type, u32 *opcode)
{
	if (type != NULL)
		*type = id >> EVTACT_EXPR_ID_TYPE_BITS_SHIFT;

	if (opcode != NULL)
		*opcode = id & GENMASK(EVTACT_EXPR_ID_TYPE_BITS_SHIFT, 0);
}

int parse_record_action(struct evlist *evlist, const char *str);
void event_actions__free(void);

int event_actions__for_each_expr(int (*func)(struct evtact_expr *, void *arg),
				 void *arg, bool recursive);

int event_actions__for_each_expr_safe(int (*func)(struct evtact_expr *, void *arg),
				      void *arg, bool recursive);

struct evtact_expr *parse_action_expr__new(u64 id, struct list_head *opnds,
					   void *data, int size);

void parse_action_expr__free_opnds(struct list_head *opnds);
void parse_action_expr__free(struct evtact_expr *expr);

int parse_action_expr__set_class(enum evtact_expr_type type,
				 struct evtact_expr_class *ops);

#endif /* __PERF_UTIL_PARSE_ACTION_H_ */
