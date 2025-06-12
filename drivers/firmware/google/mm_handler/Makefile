# SPDX-License-Identifier: GPL-2.0

## Based on realmode/rm/Makefile

always-y := handler.bin handler.relocs

handler-y			+= mm_header.o
handler-y			+= mm_handler.o

targets	+= $(handler-y)

REALMODE_OBJS = $(addprefix $(obj)/,$(handler-y))

sed-pasyms := -n -r -e 's/^([0-9a-fA-F]+) [ABCDGRSTVW] (.+)$$/pa_\2 = \2;/p'

quiet_cmd_pasyms = PASYMS  $@
      cmd_pasyms = $(NM) $(real-prereqs) | sed $(sed-pasyms) | sort | uniq > $@

targets += pasyms.h
$(obj)/pasyms.h: $(REALMODE_OBJS) FORCE
	$(call if_changed,pasyms)

targets += handler.lds
$(obj)/handler.lds: $(obj)/pasyms.h

LDFLAGS_handler.elf := -m elf_i386 --emit-relocs -T
CPPFLAGS_handler.lds += -P -C -I$(objtree)/$(obj)

targets += handler.elf
$(obj)/handler.elf: $(obj)/handler.lds $(REALMODE_OBJS) FORCE
	$(call if_changed,ld)

OBJCOPYFLAGS_handler.bin := -O binary

targets += handler.bin
$(obj)/handler.bin: $(obj)/handler.elf $(obj)/handler.relocs FORCE
	$(call if_changed,objcopy)

quiet_cmd_relocs = RELOCS  $@
      cmd_relocs = arch/x86/tools/relocs --realmode $< > $@

targets += handler.relocs
$(obj)/handler.relocs: $(obj)/handler.elf FORCE
	$(call if_changed,relocs)

# ---------------------------------------------------------------------------

KBUILD_CFLAGS	:= $(REALMODE_CFLAGS) -D_SETUP -D_WAKEUP \
		   -I$(srctree)/arch/x86/boot
KBUILD_AFLAGS	:= $(KBUILD_CFLAGS) -D__ASSEMBLY__
KBUILD_CFLAGS	+= -fno-asynchronous-unwind-tables
