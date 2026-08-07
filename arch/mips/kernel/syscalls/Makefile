# SPDX-License-Identifier: GPL-2.0
kapi := arch/$(SRCARCH)/include/generated/asm
uapi := arch/$(SRCARCH)/include/generated/uapi/asm

$(shell mkdir -p $(uapi) $(kapi))

syshdr := $(srctree)/scripts/syscallhdr.sh
sysnr := $(srctree)/scripts/syscallnr.sh
systbl := $(srctree)/scripts/syscalltbl.sh
systbl_common := $(srctree)/scripts/syscall_common.tbl

quiet_cmd_syshdr = SYSHDR  $@
      cmd_syshdr = $(CONFIG_SHELL) $(syshdr) --offset __NR_Linux \
		   --common-tbl $(systbl_common) \
		   --abis $(abis_$*) $< $@

quiet_cmd_sysnr = SYSNR   $@
      cmd_sysnr = $(CONFIG_SHELL) $(sysnr) \
		  --prefix $(sysnr_pfx_$(basetarget))_Linux_ \
		   --abis $(abis_$*) $< $@

quiet_cmd_systbl = SYSTBL  $@
      cmd_systbl = $(CONFIG_SHELL) $(systbl) \
		   --common-tbl $(systbl_common) \
		   --abis $(abis_$*) $(no_compat_$*) $< $@

$(uapi)/unistd_%.h: $(src)/syscall_%.tbl $(syshdr) FORCE
	$(call if_changed,syshdr)

sysnr_pfx_unistd_nr_n32 := N32
sysnr_pfx_unistd_nr_n64 := 64
sysnr_pfx_unistd_nr_o32 := O32

abis := common,__cln3

abis_n32 := $(abis),32,n32
abis_o32 := $(abis),32,o32
abis_n64 := $(abis),64,n64

no_compat_n64 := --no-compat

$(kapi)/unistd_nr_%.h: $(systbl_common) $(sysnr) FORCE
	$(call if_changed,sysnr)

$(kapi)/syscall_table_%.h: $(src)/syscall_%.tbl $(systbl) FORCE
	$(call if_changed,systbl)

uapisyshdr-y		+= unistd_n32.h			\
			   unistd_n64.h			\
			   unistd_o32.h
kapisyshdr-y		+= syscall_table_n32.h		\
			   syscall_table_n64.h		\
			   syscall_table_o32.h		\
			   unistd_nr_n32.h		\
			   unistd_nr_n64.h		\
			   unistd_nr_o32.h

uapisyshdr-y	:= $(addprefix $(uapi)/, $(uapisyshdr-y))
kapisyshdr-y	:= $(addprefix $(kapi)/, $(kapisyshdr-y))
targets		+= $(addprefix ../../../../, $(uapisyshdr-y) $(kapisyshdr-y))

PHONY += all
all: $(uapisyshdr-y) $(kapisyshdr-y)
	@:
