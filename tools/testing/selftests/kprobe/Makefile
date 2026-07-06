# SPDX-License-Identifier: GPL-2.0
#
# Makefile for kprobe fault stress test
#

ifneq ($(KERNELRELEASE),)

# This part is used by kernel Kbuild when building the external module:
#
#   make -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
#
obj-m += kprobe_folio_stress.o

ccflags-y += -g -O0 -fno-omit-frame-pointer

else

# This part is used when running make directly in this directory.
#
#   make KDIR=$BUILD_DIR ARCH=$ARCH CROSS_COMPILE=$CROSS_COMPILE all
#

PWD				:= $(shell pwd)
ARCH			?= arm64
CROSS_COMPILE	?= aarch64-dumpstack-linux-gnu-
KDIR			?= /lib/modules/$(shell uname -r)/build
DEST_PATH		?= $(PWD)/out
Q				?= @

# Kernel module
KP_MOD			:= kprobe_folio_stress
KP_KO			:= $(KP_MOD).ko

# Userspace test program
UNIT_TEST		:= fault_stress
UNIT_TEST_SRC	:= fault_stress.c

# Userspace CFLAGS.
# These options make userspace stacks easier to unwind.
USER_CFLAGS		:= -static -g -O0 -fno-omit-frame-pointer -fasynchronous-unwind-tables
USER_LIBS		:= -lm -lpthread

.PHONY: all modules user install clean

all: modules user

modules:
	$(Q)$(MAKE) -C $(KDIR) \
		M=$(PWD) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		modules

user:
	$(Q)$(CROSS_COMPILE)gcc \
		$(USER_CFLAGS) \
		$(UNIT_TEST_SRC) \
		-o $(UNIT_TEST) \
		$(USER_LIBS)

install: all
	$(Q)mkdir -p $(DEST_PATH)
	$(Q)cp -f $(KP_KO) $(DEST_PATH)/
	$(Q)cp -f $(UNIT_TEST) $(DEST_PATH)/

clean:
	$(Q)$(MAKE) -C $(KDIR) \
		M=$(PWD) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		clean
	$(Q)rm -f $(UNIT_TEST)
	$(Q)rm -rf $(DEST_PATH)

endif
