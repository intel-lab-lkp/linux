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
 *   - call:
 *     - print
 *   - builtin:
 */

#include <regex.h>

#include "util/debug.h"
#include "util/parse-action.h"
#include "util/parse-action-flex.h"
#include "util/parse-action-bison.h"
#include "util/record_action.h"

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

static bool initialized = false;
static int parse_action_init(void)
{
	int ret;

	if (initialized)
		return 0;

	ret = bpf_perf_record_init();
	if (ret)
		return ret;

	initialized = true;
	return 0;
}

int parse_record_action(struct evlist *evlist, const char *str)
{
	int ret;

	ret = parse_action_init();
	if (ret)
		return ret;

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

static bool is_const_str_expr(struct evtact_expr *expr)
{
	return expr->id == evtact_expr_id_encode(EVTACT_EXPR_TYPE_CONST,
						 EVTACT_EXPR_CONST_TYPE_STR);
}

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

enum print_fmt_spec_len {
	PRINT_FMT_SPEC_LEN_INT,
	PRINT_FMT_SPEC_LEN_LONG,
	PRINT_FMT_SPEC_LEN_LONG_LONG,
};

enum print_fmt_spec_type {
	PRINT_FMT_SPEC_TYPE_VOID,
	PRINT_FMT_SPEC_TYPE_INT,
	PRINT_FMT_SPEC_TYPE_STRING,
};

struct print_fmt_spec {
	struct list_head list;
	enum print_fmt_spec_type type;
	enum print_fmt_spec_len len;
	char *str;
};

#define PRINT_OUT_BUF_DEFAULT_LEN 64
struct print_expr_priv {
	struct list_head fmt_specs;
	int fmt_spec_num;
	int args_num;
	char *out;
	int out_len;
};

static int print_fmt_add_spec(struct print_expr_priv *priv, char *s,
			      regoff_t len, enum print_fmt_spec_len spec_len,
			      enum print_fmt_spec_type spec_type)
{
	struct print_fmt_spec *spec;

	spec = malloc(sizeof(*spec));
	if (spec == NULL) {
		pr_err("call print fmt spec malloc failed\n");
		return -ENOMEM;
	}

	spec->str = strndup(s, len);
	if (spec->str == NULL) {
		pr_err("call print fmt spec strndup failed\n");
		free(spec);
		return -ENOMEM;
	}

	spec->len = spec_len;
	spec->type = spec_type;
	list_add_tail(&spec->list, &priv->fmt_specs);
	return 0;
}

static int print_fmt_spec_get_len(char *s, regoff_t len)
{
	if (len == 1 && !strncmp(s, "l", len))
		return PRINT_FMT_SPEC_LEN_LONG;
	else if (len == 2 && !strncmp(s, "ll", len))
		return PRINT_FMT_SPEC_LEN_LONG_LONG;

	return PRINT_FMT_SPEC_LEN_INT;
}

static int print_fmt_spec_get_type(char *s)
{
	switch (s[0]) {
	case 'c':
	case 'd':
	case 'o':
	case 'u':
	case 'x':
	case 'X':
		return PRINT_FMT_SPEC_TYPE_INT;
	case 's':
		return PRINT_FMT_SPEC_TYPE_STRING;
	default:
		break;
	}

	return PRINT_FMT_SPEC_TYPE_VOID;
}

static int print_fmt_get_string(struct evtact_expr *expr, char **fmt)
{
	int ret, fmt_len;
	struct evtact_expr *fmt_expr;

	fmt_expr = list_first_entry_or_null(&expr->opnds, struct evtact_expr, list);
	if (fmt_expr == NULL) {
		pr_err("print() requires at least one argument\n");
		return -EINVAL;
	} else if (!is_const_str_expr(fmt_expr)) {
		pr_err("print() first argument expected to be string\n");
		return -EINVAL;
	}

	if (fmt_expr->ops->eval != NULL) {
		ret = fmt_expr->ops->eval(fmt_expr, NULL, 0, (void **)fmt, &fmt_len);
		if (ret)
			return ret;
	}

	return 0;
}

static void print_fmt_free_specs(struct print_expr_priv *priv)
{
	struct print_fmt_spec *spec, *tmp;

	if (priv == NULL)
		return;

	list_for_each_entry_safe(spec, tmp, &priv->fmt_specs, list) {
		list_del(&spec->list);
		free(spec->str);
		free(spec);
	}
}

static int print_fmt_split(struct evtact_expr *expr, struct print_expr_priv *priv)
{
	int i, ret;
	char *s, *fmt;
	regex_t regex;
	regmatch_t pmatch[6];
	int spec_len, spec_type;
	const char *const re = "%(-?)([0-9]*)(\\.[0-9]+)?(ll|l)?([cdosuxX])";

	ret = print_fmt_get_string(expr, &fmt);
	if (ret)
		return ret;

	if (regcomp(&regex, re, REG_EXTENDED)) {
		pr_err("expr call print fmt regcomp failed\n");
		return -1;
	}

	s = fmt;
	for (i = 0;; i++) {
		if (regexec(&regex, s, ARRAY_SIZE(pmatch), pmatch, 0))
			break;

		spec_len = print_fmt_spec_get_len(s + pmatch[4].rm_so,
						  pmatch[4].rm_eo - pmatch[4].rm_so);
		spec_type = print_fmt_spec_get_type(s + pmatch[5].rm_so);

		ret = print_fmt_add_spec(priv, s, pmatch[0].rm_eo, spec_len, spec_type);
		if (ret)
			goto out_free_specs;

		s += pmatch[0].rm_eo;
	}

	if ((size_t)(s - fmt) < strlen(fmt)) {
		ret = print_fmt_add_spec(priv, s, strlen(fmt) - (s - fmt),
					 PRINT_FMT_SPEC_LEN_INT, PRINT_FMT_SPEC_TYPE_VOID);
		if (ret)
			goto out_free_specs;
	}

	priv->fmt_spec_num = i;
	return 0;

out_free_specs:
	print_fmt_free_specs(priv);
	return ret;
}

static int print_check_args(struct evtact_expr *expr, struct print_expr_priv *priv)
{
	struct evtact_expr *arg;

	priv->args_num = 0;
	list_for_each_entry(arg, &expr->opnds, list)
		priv->args_num++;

	/* do not count format string argument */
	priv->args_num--;

	if (priv->args_num != priv->fmt_spec_num) {
		pr_err("print() arguments number for format string mismatch: %d expected, %d provided\n",
		       priv->fmt_spec_num, priv->args_num);
		return -EINVAL;
	}

	return 0;
}

static int expr_call_print_new(struct evtact_expr *expr,
			       void *data __maybe_unused, int size __maybe_unused)
{
	int ret;
	struct print_expr_priv *priv;

	priv = malloc(sizeof(struct print_expr_priv));
	if (priv == NULL)
		return -ENOMEM;

	priv->out = malloc(PRINT_OUT_BUF_DEFAULT_LEN);
	if (priv->out == NULL) {
		ret = -ENOMEM;
		goto out_free_priv;
	}
	priv->out_len = PRINT_OUT_BUF_DEFAULT_LEN;

	INIT_LIST_HEAD(&priv->fmt_specs);
	ret = print_fmt_split(expr, priv);
	if (ret)
		goto out_free_out_buf;

	ret = print_check_args(expr, priv);
	if (ret)
		goto out_free_specs;

	expr->priv = priv;
	return 0;

out_free_specs:
	print_fmt_free_specs(priv);
out_free_out_buf:
	free(priv->out);
	priv->out_len = 0;
out_free_priv:
	free(priv);
	return ret;
}

static void expr_call_print_free(struct evtact_expr *expr)
{
	struct print_expr_priv *priv;

	priv = expr->priv;
	if (priv == NULL)
		return;

	print_fmt_free_specs(expr->priv);
	zfree(&priv->out);
	priv->out_len = 0;
	zfree(&expr->priv);
}

static int expr_call_print_eval(struct evtact_expr *expr,
				void *in, int in_size,
				void **out __maybe_unused, int *out_size __maybe_unused)
{
	int ret, len;
	char *buf_out;
	int arg_val_size;
	unsigned long long *arg_val;
	struct evtact_expr *arg;
	struct print_fmt_spec *spec;
	struct print_expr_priv *priv = expr->priv;

retry:
	len = 0;
	priv->out[0] = '\0';
	arg = list_first_entry(&expr->opnds, struct evtact_expr, list);
	list_for_each_entry(spec, &priv->fmt_specs, list) {
		if (spec->type == PRINT_FMT_SPEC_TYPE_VOID) {
			len += snprintf(priv->out + len, priv->out_len - len, "%s", spec->str);
		} else {
			arg = list_next_entry(arg, list);
			if (arg == NULL) {
				pr_err("expr call print arguments are empty\n");
				return -EINVAL;
			}

			ret = arg->ops->eval(arg, in, in_size, (void **)&arg_val, &arg_val_size);
			if (ret) {
				pr_err("expr call print eval argument failed %d\n", ret);
				return ret;
			}

			if (spec->type == PRINT_FMT_SPEC_TYPE_STRING) {
				len += snprintf(priv->out + len, priv->out_len - len,
						spec->str, arg_val);
			} else if (spec->type == PRINT_FMT_SPEC_TYPE_INT) {
				switch (spec->len) {
				case PRINT_FMT_SPEC_LEN_INT:
					len += snprintf(priv->out + len, priv->out_len - len,
							spec->str, *(unsigned int *)arg_val);
					break;
				case PRINT_FMT_SPEC_LEN_LONG:
					len += snprintf(priv->out + len, priv->out_len - len,
							spec->str, *(unsigned long *)arg_val);
					break;
				case PRINT_FMT_SPEC_LEN_LONG_LONG:
					len += snprintf(priv->out + len, priv->out_len - len,
							spec->str, *(unsigned long long *)arg_val);
					break;
				default:
					break;
				}
			}
		}

		if (len >= priv->out_len) {
			buf_out = realloc(priv->out, priv->out_len << 1);
			if (buf_out != NULL) {
				priv->out = buf_out;
				priv->out_len <<= 1;
				goto retry;
			}
		}

		if (len >= priv->out_len)
			break;
	}

	printf("%s", priv->out);
	return 0;
}

static struct evtact_expr_ops expr_call_print_ops = {
	.new  = expr_call_print_new,
	.free = expr_call_print_free,
	.eval = expr_call_print_eval,
};

static struct evtact_expr_ops *expr_call_ops_list[EVTACT_EXPR_CALL_TYPE_MAX] = {
	[EVTACT_EXPR_CALL_TYPE_PRINT] = &expr_call_print_ops,
};

static int expr_call_set_ops(struct evtact_expr *expr, u32 opcode)
{
	if (opcode >= EVTACT_EXPR_CALL_TYPE_MAX) {
		pr_err("expr_call opcode invalid: %u\n", opcode);
		return -EINVAL;
	}

	if (expr_call_ops_list[opcode] == NULL) {
		pr_err("expr_call opcode not supported: %u\n", opcode);
		return -ENOTSUP;
	}

	expr->ops = expr_call_ops_list[opcode];
	return 0;
}

static struct evtact_expr_class expr_call = {
	.set_ops = expr_call_set_ops,
};

static struct evtact_expr_class *expr_class_list[EVTACT_EXPR_TYPE_MAX] = {
	[EVTACT_EXPR_TYPE_CONST]   = &expr_const,
	[EVTACT_EXPR_TYPE_CALL]    = &expr_call,
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
