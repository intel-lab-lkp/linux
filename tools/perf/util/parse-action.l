%option prefix="parse_action_"
%option noyywrap
%option stack

%{

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "util/debug.h"

#include "parse-action.h"
#include "parse-action-bison.h"

static int value(int base)
{
	unsigned long long num;

	errno = 0;
	num = strtoul(parse_action_text, NULL, base);
	if (errno) {
		pr_err("parse_action malloc number failed\n");
		return ERROR;
	}

	parse_action_lval.num = num;
	return NUMBER;
}

%}

num_dec		[0-9]+
num_hex		0[xX][0-9a-fA-F]+
space		[ \t]
ident		[_a-zA-Z][_a-zA-Z0-9]*

%%

{num_dec}	{ return value(10); }
{num_hex}	{ return value(16); }
{space}		{ }

";"		{ return SEMI; }

{ident}		{
			parse_action_lval.str = strdup(parse_action_text);
			if (parse_action_lval.str == NULL) {
				pr_err("parse_action malloc ident string failed\n");
				return ERROR;
			}
			return IDENT;
		}
.		{
			pr_err("invalid character: '%s'\n", parse_action_text);
			return ERROR;
		}

%%
