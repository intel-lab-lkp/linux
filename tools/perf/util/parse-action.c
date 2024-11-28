// SPDX-License-Identifier: GPL-2.0
/**
 * Generic actions for sampling events
 * Actions are the programs that run when the sampling event is triggered.
 * The action is a list of expressions separated by semicolons (;).
 * Each action is an expression, added to actions_head node as list_head node.
 *
 * Supported expressions:
 *   - constant:
 *     - integer
 *     - string
 */

#include "util/debug.h"
#include "util/parse-action.h"
#include "util/parse-action-flex.h"
#include "util/parse-action-bison.h"

static struct list_head actions_head = LIST_HEAD_INIT(actions_head);

int event_actions__for_each_expr(int (*func)(struct evtact_expr *, void *arg),
				 void *arg, bool recursive)
{
	int ret;
	struct evtact_expr *expr, *opnd;

	if (list_empty(&actions_head))
		return (*func)(NULL, arg);

	list_for_each_entry(expr, &actions_head, list) {
		ret = (*func)(expr, arg);
		if (ret)
			return ret;

		if (recursive && !list_empty(&expr->opnds)) {
			list_for_each_entry(opnd, &expr->opnds, list) {
				ret = (*func)(opnd, arg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

int event_actions__for_each_expr_safe(int (*func)(struct evtact_expr *, void *arg),
				      void *arg, bool recursive)
{
	int ret;
	struct evtact_expr *expr, *tmp;
	struct evtact_expr *opnd, *opnd_tmp;

	if (list_empty(&actions_head))
		return (*func)(NULL, arg);

	list_for_each_entry_safe(expr, tmp, &actions_head, list) {
		ret = (*func)(expr, arg);
		if (ret)
			return ret;

		if (recursive && !list_empty(&expr->opnds)) {
			list_for_each_entry_safe(opnd, opnd_tmp, &expr->opnds, list) {
				ret = (*func)(opnd, arg);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static int parse_action_option(const char *str)
{
	int ret;
	YY_BUFFER_STATE buffer;

	buffer = parse_action__scan_string(str);
	ret = parse_action_parse(&actions_head);

	parse_action__flush_buffer(buffer);
	parse_action__delete_buffer(buffer);
	parse_action_lex_destroy();

	return ret;
}

int parse_record_action(struct evlist *evlist, const char *str)
{
	int ret;

	if (evlist == NULL) {
		pr_err("--action option should follow a tracer option\n");
		return -1;
	}

	ret = parse_action_option(str);
	if (ret) {
		event_actions__free();
		pr_err("parse action option failed\n");
		return ret;
	}

	return 0;
}

static int do_action_free(struct evtact_expr *action, void *data __maybe_unused)
{
	if (action == NULL)
		return 0;

	list_del(&action->list);
	parse_action_expr__free(action);
	return 0;
}

void event_actions__free(void)
{
	(void)event_actions__for_each_expr_safe(do_action_free, NULL, false);
}

static int expr_const_int_new(struct evtact_expr *expr, void *data, int size)
{
	if (data == NULL ||
	    (size != sizeof(int)
	     && size != sizeof(long) && size != sizeof(long long))) {
		pr_err("expr_const_int size invalid: %d\n", size);
		return -EINVAL;
	}

	expr->priv = malloc(sizeof(long long));
	if (expr->priv == NULL) {
		pr_err("exp_ const_int malloc failed\n");
		return -ENOMEM;
	}

	if (size == sizeof(int))
		*(unsigned long long *)(expr->priv) = *(unsigned int *)data;
	else if (size == sizeof(long))
		*(unsigned long long *)(expr->priv) = *(unsigned long *)data;
	else if (size == sizeof(long long))
		*(unsigned long long *)(expr->priv) = *(unsigned long long *)data;

	INIT_LIST_HEAD(&expr->opnds);
	return 0;
}

static void expr_const_int_free(struct evtact_expr *expr)
{
	zfree(&expr->priv);
}

static int expr_const_int_eval(struct evtact_expr *expr,
			       void *in __maybe_unused, int in_size __maybe_unused,
			       void **out, int *out_size)
{
	if (out != NULL)
		*out = expr->priv;

	if (out_size != NULL)
		*out_size = sizeof(long long);

	return 0;
}

static struct evtact_expr_ops expr_const_int_ops = {
	.new  = expr_const_int_new,
	.free = expr_const_int_free,
	.eval = expr_const_int_eval,
};

static int expr_const_str_new(struct evtact_expr *expr,
			      void *data, int size __maybe_unused)
{
	if (data == NULL) {
		pr_err("exper const string is NULL\n");
		return -EINVAL;
	}

	expr->priv = data;
	INIT_LIST_HEAD(&expr->opnds);
	return 0;
}

static void expr_const_str_free(struct evtact_expr *expr)
{
	zfree(&expr->priv);
}

static int expr_const_str_eval(struct evtact_expr *expr,
			       void *in __maybe_unused, int in_size __maybe_unused,
			       void **out, int *out_size)
{
	if (out != NULL)
		*out = expr->priv;

	if (out_size != NULL)
		*out_size = strlen(expr->priv);

	return 0;
}

static struct evtact_expr_ops expr_const_str_ops = {
	.new  = expr_const_str_new,
	.free = expr_const_str_free,
	.eval = expr_const_str_eval,
};

static struct evtact_expr_ops *expr_const_ops_list[EVTACT_EXPR_CONST_TYPE_MAX] = {
	[EVTACT_EXPR_CONST_TYPE_INT] = &expr_const_int_ops,
	[EVTACT_EXPR_CONST_TYPE_STR] = &expr_const_str_ops,
};

static int expr_const_set_ops(struct evtact_expr *expr, u32 opcode)
{
	if (opcode >= EVTACT_EXPR_CONST_TYPE_MAX) {
		pr_err("expr_const opcode invalid: %u\n", opcode);
		return -EINVAL;
	}

	if (expr_const_ops_list[opcode] == NULL) {
		pr_err("expr_const opcode not supported: %u\n", opcode);
		return -ENOTSUP;
	}

	expr->ops = expr_const_ops_list[opcode];
	return 0;
}

static struct evtact_expr_class expr_const = {
	.set_ops = expr_const_set_ops,
};

static struct evtact_expr_class *expr_class_list[EVTACT_EXPR_TYPE_MAX] = {
	[EVTACT_EXPR_TYPE_CONST]   = &expr_const,
};

int parse_action_expr__set_class(enum evtact_expr_type type,
				 struct evtact_expr_class *class)
{
	if (type >= EVTACT_EXPR_TYPE_MAX) {
		pr_err("action expr set class ops type invalid\n");
		return -EINVAL;
	}

	if (expr_class_list[type] != NULL) {
		pr_err("action expr set class ops type already exists\n");
		return -EEXIST;
	}

	expr_class_list[type] = class;
	return 0;
}

static int expr_set_type(struct evtact_expr *expr)
{
	u64 id;
	int ret;
	u32 type, opcode;
	struct evtact_expr_class *class;

	id = expr->id;
	evtact_expr_id_decode(id, &type, &opcode);

	if (type >= EVTACT_EXPR_TYPE_MAX) {
		pr_err("parse_action_expr type invalid: %u\n", type);
		return -EINVAL;
	}

	class = expr_class_list[type];
	if (class == NULL) {
		pr_err("parse_action_expr class not supported: %u\n", type);
		return -ENOTSUP;
	}

	if (class->set_ops != NULL) {
		ret = class->set_ops(expr, opcode);
		if (ret)
			return ret;
	}

	return 0;
}

struct evtact_expr *parse_action_expr__new(u64 id, struct list_head *opnds,
					   void *data, int size)
{
	int ret;
	struct evtact_expr *expr;

	expr = malloc(sizeof(*expr));
	if (expr == NULL) {
		pr_err("parse_action_expr malloc failed\n");
		goto out_free_opnds;
	}
	expr->id = id;

	if (opnds != NULL)
		list_add_tail(&expr->opnds, opnds);
	else
		INIT_LIST_HEAD(&expr->opnds);

	ret = expr_set_type(expr);
	if (ret)
		goto out_list_del_opnds;

	if (expr->ops->new != NULL) {
		ret = expr->ops->new(expr, data, size);
		if (ret)
			goto out_free_expr;
	}

	return expr;

out_free_expr:
	free(expr);
out_list_del_opnds:
	list_del(&expr->opnds);
out_free_opnds:
	parse_action_expr__free_opnds(opnds);

	return NULL;
}

void parse_action_expr__free_opnds(struct list_head *opnds)
{
	struct evtact_expr *opnd, *tmp;

	if (opnds != NULL && !list_empty(opnds)) {
		list_for_each_entry_safe(opnd, tmp, opnds, list) {
			list_del(&opnd->list);
			parse_action_expr__free(opnd);
		}
	}
}

void parse_action_expr__free(struct evtact_expr *expr)
{
	if (expr == NULL)
		return;

	if (expr->ops->free != NULL)
		expr->ops->free(expr);

	parse_action_expr__free_opnds(&expr->opnds);
	free(expr);
}
