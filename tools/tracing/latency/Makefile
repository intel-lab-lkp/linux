# SPDX-License-Identifier: GPL-2.0
export srctree := $(abspath ../../..)
export CC      := gcc
export LD      := ld
export AR      := ar
export PKG_CONFIG := pkg-config

FEATURE_TESTS := libtraceevent
FEATURE_TESTS += libtracefs
FEATURE_DISPLAY := libtraceevent
FEATURE_DISPLAY += libtracefs

latency-collector:

include $(srctree)/tools/build/Makefile.include
include $(srctree)/tools/build/Makefile.feature
include $(srctree)/tools/scripts/Makefile.include
include Makefile.config

CFLAGS += $(INCLUDES) $(LIB_INCLUDES)

export CFLAGS := $(CFLAGS)

latency-collector: latency-collector-in.o
	$(QUIET_LINK)$(CC) $(LDFLAGS) -o $@ $^ $(EXTLIBS)

latency-collector.%: fixdep FORCE
	make -f $(srctree)/tools/build/Makefile.build dir=. $@

latency-collector-in.o: fixdep FORCE
	make $(build)=latency-collector

clean:
	$(call QUIET_CLEAN, latency-collector)
	@find . -name '*.o' -delete -o -name '\.*.cmd' -delete -o -name '\.*.d' -delete
	@rm -f latency-collector FEATURE-DUMP

.PHONY: FORCE clean
