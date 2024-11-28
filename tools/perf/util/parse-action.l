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

%}

space		[ \t]
ident		[_a-zA-Z][_a-zA-Z0-9]*

%%

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
