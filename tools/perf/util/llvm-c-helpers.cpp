// SPDX-License-Identifier: GPL-2.0

/*
 * Must come before the linux/compiler.h include, which defines several
 * macros (e.g. noinline) that conflict with compiler builtins used
 * by LLVM.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"  /* Needed for LLVM <= 15 */
#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Object/Binary.h>
#pragma GCC diagnostic pop

#include <stdio.h>
#include <sys/types.h>
#include <linux/compiler.h>
extern "C" {
#include <linux/zalloc.h>
}
#include <vector>
#include <algorithm>
#include "symbol_conf.h"
#include "llvm-c-helpers.h"

extern "C"
char *dso__demangle_sym(struct dso *dso, int kmodule, const char *elf_name);

using namespace llvm;
using namespace llvm::object;
using llvm::symbolize::LLVMSymbolizer;

/*
 * Allocate a static LLVMSymbolizer, which will live to the end of the program.
 * Unlike the bfd paths, LLVMSymbolizer has its own cache, so we do not need
 * to store anything in the dso struct.
 */
static LLVMSymbolizer *get_symbolizer()
{
	static LLVMSymbolizer *instance = nullptr;
	if (instance == nullptr) {
		LLVMSymbolizer::Options opts;
		/*
		 * LLVM sometimes demangles slightly different from the rest
		 * of the code, and this mismatch can cause new_inline_sym()
		 * to get confused and mark non-inline symbol as inlined
		 * (since the name does not properly match up with base_sym).
		 * Thus, disable the demangling and let the rest of the code
		 * handle it.
		 */
		opts.Demangle = false;
		instance = new LLVMSymbolizer(opts);
	}
	return instance;
}

/* Returns 0 on error, 1 on success. */
static int extract_file_and_line(const DILineInfo &line_info, char **file,
				 unsigned int *line)
{
	if (file) {
		if (line_info.FileName == "<invalid>") {
			/* Match the convention of libbfd. */
			*file = nullptr;
		} else {
			/* The caller expects to get something it can free(). */
			*file = strdup(line_info.FileName.c_str());
			if (*file == nullptr)
				return 0;
		}
	}
	if (line)
		*line = line_info.Line;
	return 1;
}

extern "C"
int llvm_addr2line(const char *dso_name, u64 addr,
		   char **file, unsigned int *line,
		   bool unwind_inlines,
		   llvm_a2l_frame **inline_frames)
{
	LLVMSymbolizer *symbolizer = get_symbolizer();
	object::SectionedAddress sectioned_addr = {
		addr,
		object::SectionedAddress::UndefSection
	};

	if (unwind_inlines) {
		Expected<DIInliningInfo> res_or_err =
			symbolizer->symbolizeInlinedCode(dso_name,
							 sectioned_addr);
		if (!res_or_err)
			return 0;
		unsigned num_frames = res_or_err->getNumberOfFrames();
		if (num_frames == 0)
			return 0;

		if (extract_file_and_line(res_or_err->getFrame(0),
					  file, line) == 0)
			return 0;

		*inline_frames = (llvm_a2l_frame *)calloc(
			num_frames, sizeof(**inline_frames));
		if (*inline_frames == nullptr)
			return 0;

		for (unsigned i = 0; i < num_frames; ++i) {
			const DILineInfo &src = res_or_err->getFrame(i);

			llvm_a2l_frame &dst = (*inline_frames)[i];
			if (src.FileName == "<invalid>")
				/* Match the convention of libbfd. */
				dst.filename = nullptr;
			else
				dst.filename = strdup(src.FileName.c_str());
			dst.funcname = strdup(src.FunctionName.c_str());
			dst.line = src.Line;

			if (dst.filename == nullptr ||
			    dst.funcname == nullptr) {
				for (unsigned j = 0; j <= i; ++j) {
					zfree(&(*inline_frames)[j].filename);
					zfree(&(*inline_frames)[j].funcname);
				}
				zfree(inline_frames);
				return 0;
			}
		}

		return num_frames;
	} else {
		if (inline_frames)
			*inline_frames = nullptr;

		Expected<DILineInfo> res_or_err =
			symbolizer->symbolizeCode(dso_name, sectioned_addr);
		if (!res_or_err)
			return 0;
		return extract_file_and_line(*res_or_err, file, line);
	}
}

static char *
make_symbol_relative_string(struct dso *dso, const char *sym_name,
			    u64 addr, u64 base_addr)
{
	if (!strcmp(sym_name, "<invalid>"))
		return NULL;

	char *demangled = dso__demangle_sym(dso, 0, sym_name);
	if (base_addr && base_addr != addr) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s+0x%lx",
			 demangled ? demangled : sym_name, addr - base_addr);
		free(demangled);
		return strdup(buf);
	} else {
		if (demangled)
			return demangled;
		else
			return strdup(sym_name);
	}
}

extern "C"
char *llvm_name_for_code(struct dso *dso, const char *dso_name, u64 addr)
{
	LLVMSymbolizer *symbolizer = get_symbolizer();
	object::SectionedAddress sectioned_addr = {
		addr,
		object::SectionedAddress::UndefSection
	};
	Expected<DILineInfo> res_or_err =
		symbolizer->symbolizeCode(dso_name, sectioned_addr);
	if (!res_or_err) {
		return NULL;
	}
	return make_symbol_relative_string(
		dso, res_or_err->FunctionName.c_str(),
		addr, res_or_err->StartAddress ? *res_or_err->StartAddress : 0);
}

extern "C"
char *llvm_name_for_data(struct dso *dso, const char *dso_name, u64 addr)
{
	LLVMSymbolizer *symbolizer = get_symbolizer();
	object::SectionedAddress sectioned_addr = {
		addr,
		object::SectionedAddress::UndefSection
	};
	Expected<DIGlobal> res_or_err =
		symbolizer->symbolizeData(dso_name, sectioned_addr);
	if (!res_or_err) {
		return NULL;
	}
	return make_symbol_relative_string(
		dso, res_or_err->Name.c_str(),
		addr, res_or_err->Start);
}

int llvm_load_symbols(const char *debugfile, struct llvm_symbol_list *symbols)
{
	/* NOTE: This nominally does an mmap, despite the scary name. */
	ErrorOr<std::unique_ptr<MemoryBuffer>> mem_buf_or_err =
		MemoryBuffer::getFile(debugfile);
	if (mem_buf_or_err.getError())
		return -1;

	Expected<std::unique_ptr<Binary>> binary_or_err(
		createBinary(mem_buf_or_err.get()->getMemBufferRef(), nullptr));
	if (!binary_or_err)
		return -1;

	/* Find the .text section. */
	SectionRef text_section;
	uint64_t text_filepos, image_base;
	for (SectionRef section :
	     cast<ObjectFile>(*binary_or_err.get()).sections()) {
		Expected<StringRef> name = section.getName();
		if (name && *name == ".text") {
			text_section = section;

			/*
			 * If we don't find an image base below, we infer the
			 * image base * from the address and file offset of the
			 * .text section.
			 */
			text_filepos = reinterpret_cast<const char *>(
				text_section.getContents()->bytes_begin()) -
				mem_buf_or_err.get()->getBufferStart();
			image_base = text_section.getAddress() - text_filepos;
			break;
		}
	}
	if (text_section == SectionRef())
		/* No .text section, so no symbols (but also not a failure). */
		return 0;

	/*
	 * See if we can find an explicit image base pseudosymbol. If so, get
	 * the image base directly from it, then infer the file position of
	 * .text from that (i.e., the opposite inference of the fallback above).
	 */
	for (SymbolRef symbol :
	     cast<ObjectFile>(*binary_or_err.get()).symbols())
		if (symbol.getName() &&
		    symbol.getAddress() &&
		    (*symbol.getName() == "__ImageBase" ||
		     *symbol.getName() == "__image_base__")) {
			image_base = *symbol.getAddress();
			if (image_base < 0x100000000ULL)
				/*
				 * PE symbols can only have 4 bytes, so use
				 * .text high bits (if any).
				 */
				image_base |= text_section.getAddress() &
					~0xFFFFFFFFULL;
			text_filepos = text_section.getAddress() - image_base;
			break;
		}

	symbols->image_base = image_base;
	symbols->text_end = text_filepos + text_section.getSize();

	/* Collect all valid symbols. */
	std::vector<SymbolRef> all_symbols;
	for (SymbolRef symbol :
	     cast<ObjectFile>(*binary_or_err.get()).symbols())
		if (symbol.getName() && symbol.getFlags() &&
		    symbol.getAddress() && symbol.getSection())
			all_symbols.push_back(symbol);
	symbols->num_symbols = all_symbols.size();
	symbols->symbols = (struct llvm_symbol *)calloc(
		all_symbols.size(), sizeof(struct llvm_symbol));
	if (symbols->symbols == nullptr)
		return -1;

	/*
	 * Symbols don't normally come with lengths, so we'll infer them
	 * from what comes after the symbol address-wise. There is some
	 * extra logic around zero-length symbols and deduplication,
	 * which the caller will do for us (it's shared with other backends).
	 */
	std::sort(all_symbols.begin(), all_symbols.end(),
		  [](const SymbolRef &a, const SymbolRef &b) {
			  if (*a.getAddress() != *b.getAddress())
				  return *a.getAddress() < *b.getAddress();
			  return *a.getName() < *b.getName();
		  });
	for (size_t i = 0; i < all_symbols.size(); ++i) {
		const SymbolRef &sym = all_symbols[i];
		llvm_symbol &out_sym = symbols->symbols[i];
		out_sym.start = *sym.getAddress() - image_base;
		out_sym.name = (char *)calloc(1, sym.getName()->size() + 1);
		if (out_sym.name == nullptr) {
			for (size_t i = 0; i < all_symbols.size(); ++i) {
				zfree(&symbols->symbols[i].name);
			}
			zfree(&symbols->symbols);
			return -1;
		}
		memcpy(out_sym.name, sym.getName()->bytes_begin(),
		       sym.getName()->size());
		out_sym.global = *sym.getFlags() & SymbolRef::SF_Global;
		out_sym.weak = *sym.getFlags() & SymbolRef::SF_Weak;

		SectionRef section = **sym.getSection();
		uint64_t next_addr;
		if (i + 1 < all_symbols.size() &&
		    section == **all_symbols[i + 1].getSection())
			next_addr = *all_symbols[i + 1].getAddress();
		else
			next_addr = section.getAddress() + section.getSize();

		out_sym.len = next_addr - *sym.getAddress();
	}

	return 1;
}
