// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>

int main(int argc, char **argv)
{
	printf("/* Automatically generated - do not edit */\n\n");
	printf("#ifndef GENERATED_PACKING_CHECKS_H\n");
	printf("#define GENERATED_PACKING_CHECKS_H\n\n");

	for (int i = 1; i <= 50; i++) {
		printf("#define CHECK_PACKED_FIELDS_%d(fields, pbuflen) \\\n", i);
		printf("\t({ typeof(&(fields)[0]) _f = (fields); typeof(pbuflen) _len = (pbuflen); \\\n");
		printf("\tBUILD_BUG_ON(ARRAY_SIZE(fields) != %d); \\\n", i);
		for (int j = 0; j < i; j++) {
			int final = (i == 1);

			printf("\tCHECK_PACKED_FIELD(_f[%d], _len);%s\n",
			       j, final ? " })\n" : " \\");
		}
		for (int j = 1; j < i; j++) {
			for (int k = 0; k < j; k++) {
				int final = (j == i - 1) && (k == j - 1);

				printf("\tCHECK_PACKED_FIELD_OVERLAP(_f[%d], _f[%d]);%s\n",
				       k, j, final ? " })\n" : " \\");
			}
		}
	}

	printf("#endif /* GENERATED_PACKING_CHECKS_H */\n");
}
