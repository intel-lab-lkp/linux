TESTMODS_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= /lib/modules/$(shell uname -r)/build
CONFIG_LIVEPATCH := $(shell cd $(KDIR) && scripts/config --state CONFIG_LIVEPATCH)

obj-m += test_klp_atomic_replace.o \
	test_klp_callbacks_busy.o \
	test_klp_callbacks_demo.o \
	test_klp_callbacks_demo2.o \
	test_klp_callbacks_mod.o \
	test_klp_livepatch.o \
	test_klp_state.o \
	test_klp_state2.o \
	test_klp_state3.o \
	test_klp_shadow_vars.o \
	test_klp_syscall.o


# Ensure that KDIR exists and CONFIG_LIVEPATCH is enabled, else skip compilation
modules:
ifneq ("$(wildcard $(KDIR))", "")
ifneq ($(filter y m,$(CONFIG_LIVEPATCH)),)
	$(Q)$(MAKE) -C $(KDIR) modules KBUILD_EXTMOD=$(TESTMODS_DIR)
else
    $(warning CONFIG_LIVEPATCH is not enabled in the kernel config file.)
endif
endif

# Ensure that KDIR exists, otherwise skip the clean target
clean:
ifneq ("$(wildcard $(KDIR))", "")
	$(Q)$(MAKE) -C $(KDIR) clean KBUILD_EXTMOD=$(TESTMODS_DIR)
endif
