#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate riscv instruction helper header.
# The generated helpers for each instruction are:
#   - riscv_insn_<insn>_MASK useful to help check if arbitrary binary is <insn>
#   - riscv_insn_<insn>_MATCH useful to help check if arbitrary binary is <insn>
#   - riscv_insn_<insn> useful to construct <insn>
#   - riscv_insn_<insn>_<var> useful to extract <var> from <insn>
#
# Each line of the instruction table should have the following format:
# NAME BASE FIXED_BITS [VARIABLE_LIST]
# NAME                        instruction name
# BASE                        instruction base size (common[,(32|64)])
#                             "common" means the instruction is valid on both architecture sizes
#                             "32" or "64" means the instruction is valid on that size
#                             if the instruction is valid on both architectures but has unique
#                             encodings, use a 32-bit entry "common,32" and 64-bit entry
# FIXED_BITS                  bitfields of the fixed bits of an instruction concatenated with '|'
#                             continuous grouping of fixed bits are in the form of 'bits<offset'
# VARIABLE_LIST               a variable sized list of all variables in the instruction definition
#                             in the form of name[~][<num][!num...]=(high[-low])|...
#                             symbols after the name represent different modifiers:
#                                 ~ sign extension, can only appear once
#                                 < left shift by 'num' amount on extraction, can only appear once
#                                 ! mark 'num' as an invalid input for this variable#

set -e

usage() {
	echo >&2 "usage: $0 BASE INFILE OUTFILE" >&2
	echo >&2
	echo >&2 "  INFILE    input instruction table"
	echo >&2 "  OUTFILE   output header file"
	exit 1
}

if [ $# -ne 2 ]; then
	usage
fi

infile="$1"
outfile="$2"

file=$(readlink -f $0)

echo "/* Auto-generated rv${base} header from script arch/${file#*arch/} */" > $outfile

echo "#ifndef RISCV_INSN_GEN_H" >> $outfile
echo "#define RISCV_INSN_GEN_H" >> $outfile
echo >> $outfile

printf "#include <linux/bits.h>" >> $outfile
echo >> $outfile

grep -E "^[a-z\.0-9]+[[:space:]]+" "$infile" | {
    while read name base fixed variables; do
        echo "/* $name */"

        non_compressed_insn=${name##c.*}
        invalid_inst_functions=""
        variable_params=""
        constraints=""
        match=""
        mask=""
        make=""

        # All compressed instructions start with "c."
        size=${non_compressed_insn:+32};
        size=${size:-16};

        # Replace all . with _
        formatted_inst_name=$name
        while [ ! ${formatted_inst_name##*.*} ]; do
            prefix=${formatted_inst_name%.*}
            suffix=${formatted_inst_name##*.}
            contains_dot=${formatted_inst_name##*.*}
            formatted_inst_name=${contains_dot:-${prefix}_${suffix}}
        done

        # Collect all fixed bits of an instruction
        OLD_IFS=$IFS
        IFS='|'
        for segment in $fixed; do
            bits=${segment%<*}
            offset=${segment#*<}

            len=${#bits}

            mask="${mask} | 0b"

            while [ $len -gt 0 ]; do
                len=$((len - 1))
                mask=${mask}1
            done

            if [ ${offset} -gt 0 ]; then
                s=" << ${offset}"
            else
                s=""
            fi

            mask="${mask}${s}"

            match="${match} | 0b${bits}${s}"
        done
        IFS=$OLD_IFS

        # Instruction only appears in one base
        only_base=
        if [ "${base}" != "${base%32}" ]; then
            echo "#if __riscv_xlen == 32"
            only_base=32
        elif [ "${base}" != "${base%64}" ]; then
            echo "#if __riscv_xlen == 64"
            only_base=64
        fi

        # Standard name for the instruction parameter in generated functions
        insn="_insn"

        for variable in ${variables}; do
            variable_name="${variable%%[<~=!]*}"
            parts="${variable#*=}"
            insert_mask=""
            sign_extend=""
            left_shift=""
            extract=""
            insert=""

            # Standard name for the variable parameter in generated functions
            var="_${variable_name}"
            variable_params="${variable_params}u32 ${var}, "

            if [ "${variable}" != "${variable#*~}" ]; then
                sign_extend="1"
            fi

            if [ "${variable}" != "${variable#*<}" ]; then
                left_shift="${variable#*<}"
                left_shift="${left_shift%%[=<~!]*}"
            else
                left_shift="0"
            fi

            if [ "${variable}" != "${variable#*!}" ]; then
                raw_constraints="${variable#*!}"
                raw_constraints="${raw_constraints%%[=<~!]**}"

                OLD_IFS=$IFS
                IFS='!'
                for constraint in $raw_constraints; do
                    constraints="${constraints}(riscv_insn_${formatted_inst_name}_extract_${variable_name}(${insn}) != ${constraint}) && "
                done
                IFS=$OLD_IFS
            fi

            offset=0
            while true; do
                part=${parts##*|}

                if [ "${part#*-}" = "${part}" ]; then
                    high="${part}"
                    low="${part}"
                    len=1
                else
                    high="${part%-*}"
                    low="${part#*-}"
                    len=$((high - low + 1))
                fi

                # Don't emit shift if 0
                first_shift=${low}
                if [ "${first_shift}" = "0" ]; then
                    first_shift=
                fi

                second_shift=$((offset + left_shift))
                if [ "${second_shift}" = "0" ]; then
                    second_shift=
                fi

                extract="${extract} | ((${insn}${first_shift:+ >> }${first_shift} & GENMASK($((len - 1)), 0))${second_shift:+ << }${second_shift})"
                insert_mask="${insert_mask} & ~GENMASK(${high}, ${low})"
                insert="${insert} | (((${var}${second_shift:+ >> }${second_shift}) & GENMASK($((len - 1)), 0))${first_shift:+ << }${first_shift})"
                offset=$((offset + len))

                if [ "${parts}" = "${part}" ]; then
                    # Processed all parts of variable
                    break
                fi

                parts=${parts%|*}
            done

            extract="${extract# | }"

            if [ ${sign_extend} ]; then
                extract="sign_extend32(${extract}, $((offset + left_shift - 1)))"
                type="s"
            else
                type="u"
            fi

            printf "static __always_inline ${type}${size} riscv_insn_${formatted_inst_name}_extract_${variable_name}(u${size} ${insn})\n"
            printf "{\n"
            printf "\treturn ${extract};\n"
            printf "}\n"
            printf "static __always_inline void riscv_insn_${formatted_inst_name}_insert_${variable_name}(u${size} *${insn}, ${type}32 ${var})\n"
            printf "{\n"
            printf "\t*_insn &= ${insert_mask# & };\n"
            printf "\t*_insn |= ${insert# | };\n"
            printf "}\n"

            if [ "${only_base}" ]; then
                invalid_inst_functions="${invalid_inst_functions}static __always_inline ${type}${size} riscv_insn_${formatted_inst_name}_extract_${variable_name}(u${size} ${insn}) {\n\tBUILD_BUG_ON_MSG(1, \"${name} is not supported on non ${only_base}-bit systems.\");\n}\n"
            fi

            make="${make}	riscv_insn_${formatted_inst_name}_insert_${variable_name}(&${insn}, ${var});\n"
        done

        variable_params="${variable_params%, }"
        variable_params="${variable_params:-void}"

        printf "#define riscv_insn_${formatted_inst_name}_MASK (${mask# | })\n"
        printf "#define riscv_insn_${formatted_inst_name}_MATCH (${match# | })\n"
        printf "static __always_inline u${size} riscv_insn_${formatted_inst_name}(${variable_params})\n"
        printf "{\n"
        printf "\tu${size} ${insn} = riscv_insn_${formatted_inst_name}_MATCH;\n"
        printf "${make}	return ${insn};\n"
        printf "}\n"

        # Check against instructions that have a variable that may contain invalid values
        if [ "$constraints" ]; then
            printf "__RISCV_INSN_FUNCS_CONSTRAINED(${formatted_inst_name}, ${constraints% && });\n"
        else
            printf "__RISCV_INSN_FUNCS(${formatted_inst_name});\n"
        fi

        # If common does not appear in the base, then this instruction only appears in one base
        if [ "$base" = "${base#common}" ]; then
            printf "#else\n"
            printf "__RISCV_INSN_FUNCS_UNSUPPORTED(${formatted_inst_name});\n"
            printf "${invalid_inst_functions%\\n}\n"
        fi

        # Instruction has a base variant
        if [ "$base" != "${base%[24]}" ]; then
            echo "#endif"
        fi

        echo
    done

    echo "#endif /* RISCV_INST_GEN_H */"
} >> $outfile
