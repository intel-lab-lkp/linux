// SPDX-License-Identifier: GPL-2.0
#include <stdbool.h>
#include <stdio.h>

static bool generate_checks[51];

static void parse_defines(void)
{
#ifdef PACKING_CHECK_FIELDS_1
	generate_checks[1] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_2
	generate_checks[2] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_3
	generate_checks[3] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_4
	generate_checks[4] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_5
	generate_checks[5] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_6
	generate_checks[6] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_7
	generate_checks[7] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_8
	generate_checks[8] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_9
	generate_checks[9] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_10
	generate_checks[10] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_11
	generate_checks[11] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_12
	generate_checks[12] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_13
	generate_checks[13] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_14
	generate_checks[14] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_15
	generate_checks[15] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_16
	generate_checks[16] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_17
	generate_checks[17] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_18
	generate_checks[18] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_19
	generate_checks[19] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_20
	generate_checks[20] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_21
	generate_checks[21] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_22
	generate_checks[22] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_23
	generate_checks[23] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_24
	generate_checks[24] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_25
	generate_checks[25] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_26
	generate_checks[26] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_27
	generate_checks[27] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_28
	generate_checks[28] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_29
	generate_checks[29] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_30
	generate_checks[30] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_31
	generate_checks[31] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_32
	generate_checks[32] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_33
	generate_checks[33] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_34
	generate_checks[34] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_35
	generate_checks[35] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_36
	generate_checks[36] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_37
	generate_checks[37] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_38
	generate_checks[38] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_39
	generate_checks[39] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_40
	generate_checks[40] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_41
	generate_checks[41] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_42
	generate_checks[42] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_43
	generate_checks[43] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_44
	generate_checks[44] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_45
	generate_checks[45] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_46
	generate_checks[46] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_47
	generate_checks[47] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_48
	generate_checks[48] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_49
	generate_checks[49] = true;
#endif
#ifdef PACKING_CHECK_FIELDS_50
	generate_checks[50] = true;
#endif
}

int main(int argc, char **argv)
{
	parse_defines();

	printf("/* Automatically generated - do not edit */\n\n");
	printf("#ifndef GENERATED_PACKING_CHECKS_H\n");
	printf("#define GENERATED_PACKING_CHECKS_H\n\n");

	for (int i = 1; i <= 50; i++) {
		if (!generate_checks[i])
			continue;

		printf("#define CHECK_PACKED_FIELDS_%d(fields, pbuflen) \\\n", i);
		printf("\t({ typeof(&(fields)[0]) _f = (fields); typeof(pbuflen) _len = (pbuflen); \\\n");
		printf("\tBUILD_BUG_ON(ARRAY_SIZE(fields) != %d); \\\n", i);
		for (int j = 0; j < i; j++) {
			bool final = (i == 1);

			printf("\tCHECK_PACKED_FIELD(_f[%d], _len);%s\n",
			       j, final ? " })\n" : " \\");
		}
		for (int j = 1; j < i; j++) {
			for (int k = 0; k < j; k++) {
				bool final = (j == i - 1) && (k == j - 1);

				printf("\tCHECK_PACKED_FIELD_OVERLAP(_f[%d], _f[%d]);%s\n",
				       k, j, final ? " })\n" : " \\");
			}
		}
	}

	printf("#endif /* GENERATED_PACKING_CHECKS_H */\n");
}
