# SPDX-License-Identifier: GPL-2.0
# Kselftest Makefile for devmem test

CFLAGS += -Wall -O2

TEST_GEN_PROGS_EXTENDED := devmem

$(OUTPUT)/devmem: devmem.c $(OUTPUT)/ram_map.o $(OUTPUT)/secret.o  $(OUTPUT)/tests.o $(OUTPUT)/utils.o $(OUTPUT)/debug.o
	$(CC) $^ -o $@ $(CFLAGS)

EXTRA_CLEAN += $(OUTPUT)/ram_map.o $(OUTPUT)/secret.o  $(OUTPUT)/tests.o $(OUTPUT)/utils.o $(OUTPUT)/debug.o

include ../lib.mk
