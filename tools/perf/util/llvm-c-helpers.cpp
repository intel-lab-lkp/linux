// SPDX-License-Identifier: GPL-2.0

/*
 * Must come before the linux/compiler.h include, which defines several
 * macros (e.g. noinline) that conflict with compiler builtins used
 * by LLVM.
 */
#ifdef HAVE_LIBLLVM_SUPPORT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"  /* Needed for LLVM <= 15 */
#include <llvm/DebugInfo/Symbolize/Symbolize.h>
#include <llvm/Support/TargetSelect.h>
#pragma GCC diagnostic pop
#endif

#if !defined(HAVE_LIBLLVM_SUPPORT) || defined(HAVE_LIBLLVM_DYNAMIC)
#include <dlfcn.h>
#endif
#include <inttypes.h>
#include <stdio.h>
#include <sys/types.h>
#include <linux/compiler.h>
extern "C" {
#include "debug.h"
#include <linux/zalloc.h>
}
#include "llvm-c-helpers.h"

extern "C"
char *dso__demangle_sym(struct dso *dso, int kmodule, const char *elf_name);

#ifdef HAVE_LIBLLVM_SUPPORT
using namespace llvm;
using llvm::symbolize::LLVMSymbolizer;
#endif

#if !defined(HAVE_LIBLLVM_SUPPORT) && defined(HAVE_LIBLLVM_DYNAMIC)
static void *perf_llvm_c_helpers_dll_handle(void)
{
	static bool dll_handle_init;
	static void *dll_handle;

	if (!dll_handle_init) {
		dll_handle_init = true;
		dll_handle = dlopen("libperf-llvm.so", RTLD_LAZY);
		if (!dll_handle)
			pr_debug("dlopen failed for libperf-llvm.so\n");
	}
	return dll_handle;
}
#endif

/*
 * Allocate a static LLVMSymbolizer, which will live to the end of the program.
 * Unlike the bfd paths, LLVMSymbolizer has its own cache, so we do not need
 * to store anything in the dso struct.
 */
#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
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
#endif

/* Returns 0 on error, 1 on success. */
#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
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
#endif

extern "C"
int llvm_addr2line(const char *dso_name __maybe_unused, u64 addr __maybe_unused,
		   char **file __maybe_unused, unsigned int *line __maybe_unused,
		   bool unwind_inlines __maybe_unused,
		   llvm_a2l_frame **inline_frames __maybe_unused)
{
#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
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
#elif defined(HAVE_LIBLLVM_DYNAMIC)
	static bool fn_init;
	static int (*fn)(const char *dso_name, u64 addr,
			 char **file, unsigned int *line,
			 bool unwind_inlines,
			 llvm_a2l_frame **inline_frames);

	if (!fn_init) {
		void * handle = perf_llvm_c_helpers_dll_handle();

		if (!handle)
			return 0;

		fn = reinterpret_cast<decltype(fn)>(dlsym(handle, "llvm_addr2line"));
		if (!fn)
			pr_debug("dlsym failed for llvm_addr2line\n");
		fn_init = true;
	}
	if (!fn)
		return 0;
	return fn(dso_name, addr, file, line, unwind_inlines, inline_frames);
#else
	return 0;
#endif
}

#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
static char *
make_symbol_relative_string(struct dso *dso, const char *sym_name,
			    u64 addr, u64 base_addr)
{
	if (!strcmp(sym_name, "<invalid>"))
		return NULL;

	char *demangled = dso__demangle_sym(dso, 0, sym_name);
	if (base_addr && base_addr != addr) {
		char buf[256];
		snprintf(buf, sizeof(buf), "%s+0x%" PRIx64,
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
#endif

extern "C"
char *llvm_name_for_code(struct dso *dso __maybe_unused, const char *dso_name __maybe_unused,
			 u64 addr __maybe_unused)
{
#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
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
#elif defined(HAVE_LIBLLVM_DYNAMIC)
	static bool fn_init;
	static char *(*fn)(struct dso *dso, const char *dso_name, u64 addr);

	if (!fn_init) {
		void * handle = perf_llvm_c_helpers_dll_handle();

		if (!handle)
			return NULL;

		fn = reinterpret_cast<decltype(fn)>(dlsym(handle, "llvm_name_for_code"));
		if (!fn)
			pr_debug("dlsym failed for llvm_name_for_code\n");
		fn_init = true;
	}
	if (!fn)
		return NULL;
	return fn(dso, dso_name, addr);
#else
	return 0;
#endif
}

extern "C"
char *llvm_name_for_data(struct dso *dso __maybe_unused, const char *dso_name __maybe_unused,
			 u64 addr __maybe_unused)
{
#if defined(HAVE_LIBLLVM_SUPPORT) && !defined(HAVE_LIBLLVM_DYNAMIC)
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
#elif defined(HAVE_LIBLLVM_DYNAMIC)
	static bool fn_init;
	static char *(*fn)(struct dso *dso, const char *dso_name, u64 addr);

	if (!fn_init) {
		void * handle = perf_llvm_c_helpers_dll_handle();

		if (!handle)
			return NULL;

		fn = reinterpret_cast<decltype(fn)>(dlsym(handle, "llvm_name_for_data"));
		if (!fn)
			pr_debug("dlsym failed for llvm_name_for_data\n");
		fn_init = true;
	}
	if (!fn)
		return NULL;
	return fn(dso, dso_name, addr);
#else
	return 0;
#endif
}
