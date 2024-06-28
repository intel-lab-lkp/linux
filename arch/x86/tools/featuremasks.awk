#!/usr/bin/awk
#
# Convert cpufeatures.h to a list of compile-time masks
# Note: this blithly assumes that each word has at least one
# feature defined in it; if not, something else is wrong!
#

BEGIN {
	printf "#ifndef _ASM_X86_FEATUREMASKS_H\n";
	printf "#define _ASM_X86_FEATUREMASKS_H\n\n";

	file = 0
}

BEGINFILE {
	switch (++file) {
	case 1:			# cpufeatures.h
		FPAT = "#[ \t]*[a-z]+|[A-Za-z0-9_]+|[^ \t]";
		break;
	case 2:			# .config
		FPAT = "CONFIG_[A-Z0-9_]+|is not set|[yn]";
		break;
	}
}

# Create a dictionary of sorts, containing all defined feature bits
file == 1 && $1 ~ /^#[ \t]*define$/ && $2 ~ /^X86_FEATURE_/ &&
$3 == "(" && $5 == "*" && $7 == "+" && $9 == ")" {
	nfeat = $4 * $6 + $8;
	feat = $2;
	sub(/^X86_FEATURE_/, "", feat);
	feats[nfeat] = feat;
}
file == 1 && $1 ~ /^#[ \t]*define$/ && $2 == "NCAPINTS" {
	ncapints = strtonum($3);
}

# Create a dictionary featstat[REQUIRED|DISABLED, FEATURE_NAME] = on | off
file == 2 && $1 ~ /^CONFIG_X86_[A-Z]*_FEATURE_/ {
	on = ($2 == "y");
	if (split($1, fs, "CONFIG_X86_|_FEATURE_") == 3)
		featstat[fs[2], fs[3]] = on;
}

END {
	sets[1] = "REQUIRED";
	sets[2] = "DISABLED";

	for (ns in sets) {
		s = sets[ns];

		printf "/*\n";
		printf " * %s features:\n", s;
		printf " *\n";
		fstr = "";
		for (i = 0; i < ncapints; i++) {
			mask = 0;
			for (j = 0; j < 32; j++) {
				nfeat = i*32 + j;
				feat = feats[nfeat];
				if (feat) {
					st = !!featstat[s, feat];
					if (st) {
						nfstr = fstr " " feat;
						if (length(nfstr) > 72) {
							printf " *   %s\n", fstr;
							nfstr = " " feat;
						}
						fstr = nfstr;
						mask += (2 ^ j);
					}
				}
			}
			masks[i] = mask;
		}
		printf " *   %s\n */\n", fstr;

		for (i = 0; i < ncapints; i++)
			printf "#define %s_MASK%d\t0x%08xU\n", s, i, masks[i];

		printf "#define %s_MASK_CHECK BUILD_BUG_ON_ZERO(NCAPINTS != %d)\n\n", s, ncapints;
	}

	printf "#endif /* _ASM_X86_FEATUREMASKS_H */\n";
}
