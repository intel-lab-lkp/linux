# SPDX-License-Identifier: GPL-2.0
#
# Note: To build this you must install libnl-3 and libnl-genl-3 development
# packages.
remove-ip:
	gcc -I/usr/include/libnl3 \
	    -I../../../../usr/include \
	    remove-ip.c \
	    -o remove-ip \
	    -lnl-genl-3 \
	    -lnl-3

.PHONY: all
all: remove-ip

.PHONY: clean
clean:
	rm remove-ip
