# SPDX-License-Identifier: GPL-2.0
export srctree	:= $(abspath ../../..)

VERSION		:= $(shell sh -c "make -sC ../../.. kernelversion | grep -v make")
DOCSRC		:= ../../../Documentation/tools/rtla/

FEATURE_TESTS	:= libtraceevent
FEATURE_TESTS	+= libtracefs
FEATURE_DISPLAY	:= libtraceevent
FEATURE_DISPLAY	+= libtracefs

ifeq ($(V),1)
  Q =
else
  Q = @
endif

rtla:

include $(srctree)/tools/build/Makefile.include
include $(srctree)/tools/build/Makefile.feature
include $(srctree)/tools/scripts/Makefile.include
include Makefile.rtla
include Makefile.config

CFLAGS		+= $(INCLUDES) $(LIB_INCLUDES)
export CFLAGS	:= $(CFLAGS)

rtla: rtla-in.o
	$(QUIET_LINK)$(CC) $(LDFLAGS) -o $@ $^ $(EXTLIBS)

static: rtla-in.o
	$(eval LDFLAGS += -static)
	$(QUIET_LINK)$(CC) -static $(LDFLAGS) -o rtla-static $^ $(EXTLIBS)

rtla.%: fixdep FORCE
	make -f $(srctree)/tools/build/Makefile.build dir=. $@

rtla-in.o: fixdep FORCE
	make $(build)=rtla

clean: doc_clean fixdep-clean
	$(call QUIET_CLEAN, rtla)
	$(Q)find . -name '*.o' -delete -o -name '\.*.cmd' -delete -o -name '\.*.d' -delete
	$(Q)rm -f rtla rtla-static FEATURE-DUMP rtla-*
.PHONY: FORCE clean
