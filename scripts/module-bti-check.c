// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fix cross section AArch64 calls whose targets lack BTI landing pads.
 *
 * This utility scans for R_ARCH64_CALL26 cross section calls whose targets
 * lack a BTI compatible landing pad. It places a veneer in the target
 * function's section, redirects the relocation to the veneer, and uses a
 * direct branch from the veneer to the original function entry point.
 *
 * This can't fix text sections where the target is more than 128M away.
 * The scanner detects those cases and fails. Making the target non static
 * causes the compiler to emit a suitable landing pad.
 */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <gelf.h>
#include <getopt.h>
#include <libelf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tools/be_byteshift.h>
#include <tools/le_byteshift.h>

#define BTI_MASK	0xffffff3f
#define BTI_INSN	0xd503241f
#define BTI_C_INSN	0xd503245f
#define PACIASP_INSN	0xd503233f
#define PACIBSP_INSN	0xd503237f
#define B_INSN		0x14000000
#define VENEER_SIZE	8	    /* Two instructions, BTI C, B Original */

struct object {
	const char *name;
	Elf *elf;
	Elf_Data *symdata;
	Elf_Data *xndxdata;
	GElf_Shdr symshdr;
	size_t shnum;
	size_t shstrndx;
	bool changed;
};

struct function {
	const char *name;
	uint64_t value;
	unsigned int section_index;
};

struct location {
	uint64_t value;
	uint32_t branch;
	unsigned int section_index;
	unsigned int secsym;
};

struct veneer {
	struct veneer *next;
	uint64_t target;
	uint64_t value;
	unsigned int section_index;
	unsigned int secsym;
	uint32_t insns[VENEER_SIZE / sizeof(uint32_t)];
};

static int elf_error(const struct object *obj)
{
	fprintf(stderr, "%s: %s\n", obj->name, elf_errmsg(-1));

	return -ELIBBAD;
}

static bool get_sym(const struct object *obj, size_t ndx, GElf_Sym *sym,
			    unsigned int *section_index)
{
	Elf32_Word xndx;

	if (!gelf_getsymshndx(obj->symdata, obj->xndxdata, ndx, sym, &xndx))
		return false;

	*section_index = sym->st_shndx == SHN_XINDEX ? xndx : sym->st_shndx;

	return true;
}

static const char *section_name(const struct object *obj, unsigned int ndx)
{
	GElf_Shdr shdr;
	Elf_Scn *scn;
	const char *name = NULL;

	scn = elf_getscn(obj->elf, ndx);
	if (!scn)
		goto out;

	if (!gelf_getshdr(scn, &shdr))
		goto out;

	name = elf_strptr(obj->elf, obj->shstrndx, shdr.sh_name);

out:
	return name ? name : "<invalid>";
}

static const char *symbol_name(const struct object *obj, const GElf_Sym *sym)
{
	const char *name;

	name = elf_strptr(obj->elf, obj->symshdr.sh_link, sym->st_name);

	return name ? name : "<invalid>";
}

static int init_object(struct object *obj)
{
	GElf_Ehdr ehdr;
	size_t symtab = 0;
	size_t i;

	if (elf_kind(obj->elf) != ELF_K_ELF)
		return -ENOEXEC;

	if (gelf_getclass(obj->elf) != ELFCLASS64)
		return -ENOEXEC;

	if (!gelf_getehdr(obj->elf, &ehdr))
		goto out;

	if (elf_getshdrnum(obj->elf, &obj->shnum) < 0)
		goto out;

	if (elf_getshdrstrndx(obj->elf, &obj->shstrndx) < 0)
		goto out;

	if (ehdr.e_type != ET_REL || ehdr.e_machine != EM_AARCH64)
		return -ENOEXEC;

	for (i = 1; i < obj->shnum; i++) {
		Elf_Scn *scn;
		GElf_Shdr shdr;

		scn = elf_getscn(obj->elf, i);
		if (!scn)
			goto out;

		if (!gelf_getshdr(scn, &shdr))
			goto out;

		if (shdr.sh_type != SHT_SYMTAB)
			continue;

		if (symtab)
			return -ENOEXEC;

		symtab = i;
		obj->symshdr = shdr;
		obj->symdata = elf_getdata(scn, NULL);
		if (!obj->symdata)
			goto out;
	}

	if (!symtab || !obj->symshdr.sh_entsize ||
	    obj->symshdr.sh_size % obj->symshdr.sh_entsize)
		return -ENOEXEC;

	for (i = 1; i < obj->shnum; i++) {
		Elf_Scn *scn;
		GElf_Shdr shdr;

		scn = elf_getscn(obj->elf, i);
		if (!scn)
			goto out;

		if (!gelf_getshdr(scn, &shdr))
			goto out;

		if (shdr.sh_type == SHT_SYMTAB_SHNDX && shdr.sh_link == symtab) {
			/* duplicate SHT_SYMTAB_SHNDX?! */
			if (obj->xndxdata)
				return -ENOEXEC;

			obj->xndxdata = elf_getdata(scn, NULL);
			if (!obj->xndxdata)
				goto out;
		}
	}

	return 0;
out:
	return elf_error(obj);
}

static bool executable_section(const struct object *obj, unsigned int ndx)
{
	GElf_Shdr shdr;
	Elf_Scn *scn = elf_getscn(obj->elf, ndx);

	if (ndx == SHN_UNDEF || !scn)
		return false;

	return gelf_getshdr(scn, &shdr) && (shdr.sh_flags & SHF_EXECINSTR);
}

static int get_first_insn(const struct object *obj, const struct function *func,
			  uint32_t *insn)
{
	Elf_Scn *scn;
	Elf_Data *data;
	const void *p;

	scn = elf_getscn(obj->elf, func->section_index);
	if (!scn)
		return -ENOEXEC;

	data = elf_getdata(scn, NULL);
	if (!data)
		return -ENOEXEC;

	if (func->value > data->d_size ||
	    sizeof(*insn) > data->d_size - func->value)
		return -ENOEXEC;

	p = (char *)data->d_buf + func->value;
	*insn = get_unaligned_le32(p);
	return 0;
}

static void put_insn(void *p, uint32_t insn)
{
	put_unaligned_le32(insn, p);
}

/* Is the target instruction a valid BTI landing pad? */
static bool is_landing_pad(uint32_t insn)
{
	return (insn & BTI_MASK) == BTI_INSN || insn == PACIASP_INSN ||
	       insn == PACIBSP_INSN;
}

static bool find_function(const struct object *obj, unsigned int section_index,
			  uint64_t value, bool exact, struct function *func)
{
	size_t count = obj->symshdr.sh_size / obj->symshdr.sh_entsize, i;
	bool found = false;

	for (i = 0; i < count; i++) {
		GElf_Sym sym;
		unsigned int symsec;

		if (!get_sym(obj, i, &sym, &symsec))
			continue;

		if (symsec != section_index)
			continue;

		if (GELF_ST_TYPE(sym.st_info) != STT_FUNC)
			continue;

		if (value < sym.st_value)
			continue;

		if (exact && sym.st_value != value)
			continue;

		if (!exact && sym.st_size && value - sym.st_value >= sym.st_size)
			continue;

		if (found && sym.st_value < func->value)
			continue;

		func->name = symbol_name(obj, &sym);
		func->value = sym.st_value;
		func->section_index = section_index;
		found = true;
	}
	return found;
}

/*
 * Resolve the relocation target as a section-relative offset.
 * Treat invalid targets as unresolved.
 */
static bool relocation_target(const struct object *obj, const GElf_Rela *rela,
			     struct function *func)
{
	GElf_Sym sym;
	unsigned int section_index;
	uint64_t value;

	if (!get_sym(obj, GELF_R_SYM(rela->r_info), &sym, &section_index))
		return false;

	if (!executable_section(obj, section_index))
		return false;

	/* CALL26 relocates to symbol + addend */
	if (rela->r_addend >= 0) {
		if (sym.st_value > UINT64_MAX - (uint64_t)rela->r_addend)
			return false;
		value = sym.st_value + rela->r_addend;
	} else {
		uint64_t magnitude = (uint64_t)(-(rela->r_addend + 1)) + 1;

		if (sym.st_value < magnitude)
			return false;
		value = sym.st_value - magnitude;
	}

	if (!find_function(obj, section_index, value, false, func))
		func->name = symbol_name(obj, &sym);

	func->value = value;
	func->section_index = section_index;

	return true;
}

static bool encode_branch(uint64_t from, uint64_t target, uint32_t *insn_position)
{
	int64_t delta;

	if ((from | target) & 3)
		return false;

	delta = (int64_t)(target - from);

	if (delta < -(1LL << 27) || delta >= (1LL << 27))
		return false;

	*insn_position = B_INSN | ((delta >> 2) & 0x03ffffff);
	return true;
}

static int find_possible_location(const struct object *obj,
			 const struct function *target, struct location *loc)
{
	size_t count = obj->symshdr.sh_size / obj->symshdr.sh_entsize, i;
	GElf_Shdr shdr;
	Elf_Scn *scn = elf_getscn(obj->elf, target->section_index);

	/* Keep the veneer and direct branch in the callee's section. */
	if (!scn || !gelf_getshdr(scn, &shdr) || shdr.sh_type != SHT_PROGBITS ||
	    (shdr.sh_size & 3) || shdr.sh_size > UINT64_MAX - VENEER_SIZE)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		GElf_Sym sym;
		unsigned int section_index;

		if (get_sym(obj, i, &sym, &section_index)
		    && section_index == target->section_index
		    && GELF_ST_TYPE(sym.st_info) == STT_SECTION)
			break;
	}

	if (i == count)
		return 1;

	loc->value = shdr.sh_size;
	loc->section_index = target->section_index;
	loc->secsym = i;

	if (!encode_branch(loc->value + VENEER_SIZE - 4, target->value,
			   &loc->branch))
		return 1;

	return 0;
}

static int insert_veneer(struct object *obj, const struct location *loc,
			 uint64_t target, struct veneer **result)
{
	Elf_Scn *scn;
	Elf_Data *data;
	GElf_Shdr shdr;
	struct veneer *veneer;

	veneer = calloc(1, sizeof(*veneer));
	if (!veneer)
		return -ENOMEM;

	veneer->target = target;
	veneer->value = loc->value;
	veneer->section_index = loc->section_index;
	veneer->secsym = loc->secsym;

	put_insn(&veneer->insns[0], BTI_C_INSN);
	put_insn(&veneer->insns[1], loc->branch);

	scn = elf_getscn(obj->elf, loc->section_index);
	if (!scn || !gelf_getshdr(scn, &shdr))
		goto out;

	data = elf_newdata(scn);
	if (!data)
		goto out;

	/* Libelf references this buffer until elf_end(), so the veneer owns it. */
	data->d_buf = veneer->insns;
	data->d_type = ELF_T_BYTE;
	data->d_size = VENEER_SIZE;
	data->d_align = 4;
	data->d_version = EV_CURRENT;
	shdr.sh_size += VENEER_SIZE;

	if (!gelf_update_shdr(scn, &shdr)) {
		data->d_buf = NULL;
		goto out;
	}

	obj->changed = true;
	*result = veneer;

	return 0;
out:
	free(veneer);
	return elf_error(obj);
}

static int get_veneer(struct object *obj, struct veneer **veneers,
		      const struct function *target, struct veneer **result)
{
	struct veneer *veneer;
	struct location loc;
	int ret = EXIT_SUCCESS;

	for (veneer = *veneers; veneer; veneer = veneer->next) {
		if (veneer->section_index == target->section_index &&
		    veneer->target == target->value) {
			*result = veneer;
			goto out;
		}
	}

	/* no existing veneer found, create one */
	ret = find_possible_location(obj, target, &loc);
	if (ret) {
		fprintf(stderr, "%s: cannot place BTI veneer for %s\n",
			obj->name, target->name);
		goto out;
	}

	ret = insert_veneer(obj, &loc, target->value, &veneer);
	if (ret)
		goto out;

	veneer->next = *veneers;
	*veneers = veneer;

	*result = veneer;

	fprintf(stderr, "%s: grew %s for BTI veneer to %s at +0x%llx; "
		"B-to-target distance is %lld bytes\n", obj->name,
		section_name(obj, target->section_index), target->name,
		(unsigned long long)loc.value,
		(long long)(target->value - (loc.value + 4)));

out:
	return ret;
}

static int scan_relocations(struct object *obj, struct veneer **veneers, bool fix)
{
	size_t i;
	int unfixed = 0;
	int ret = EXIT_SUCCESS;

	for (i = 1; i < obj->shnum; i++) {
		Elf_Scn *scn = elf_getscn(obj->elf, i);
		Elf_Data *data;
		GElf_Shdr shdr;
		size_t j, count;

		if (!scn || !gelf_getshdr(scn, &shdr))
			goto out_elferr;

		if (shdr.sh_type != SHT_RELA || !executable_section(obj, shdr.sh_info))
			continue;

		data = elf_getdata(scn, NULL);
		if (!data || !shdr.sh_entsize || shdr.sh_size % shdr.sh_entsize) {
			ret = -ENOEXEC;
			goto out;
		}

		count = shdr.sh_size / shdr.sh_entsize;
		for (j = 0; j < count; j++) {
			GElf_Rela rela;
			struct function target, caller;
			struct veneer *veneer;
			const char *source;
			uint32_t insn;

			if (!gelf_getrela(data, j, &rela))
				goto out_elferr;

			if (GELF_R_TYPE(rela.r_info) != R_AARCH64_CALL26)
				continue;

			if (!relocation_target(obj, &rela, &target))
				continue;

			if (target.section_index == shdr.sh_info)
				continue;

			ret = get_first_insn(obj, &target, &insn);
			if (ret)
				goto out;

			if (is_landing_pad(insn))
				continue;

			if (find_function(obj, shdr.sh_info, rela.r_offset,
					  false, &caller))
				source = caller.name;
			else
				source = "<unknown>";

			if (fix) {
				ret = get_veneer(obj, veneers, &target, &veneer);
				if (ret)
					goto out;

				rela.r_info = GELF_R_INFO(veneer->secsym, R_AARCH64_CALL26);
				rela.r_addend = veneer->value;
				if (!gelf_update_rela(data, j, &rela))
					return elf_error(obj);

				obj->changed = true;
				continue;
			}

			fprintf(stderr,
				"%s: cross-section call from %s%s%s+0x%llx to %s (%s+0x%llx) lacks a BTI landing pad\n",
				obj->name, source ?: "", source ? " " : "",
				section_name(obj, shdr.sh_info),
				(unsigned long long)rela.r_offset, target.name,
				section_name(obj, target.section_index),
				(unsigned long long)target.value);

			unfixed++;
		}
	}

	ret = unfixed;
out:
	return ret;
out_elferr:
	return elf_error(obj);
}

static int process_file(const char *name, bool fix)
{
	struct object obj = { .name = name };
	struct veneer *veneers = NULL;
	int fd = -1, ret;


	fd = open(name, fix ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	obj.elf = elf_begin(fd, fix ? ELF_C_RDWR : ELF_C_READ, NULL);
	if (!obj.elf) {
		fprintf(stderr, "%s: %s\n", name, elf_errmsg(-1));
		ret = -ELIBBAD;
		goto out;
	}

	ret = init_object(&obj);
	if (ret)
		goto out;

	ret = scan_relocations(&obj, &veneers, fix);
	if (!ret && fix && obj.changed) {
		if (elf_update(obj.elf, ELF_C_WRITE) < 0)
			ret = elf_error(&obj);
	}


out:
	if (obj.elf) {
		if (elf_end(obj.elf) < 0) {
			fprintf(stderr, "%s: %s\n", name, elf_errmsg(-1));
			ret = -ELIBBAD;
		}
	}

	/* Must happen after elf end, to assure correctness */
	while (veneers) {
		struct veneer *next = veneers->next;

		free(veneers);
		veneers = next;
	}

	if (fd >= 0)
		close(fd);


	if (ret < 0) {
		if (ret != -ELIBBAD)
			fprintf(stderr, "%s: %s\n", name, strerror(-ret));
		return EXIT_FAILURE;
	}

	return ret;
}


int main(int argc, char **argv)
{
	bool warn = false;
	bool fix = false;
	int option, i, ret = EXIT_FAILURE;

	static const struct option options[] = {
		{ "fix", no_argument, NULL, 'f' },
		{ "warn", no_argument, NULL, 'w' },
		{ }
	};

	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		switch (option) {
		case 'f':
			fix = true;
			break;
		case 'w':
			warn = true;
			break;
		default:
			goto out;
		}
	}
	if (optind == argc) {
		fprintf(stderr, "Usage: %s [--fix] [--warn] <module.ko>...\n",
			argv[0]);
		goto out;
	}

	if (elf_version(EV_CURRENT) == EV_NONE) {
		fprintf(stderr, "libelf initialization failed: %s\n", elf_errmsg(-1));
		goto out;
	}

	for (i = optind; i < argc; i++) {
		int status = process_file(argv[i], fix);

		if (status && !warn) {
			ret = status;
			goto out;
		}
	}

	ret = EXIT_SUCCESS;
out:
	return ret;
}
