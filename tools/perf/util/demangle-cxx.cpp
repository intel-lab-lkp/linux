// SPDX-License-Identifier: GPL-2.0
#include "demangle-cxx.h"
#include <stdlib.h>
#include <string.h>
#include <linux/compiler.h>

#ifdef HAVE_CXA_DEMANGLE_SUPPORT
#include <cxxabi.h>
#endif

/*
 * Demangle C++ function signature
 *
 * Note: caller is responsible for freeing demangled string
 */
extern "C"
char *cxx_demangle_sym(const char *str, bool params __maybe_unused,
                       bool modifiers __maybe_unused)
{
#if defined(HAVE_CXA_DEMANGLE_SUPPORT)
        char *output;
        int status;

        output = abi::__cxa_demangle(str, /*output_buffer=*/NULL, /*length=*/NULL, &status);
        return output;
#else
        return NULL;
#endif
}
