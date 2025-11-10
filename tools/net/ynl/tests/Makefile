# SPDX-License-Identifier: GPL-2.0
# Makefile for YNL tests

TESTS := \
	test_ynl_cli.sh \
	test_ynl_ethtool.sh \
# end of TESTS

all: $(TESTS)

run_tests:
	@echo "Running YNL tests..."
	@failed=0; \
	echo "Running test_ynl_cli.sh..."; \
	./test_ynl_cli.sh || failed=$$(($$failed + 1)); \
	echo "Running test_ynl_ethtool.sh..."; \
	./test_ynl_ethtool.sh || failed=$$(($$failed + 1)); \
	if [ $$failed -eq 0 ]; then \
		echo "All tests passed!"; \
	else \
		echo "$$failed test(s) failed!"; \
		exit 1; \
	fi

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

.PHONY: all install clean run_tests
