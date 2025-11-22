// SPDX-License-Identifier: GPL-2.0
#include "dso.h"
#include "libdw.h"
#include "srcline.h"
#include "symbol.h"
#include "dwarf-aux.h"
#include <fcntl.h>
#include <unistd.h>
#include <elfutils/libdwfl.h>

void dso__free_a2l_libdw(struct dso *dso)
{
	Dwfl *dwfl = dso__a2l_libdw(dso);

	if (dwfl) {
		dwfl_end(dwfl);
		dso__set_a2l_libdw(dso, NULL);
	}
}

int libdw__addr2line(const char *dso_name, u64 addr,
		     char **file, unsigned int *line_nr,
		     struct dso *dso, bool unwind_inlines,
		     struct inline_node *node, struct symbol *sym)
{
	static const Dwfl_Callbacks offline_callbacks = {
		.find_debuginfo = dwfl_standard_find_debuginfo,
		.section_address = dwfl_offline_section_address,
		.find_elf = dwfl_build_id_find_elf,
	};
	Dwfl *dwfl = dso__a2l_libdw(dso);
	Dwfl_Module *mod;
	Dwfl_Line *dwline;
	Dwarf_Addr bias;
	const char *src;
	int lineno;

	if (!dwfl) {
		/*
		 * Initialize Dwfl session.
		 * We need to open the DSO file to report it to libdw.
		 */
		int fd;

		fd = open(dso_name, O_RDONLY);
		if (fd < 0)
			return 0;

		dwfl = dwfl_begin(&offline_callbacks);
		if (!dwfl) {
			close(fd);
			return 0;
		}

		/*
		 * If the report is successful, the file descriptor fd is consumed
		 * and closed by the Dwfl. If not, it is not closed.
		 */
		mod = dwfl_report_offline(dwfl, dso_name, dso_name, fd);
		if (!mod) {
			dwfl_end(dwfl);
			close(fd);
			return 0;
		}

		dwfl_report_end(dwfl, /*removed=*/NULL, /*arg=*/NULL);
		dso__set_a2l_libdw(dso, dwfl);
	} else {
		/* Dwfl session already initialized, get module for address. */
		mod = dwfl_addrmodule(dwfl, addr);
	}

	if (!mod)
		return 0;

	/* Find source line information for the address. */
	dwline = dwfl_module_getsrc(mod, addr);
	if (!dwline)
		return 0;

	/* Get line information. */
	src = dwfl_lineinfo(dwline, &addr, &lineno, /*col=*/NULL, /*mtime=*/NULL, /*length=*/NULL);

	if (file)
		*file = src ? strdup(src) : NULL;
	if (line_nr)
		*line_nr = lineno;

	/* Optionally unwind inline function call chain. */
	if (unwind_inlines && node && src) {
		Dwarf_Die *cudie = dwfl_module_addrdie(mod, addr, &bias);
		Dwarf_Die *scopes = NULL;
		int nscopes;

		if (!cudie)
			return 1;

		nscopes = die_get_scopes(cudie, addr, &scopes);
		if (nscopes > 0) {
			int i;
			const char *call_file = src;
			unsigned int call_line = lineno;

			for (i = 0; i < nscopes; i++) {
				Dwarf_Die *die = &scopes[i];
				struct symbol *inline_sym;
				char *srcline = NULL;
				int tag = dwarf_tag(die);

				/* We are interested in inlined subroutines. */
				if (tag != DW_TAG_inlined_subroutine &&
				    tag != DW_TAG_subprogram)
					continue;

				inline_sym = new_inline_sym(dso, sym, dwarf_diename(die));

				if (call_file)
					srcline = srcline_from_fileline(call_file, call_line);

				inline_list__append(inline_sym, srcline, node);

				/* Update call site for next level. */
				if (tag == DW_TAG_inlined_subroutine) {
					call_file = die_get_call_file(die);
					call_line = die_get_call_lineno(die);
				} else {
					/* Reached the root subprogram. */
					break;
				}
			}
			free(scopes);
		}
	}

	return 1;
}
