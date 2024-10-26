# SPDX-License-Identifier: GPL-2.0
#
# Kbuild for top-level directory of the kernel

# Prepare global headers and check sanity before descending into sub-directories
# ---------------------------------------------------------------------------

# Generate bounds.h

bounds-file := include/generated/bounds.h

targets := kernel/bounds.s

$(bounds-file): kernel/bounds.s FORCE
	$(call filechk,offsets,__LINUX_BOUNDS_H__)

# Generate timeconst.h

timeconst-file := include/generated/timeconst.h

filechk_gentimeconst = echo $(CONFIG_HZ) | bc -q $<

$(timeconst-file): kernel/time/timeconst.bc FORCE
	$(call filechk,gentimeconst)

# Generate asm-offsets.h

offsets-file := include/generated/asm-offsets.h

targets += arch/$(SRCARCH)/kernel/asm-offsets.s

arch/$(SRCARCH)/kernel/asm-offsets.s: $(timeconst-file) $(bounds-file)

$(offsets-file): arch/$(SRCARCH)/kernel/asm-offsets.s FORCE
	$(call filechk,offsets,__ASM_OFFSETS_H__)

ifdef CONFIG_PACKING_CHECK_FIELDS

# Generate packing-checks.h

ifdef CONFIG_PACKING_CHECK_FIELDS_1
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_1
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_2
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_2
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_3
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_3
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_4
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_4
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_5
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_5
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_6
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_6
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_7
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_7
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_8
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_8
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_9
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_9
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_10
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_10
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_11
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_11
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_12
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_12
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_13
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_13
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_14
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_14
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_15
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_15
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_16
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_16
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_17
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_17
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_18
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_18
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_19
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_19
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_20
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_20
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_21
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_21
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_22
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_22
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_23
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_23
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_24
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_24
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_25
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_25
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_26
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_26
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_27
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_27
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_28
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_28
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_29
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_29
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_30
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_30
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_31
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_31
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_32
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_32
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_33
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_33
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_34
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_34
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_35
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_35
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_36
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_36
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_37
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_37
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_38
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_38
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_39
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_39
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_40
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_40
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_41
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_41
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_42
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_42
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_43
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_43
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_44
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_44
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_45
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_45
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_46
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_46
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_47
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_47
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_48
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_48
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_49
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_49
endif
ifdef CONFIG_PACKING_CHECK_FIELDS_50
HOSTCFLAGS_lib/gen_packing_checks.o += -DPACKING_CHECK_FIELDS_50
endif

hostprogs += lib/gen_packing_checks

packing-checks := include/generated/packing-checks.h

filechk_gen_packing_checks = lib/gen_packing_checks

$(packing-checks): lib/gen_packing_checks FORCE
	$(call filechk,gen_packing_checks)

endif

# Check for missing system calls

quiet_cmd_syscalls = CALL    $<
      cmd_syscalls = $(CONFIG_SHELL) $< $(CC) $(c_flags) $(missing_syscalls_flags)

PHONY += missing-syscalls
missing-syscalls: scripts/checksyscalls.sh $(offsets-file)
	$(call cmd,syscalls)

# Check the manual modification of atomic headers

quiet_cmd_check_sha1 = CHKSHA1 $<
      cmd_check_sha1 = \
	if ! command -v sha1sum >/dev/null; then \
		echo "warning: cannot check the header due to sha1sum missing"; \
		exit 0; \
	fi; \
	if [ "$$(sed -n '$$s:// ::p' $<)" != \
	     "$$(sed '$$d' $< | sha1sum | sed 's/ .*//')" ]; then \
		echo "error: $< has been modified." >&2; \
		exit 1; \
	fi; \
	touch $@

atomic-checks += $(addprefix $(obj)/.checked-, \
	  atomic-arch-fallback.h \
	  atomic-instrumented.h \
	  atomic-long.h)

targets += $(atomic-checks)
$(atomic-checks): $(obj)/.checked-%: include/linux/atomic/%  FORCE
	$(call if_changed,check_sha1)

# A phony target that depends on all the preparation targets

PHONY += prepare
prepare: $(offsets-file) missing-syscalls $(atomic-checks) $(packing-checks)
	@:

# Ordinary directory descending
# ---------------------------------------------------------------------------

obj-y			+= init/
obj-y			+= usr/
obj-y			+= arch/$(SRCARCH)/
obj-y			+= $(ARCH_CORE)
obj-y			+= kernel/
obj-y			+= certs/
obj-y			+= mm/
obj-y			+= fs/
obj-y			+= ipc/
obj-y			+= security/
obj-y			+= crypto/
obj-$(CONFIG_BLOCK)	+= block/
obj-$(CONFIG_IO_URING)	+= io_uring/
obj-$(CONFIG_RUST)	+= rust/
obj-y			+= $(ARCH_LIB)
obj-y			+= drivers/
obj-y			+= sound/
obj-$(CONFIG_SAMPLES)	+= samples/
obj-$(CONFIG_NET)	+= net/
obj-y			+= virt/
obj-y			+= $(ARCH_DRIVERS)
