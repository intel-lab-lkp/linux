# SPDX-License-Identifier: GPL-2.0

include ../Makefile.miniconfig

BIN=context_switch_loop
LIB=

all: $(BIN)

$(BIN): $(BIN).c
ifdef CORESIGHT
ifeq ($(ARCH),arm64)
	$(Q)$(CC) $(CFLAGS) $(BIN).c -O0 -o $(BIN) $(LIB)
endif
endif

install-tests: all
ifdef CORESIGHT
ifeq ($(ARCH),arm64)
	$(call QUIET_INSTALL, tests) \
		$(INSTALL) -d -m 755 '$(DESTDIR_SQ)$(perfexec_instdir_SQ)/$(INSTDIR_SUB)/$(BIN)'; \
		$(INSTALL) $(BIN) '$(DESTDIR_SQ)$(perfexec_instdir_SQ)/$(INSTDIR_SUB)/$(BIN)/$(BIN)'
endif
endif

clean:
	$(Q)$(RM) -f $(BIN)

.PHONY: all clean install-tests
