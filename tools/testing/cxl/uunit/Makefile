# SPDX-License-Identifier: GPL-2.0
ROOT_DIR := $(CURDIR)
KERNEL_DIR := ../../../..
UNIT_TEST_DIR := $(ROOT_DIR)/app

V ?= @
.SECONDARY:

# Pull environment (include paths, cpp/c flags) from top-level Makefile
LINUXINCLUDE_STRING    = $(shell make -C $(KERNEL_DIR) -qp | grep "^LINUXINCLUDE :=")
KBUILD_CPPFLAGS_STRING = $(shell make -C $(KERNEL_DIR) -qp | grep "^KBUILD_CPPFLAGS :=")
KBUILD_CFLAGS_STRING   = $(shell make -C $(KERNEL_DIR) -qp | grep "^KBUILD_CFLAGS :=")

$(eval $(subst ./,$(KERNEL_DIR)/,$(LINUXINCLUDE_STRING)))
$(eval $(KBUILD_CPPFLAGS_STRING))
$(eval $(KBUILD_CFLAGS_STRING))

KBUILD_NAME_STRING = -DKBUILD_BASENAME='"uunit"' -DKBUILD_MODNAME='"uunit"' -DKBUILD_MODFILE='"uunit"'

# Add uunit include directories
INCLUDE_FLAGS := -I$(ROOT_DIR)/include -I$(ROOT_DIR)/build/include
# Add kernel root to include directories, so unit tests can include kernel source directly
INCLUDE_FLAGS := $(INCLUDE_FLAGS) -I$(KERNEL_DIR)
# Add stubs for KBUILD defines
INCLUDE_FLAGS := $(INCLUDE_FLAGS) $(KBUILD_NAME_STRING)
# Include kernel build parameters
INCLUDE_FLAGS := $(INCLUDE_FLAGS) $(LINUXINCLUDE) $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS)
# Adjust kernel parameters
#  shadow-call-stack not available in userspace
#  no-frame-larger-than restricts ability to put objects on stack in unit tests
#  -O0 to enable better debuggability
#  -D__NO_FORTIFY since fortify does not work with some compilers with -O0
#  Don't use thunk-extern, fentry, or stack-protector-strong with unit tests
INCLUDE_FLAGS := $(INCLUDE_FLAGS) -fno-sanitize=shadow-call-stack -Wno-frame-larger-than -O0 -D__NO_FORTIFY
INCLUDE_FLAGS := $(subst -mindirect-branch=thunk-extern,,$(INCLUDE_FLAGS))
INCLUDE_FLAGS := $(subst -mfunction-return=thunk-extern,,$(INCLUDE_FLAGS))
INCLUDE_FLAGS := $(subst -fstack-protector-strong,,$(INCLUDE_FLAGS))
INCLUDE_FLAGS := $(subst -mfentry,,$(INCLUDE_FLAGS))
INCLUDE_FLAGS := $(subst -DCC_USING_FENTRY,,$(INCLUDE_FLAGS))

LINK_FLAGS = $(INCLUDE_FLAGS) -no-pie -lcunit

# The following source files are built and linked into each unit test application
# instead of creating stubs for them.
KERNEL_SRC = \
	drivers/base/bus.c \
	drivers/base/driver.c \
	lib/kobject.c \
	lib/klist.c \
	lib/refcount.c \
	lib/xarray.c \
	lib/find_bit.c \
	lib/hweight.c \
	lib/radix-tree.c \
	lib/uuid.c \
	lib/hexdump.c \
	lib/kstrtox.c \
	lib/sort.c

UNIT_TEST_SRC = \
	$(UNIT_TEST_DIR)/core_hdm_ut.c \
	$(UNIT_TEST_DIR)/core_region_ut.c
UNIT_TEST_BIN = $(patsubst $(UNIT_TEST_DIR)/%.c,build/bin/%,$(UNIT_TEST_SRC))
UNIT_TEST_EXEC = $(patsubst build/bin/%,exec_%,$(UNIT_TEST_BIN))

all: $(UNIT_TEST_BIN)

clean:
	$(V)rm -rf build

# compiler_attributes.h uses __attribute__(__error__) for static asserts - this depends
# on the compiler optimizing away the function, so for unit test builds with -O0 we
# create a new version of that header to ensure the attribute doesn't get used.
MODIFIED_HEADERS = build/include/linux/compiler_attributes.h

# We will disable some configuration settings for unit tests, to simplify the unit test
# environment.
MODIFIED_HEADERS += build/include/generated/autoconf.h

# WARN_FLAGS generates constraint errors on x86. So we will modify bug.h slightly to
# workaround this problem.
MODIFIED_HEADERS += build/include/asm-generic/bug.h

build/include/linux/compiler_attributes.h: $(KERNEL_DIR)/include/linux/compiler_attributes.h
	$(V)echo [HEADER] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)sed 's/__has_attribute(__error__)/& \&\& defined(__OPTIMIZE__)/' $< > $@

build/include/generated/autoconf.h: $(KERNEL_DIR)/include/generated/autoconf.h
	$(V)echo [HEADER] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)cp $< $@
	$(V)sed -i 's/#define CONFIG_DYNAMIC_DEBUG 1//' $@
	$(V)sed -i 's/#define CONFIG_DYNAMIC_DEBUG_CORE 1//' $@
	$(V)sed -i 's/#define CONFIG_HAVE_FENTRY 1//' $@
	$(V)sed -i 's/#define CONFIG_LIST_HARDENED 1//' $@
	$(V)sed -i 's/#define CONFIG_PREEMPTION 1//' $@
	$(V)sed -i 's/#define CONFIG_PREEMPT_DYNAMIC 1//' $@
	$(V)sed -i 's/#define CONFIG_DEBUG_PREEMPT 1//' $@

build/include/asm-generic/bug.h: $(KERNEL_DIR)/include/asm-generic/bug.h
	$(V)echo [HEADER] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)cp $< $@
	$(V)sed -i 's/#ifndef __WARN_FLAGS/#undef __WARN_FLAGS\n&/' $@

# Kernel source cannot be compiled directly, it defines functions with slightly different
# signatures than same stdlib functions. So we build them with preprocessor wrappers that
# re-define offending functions to something non-stdlib.
WRAPPED_KERNEL_SRC = $(patsubst %.c,build/wrap/%.c,$(KERNEL_SRC))
WRAPPED_KERNEL_OBJ = $(subst .c,.o,$(WRAPPED_KERNEL_SRC))

build/wrap/%.c: $(MODIFIED_HEADERS) $(KERNEL_DIR)/%.c
	$(V)echo [SRC] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)echo \#include \"pre.h\" > $@
	$(V)echo \#include \"$(KERNEL_DIR)/$*.c\" >> $@
	$(V)echo \#include \"post.h\" >> $@

build/wrap/%.o: build/wrap/%.c Makefile
	$(V)echo [OBJ] $(subst build/,,$@)
	$(V)$(CC) -o $@ -c $< $(INCLUDE_FLAGS)

# Unit test applications include cxl source directly, which requires internal cxl headers.
build/%_ut.o: INCLUDE_FLAGS += -I$(KERNEL_DIR)/drivers/cxl

build/obj/%_ut.o: $(UNIT_TEST_DIR)/%_ut.c $(ROOT_DIR)/stub.c $(MODIFIED_HEADERS)  Makefile
	$(V)echo [OBJ] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)$(CC) -o $@ -c $< $(INCLUDE_FLAGS) -DMODULE

build/bin/%_ut: $(WRAPPED_KERNEL_OBJ) build/obj/%_ut.o
	$(V)echo [LINK] $(subst build/,,$@)
	$(V)mkdir -p $(dir $@)
	$(V)$(CC) -o $@ $^ $(LINK_FLAGS)

exec_%: build/bin/%
	$(V)echo [RUN] $(subst build/,,$^)
	$(V)$^

runtests: $(UNIT_TEST_EXEC)
