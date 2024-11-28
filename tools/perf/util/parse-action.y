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
}

%token IDENT ERROR
%token SEMI
%type <expr> action_term expr_term
%destructor { parse_action_expr__free($$); } <expr>
%type <str> IDENT

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
expr_term
{
	$$ = $1;
}

expr_term:
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
