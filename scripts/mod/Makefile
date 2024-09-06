# SPDX-License-Identifier: GPL-2.0
CFLAGS_REMOVE_empty.o += $(CC_FLAGS_LTO)

hostprogs-always-y	+= modpost mk_elfconfig
always-y		+= empty.o

modpost-objs	:= modpost.o file2alias.o sumversion.o symsearch.o

devicetable-offsets-file := devicetable-offsets.h

HOSTCFLAGS_file2alias.o += -D_UUID_T -D__GETHOSTUUID_H
HOSTCFLAGS_modpost.o = $(shell $(HOSTPKG_CONFIG) --cflags bee-headers 2> /dev/null)
HOSTCFLAGS_file2alias.o = $(shell $(HOSTPKG_CONFIG) --cflags bee-headers 2> /dev/null)
HOSTCFLAGS_sumversion.o = $(shell $(HOSTPKG_CONFIG) --cflags bee-headers 2> /dev/null)
HOSTCFLAGS_symsearch.o = $(shell $(HOSTPKG_CONFIG) --cflags bee-headers 2> /dev/null)
HOSTCFLAGS_mk_elfconfig.o = $(shell $(HOSTPKG_CONFIG) --cflags bee-headers 2> /dev/null)

$(obj)/$(devicetable-offsets-file): $(obj)/devicetable-offsets.s FORCE
	$(call filechk,offsets,__DEVICETABLE_OFFSETS_H__)

targets += $(devicetable-offsets-file) devicetable-offsets.s

# dependencies on generated files need to be listed explicitly

$(obj)/modpost.o $(obj)/file2alias.o $(obj)/sumversion.o $(obj)/symsearch.o: $(obj)/elfconfig.h
$(obj)/file2alias.o: $(obj)/$(devicetable-offsets-file)

quiet_cmd_elfconfig = MKELF   $@
      cmd_elfconfig = $(obj)/mk_elfconfig < $< > $@

$(obj)/elfconfig.h: $(obj)/empty.o $(obj)/mk_elfconfig FORCE
	$(call if_changed,elfconfig)

targets += elfconfig.h
