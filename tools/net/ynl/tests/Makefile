# SPDX-License-Identifier: GPL-2.0
# Makefile for YNL tests

TESTS := \
	test_ynl_cli.sh \
	test_ynl_ethtool.sh \
# end of TESTS

all: $(TESTS)

install: $(TESTS)
	@mkdir -p $(DESTDIR)/usr/bin
	@for test in $(TESTS); do \
		name=$$(basename $$test .sh); \
		sed -e 's|^ynl=.*|ynl="ynl"|' \
		    -e 's|^ynl_ethtool=.*|ynl_ethtool="ynl-ethtool"|' \
		    $$test > $(DESTDIR)/usr/bin/$$name; \
		chmod +x $(DESTDIR)/usr/bin/$$name; \
	done

clean:
	@# Nothing to clean

.PHONY: all install clean
