# SPDX-License-Identifier: GPL-2.0
export srctree	:= $(abspath ../../..)

VERSION		:= $(shell sh -c "make -sC ../../.. kernelversion | grep -v make")
DOCSRC		:= ../../../Documentation/tools/rv/

FEATURE_TESTS	:= libtraceevent
FEATURE_TESTS	+= libtracefs
FEATURE_DISPLAY	:= libtraceevent
FEATURE_DISPLAY	+= libtracefs

ifeq ($(V),1)
  Q =
else
  Q = @
endif

rv:

include $(srctree)/tools/build/Makefile.include
include $(srctree)/tools/build/Makefile.feature
include $(srctree)/tools/scripts/Makefile.include
include Makefile.rv
include Makefile.config

CFLAGS		+= $(INCLUDES) $(LIB_INCLUDES)
export CFLAGS	:= $(CFLAGS)

rv: rv-in.o
	$(QUIET_LINK)$(CC) $(LDFLAGS) -o $@ $^ $(EXTLIBS)

static: rv-in.o
	$(eval LDFLAGS += -static)
	$(QUIET_LINK)$(CC) $(LDFLAGS) -o rv-static $^ $(EXTLIBS)

rv.%: fixdep FORCE
	make -f $(srctree)/tools/build/Makefile.build dir=. $@

rv-in.o: fixdep FORCE
	make $(build)=rv

clean: doc_clean fixdep-clean
	$(call QUIET_CLEAN, rv)
	$(Q)find . -name '*.o' -delete -o -name '\.*.cmd' -delete -o -name '\.*.d' -delete
	$(Q)rm -f rv rv-static FEATURE-DUMP rv-*
.PHONY: FORCE clean
