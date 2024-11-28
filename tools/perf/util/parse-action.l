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

static char *str_buf;
static unsigned int str_buf_size = 64;
static int str_begin(void)
{
	if (str_buf == NULL) {
		str_buf = malloc(str_buf_size);
		if (str_buf == NULL) {
			pr_err("parse_action malloc string buffer failed\n");
			return ERROR;
		}
	}

	str_buf[0] = '\0';
	return 0;
}

static int str_write(const char *s)
{
	char *new_buf;

	if (strlen(str_buf) + strlen(s) >= str_buf_size) {
		str_buf_size = strlen(str_buf) + strlen(s) + 1;
		new_buf = realloc(str_buf, str_buf_size);
		if (new_buf == NULL) {
			free(str_buf);
			str_buf = NULL;
			pr_err("parse_action realloc string buffer failed\n");
			return ERROR;
                } else {
			str_buf = new_buf;
		}
	}

	strcat(str_buf, s);
	return 0;
}

static int str_end(void)
{
	parse_action_lval.str = strdup(str_buf);
	if (parse_action_lval.str == NULL) {
		pr_err("parse_action strdup string buffer failed\n");
		return ERROR;
	}

	return STRING;
}

static int call(u32 opcode)
{
	parse_action_lval.opcode = opcode;
	return CALL;
}

static int builtin(u32 opcode)
{
	parse_action_lval.opcode = opcode;
	return BUILTIN;
}

%}

num_dec		[0-9]+
num_hex		0[xX][0-9a-fA-F]+
space		[ \t]
ident		[_a-zA-Z][_a-zA-Z0-9]*

%x STR_BEGIN

%%

{num_dec}	{ return value(10); }
{num_hex}	{ return value(16); }
{space}		{ }

cpu		{ return builtin(EVTACT_EXPR_BUILTIN_TYPE_CPU);  }
pid		{ return builtin(EVTACT_EXPR_BUILTIN_TYPE_PID);  }

print		{ return call(EVTACT_EXPR_CALL_TYPE_PRINT); }

";"		{ return SEMI; }
"("		{ return LP;   }
")"		{ return RP;   }
","		{ return COM;  }

\"		{
			int ret;
			yy_push_state(STR_BEGIN);
			ret = str_begin();
			if (ret) {
				yy_pop_state();
				return ret;
			}
		}
<STR_BEGIN>{
  \"		{
			yy_pop_state();
			return str_end();
		}
  [^\\\n\"]+	{
			int ret;
			ret = str_write((const char *)yytext);
			if (ret) {
				yy_pop_state();
				return ret;
			}
		}
  \\n		{
			int ret;
			ret = str_write("\n");
			if (ret) {
				yy_pop_state();
				return ret;
                        }
                }
  \\t           {
			int ret;
			ret = str_write("\t");
			if (ret) {
				yy_pop_state();
				return ret;
			}
		}
  \\\\		{
			int ret;
			ret = str_write("\\");
			if (ret) {
				yy_pop_state();
				return ret;
			}
		}
  \\.		{
			yy_pop_state();
			pr_err("parse_action invalid escape character: '%s'\n", parse_action_text);
			return ERROR;
		}
  .		{
			yy_pop_state();
			pr_err("parse_action invalid character: '%s'\n", parse_action_text);
			return ERROR;
		}
}

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
