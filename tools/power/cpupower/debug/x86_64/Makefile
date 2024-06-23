# SPDX-License-Identifier: GPL-2.0
OUTPUT=./
ifeq ("$(origin O)", "command line")
	OUTPUT := $(O)/
endif

DESTDIR =
bindir  = /usr/bin

INSTALL = /usr/bin/install


default: all

$(OUTPUT)centrino-decode: ../i386/centrino-decode.c
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) $<

$(OUTPUT)powernow-k8-decode: ../i386/powernow-k8-decode.c
	$(CC) $(CFLAGS) -o $@ $(LDFLAGS) $<

all: $(OUTPUT)centrino-decode $(OUTPUT)powernow-k8-decode

clean:
	rm -rf $(OUTPUT)centrino-decode $(OUTPUT)powernow-k8-decode

install:
	$(INSTALL) -d $(DESTDIR)${bindir}
	$(INSTALL) $(OUTPUT)centrino-decode $(DESTDIR)${bindir}
	$(INSTALL) $(OUTPUT)powernow-k8-decode $(DESTDIR)${bindir}

help:
	@echo  'Build targets:'
	@echo  '  all		  - Default target. Could be omitted. Put build artifacts'
	@echo  '                    to "O" cmdline option dir (default: current dir)'
	@echo  '  install	  - Install previously built project files from the output'
	@echo  '                    dir defined by "O" cmdline option (default: current dir)'
	@echo  '                    to the install dir  defined by "DESTDIR" cmdline'
	@echo  ''
	@echo  'Clean targets:'
	@echo  '  clean		  - Clean build artifacts from the dir defined by "O" cmdline'
	@echo  '                    option (default: current dir)'

.PHONY: all default clean install
