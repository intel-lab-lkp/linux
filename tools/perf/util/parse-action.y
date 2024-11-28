%parse-param {struct list_head *actions_head}
%define parse.error verbose

%{

#ifndef NDEBUG
#define YYDEBUG 1
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <linux/compiler.h>
#include <linux/list.h>

#include "util/debug.h"
#include "util/parse-action.h"

#define expr_id(t, o) evtact_expr_id_encode(EVTACT_EXPR_TYPE_##t, EVTACT_EXPR_##t##_TYPE_##o)

int parse_action_lex(void);

static void parse_action_error(struct list_head *expr __maybe_unused,
			       char const *msg)
{
	pr_err("parse_action: %s\n", msg);
}

%}

%union
{
	char *str;
	struct evtact_expr *expr;
	struct list_head *list;
	unsigned long long num;
	u32 opcode;
}

%token IDENT ERROR NUMBER STRING CALL BUILTIN
%token SEMI LP RP COM
%type <expr> action_term expr_term expr_call_term
%destructor { parse_action_expr__free($$); } <expr>
%type <str> IDENT
%type <num> NUMBER
%type <str> STRING
%type <opcode> CALL BUILTIN
%type <list> opnds

%%

actions:
action_term SEMI actions
{
	list_add(&$1->list, actions_head);
}
|
action_term SEMI
{
	list_add(&$1->list, actions_head);
}
|
action_term
{
	list_add(&$1->list, actions_head);
}

action_term:
expr_call_term
{
	$$ = $1;
}

expr_call_term:
CALL LP RP
{
	$$ = parse_action_expr__new(evtact_expr_id_encode(EVTACT_EXPR_TYPE_CALL, $1), NULL, NULL, 0);
	if ($$ == NULL)
		YYERROR;
}
|
CALL LP opnds RP
{
	$$ = parse_action_expr__new(evtact_expr_id_encode(EVTACT_EXPR_TYPE_CALL, $1), $3, NULL, 0);
	if ($$ == NULL)
		YYERROR;
}
|
IDENT LP RP
{
	$$ = NULL;
	pr_err("unknown function '%s()'\n", $1);
	free($1);
	YYERROR;
}
|
IDENT LP opnds RP
{
	$$ = NULL;
	pr_err("unknown function '%s()'\n", $1);
	parse_action_expr__free_opnds($3);
	free($1);
	YYERROR;
}

opnds:
opnds COM expr_term
{
	list_add_tail(&$3->list, $1);
	$$ = $1;
}
|
expr_term
{
	INIT_LIST_HEAD(&$1->list);
	$$ = &$1->list;
}

expr_term:
NUMBER
{
	$$ = parse_action_expr__new(expr_id(CONST, INT), NULL, (void *)&$1, sizeof($1));
	if ($$ == NULL)
		YYERROR;
}
|
STRING
{
	$$ = parse_action_expr__new(expr_id(CONST, STR), NULL, (void *)$1, strlen($1));
	if ($$ == NULL)
		YYERROR;
}
|
BUILTIN
{
	$$ = parse_action_expr__new(evtact_expr_id_encode(EVTACT_EXPR_TYPE_BUILTIN, $1), NULL, NULL, 0);
	if ($$ == NULL)
		YYERROR;
}
|
IDENT
{
	$$ = NULL;
	pr_err("unsupported ident: '%s'\n", $1);
	free($1);
	YYERROR;
}
|
ERROR
{
	$$ = NULL;
	YYERROR;
}

%%
