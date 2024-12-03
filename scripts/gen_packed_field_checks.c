// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024, Intel Corporation
#include <stdbool.h>
#include <stdio.h>

#define MAX_PACKED_FIELD_SIZE 50

int main(int argc, char **argv)
{
	for (int i = 1; i <= MAX_PACKED_FIELD_SIZE; i++) {
		printf("#define CHECK_PACKED_FIELDS_%d(fields) ({ \\\n", i);
		printf("\ttypeof(&(fields)[0]) _f = (fields); \\\n");
		printf("\tBUILD_BUG_ON(ARRAY_SIZE(fields) != %d); \\\n", i);

		for (int j = 0; j < i; j++)
			printf("\tCHECK_PACKED_FIELD(_f[%d]); \\\n", j);

		for (int j = 1; j < i; j++)
			printf("\tCHECK_PACKED_FIELD_OVERLAP(_f[0].startbit < _f[1].startbit, _f[%d], _f[%d]); \\\n",
			       j - 1, j);

		printf("})\n\n");
	}

	printf("#define CHECK_PACKED_FIELDS(fields) \\\n");

	for (int i = 1; i <= MAX_PACKED_FIELD_SIZE; i++)
		printf("\t__builtin_choose_expr(ARRAY_SIZE(fields) == %d, CHECK_PACKED_FIELDS_%d(fields), \\\n",
		       i, i);

	printf("\t({ BUILD_BUG_ON_MSG(1, \"CHECK_PACKED_FIELDS() must be regenerated to support array sizes larger than %d.\"); }) \\\n",
	       MAX_PACKED_FIELD_SIZE);

	for (int i = 1; i <= MAX_PACKED_FIELD_SIZE; i++)
		printf(")");

	printf("\n");
}
