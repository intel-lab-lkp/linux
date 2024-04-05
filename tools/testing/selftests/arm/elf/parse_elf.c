// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Limited
 *
 * Author : Dev Jain <dev.jain@arm.com>
 *
 * Parse elf header to confirm 32-bit process
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <elf.h>
#include <stdint.h>

#include <kselftest.h>

/* The ELF file header.  This appears at the start of every ELF file. */

struct elf_header {
	unsigned char	e_ident[16];	/* Magic number and other info */
	uint16_t	e_type;		/* Object file type */
	uint16_t	e_machine;	/* Architecture */
	uint32_t	e_version;	/* Object file version */
	uint64_t	e_entry;	/* Entry point virtual address */
	uint64_t	e_phoff;	/* Program header table file offset */
	uint64_t	e_shoff;	/* Section header table file offset */
	uint32_t	e_flags;	/* Processor-specific flags */
	uint16_t	e_ehsize;	/* ELF header size in bytes */
	uint16_t	e_phentsize;	/* Program header table entry size */
	uint16_t	e_phnum;	/* Program header table entry count */
	uint16_t	e_shentsize;	/* Section header table entry size */
	uint16_t	e_shnum;	/* Section header table entry count */
	uint16_t	e_shstrndx;	/* Section header string table index */
};

static int read_elf_header(const char  *elfFile)
{
	struct elf_header header;
	FILE *file;

	file = fopen(elfFile, "r");
	if (file) {

		/* store header in struct */
		fread(&header, 1, sizeof(header), file);
		fclose(file);

		/* sanity check: does it really follow ELF format */
		if (header.e_ident[0] == 0x7f &&
		    header.e_ident[1] == 'E' &&
		    header.e_ident[2] == 'L' &&
		    header.e_ident[3] == 'F') {
			if (header.e_ident[4] == 0x01)
				return 0;
			return 1;
		}
		ksft_exit_fail_msg("Cannot parse /proc/self/exe\n");
	}
	ksft_exit_fail_msg("Cannot open /proc/self/exe\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[])
{
	const char *file_name;

	ksft_print_header();
	ksft_set_plan(1);

	file_name = "/proc/self/exe";
	ksft_test_result(read_elf_header(file_name) == 0, "ELF is 32 bit\n");
	ksft_finished();
}
