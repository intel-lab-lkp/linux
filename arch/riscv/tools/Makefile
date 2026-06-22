# SPDX-License-Identifier: GPL-2.0

gen := arch/$(ARCH)/include/generated/asm
insn_tbl := $(src)/insn_tbl.sh
insn := $(src)/insn.tbl

gen-y := $(gen)/insn_gen.h

targets += $(addprefix ../../../,$(gen-y))

PHONY += insn

insn:	$(gen-y)

# Create output directory if not already present
$(shell mkdir -p $(gen))

quiet_cmd_insn_tbl = INST_TBL  $@
      cmd_insn_tbl = $(CONFIG_SHELL) $(insn_tbl) $< $@

$(gen)/insn_gen.h: $(insn) $(insn_tbl) FORCE
	$(call if_changed,insn_tbl)
