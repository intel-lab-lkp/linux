/* stub for compiling srcs syncd from mesa */

#ifndef U_DEBUG_H_
#define U_DEBUG_H_

#if defined(__GNUC__)
#define _util_printf_format(fmt, list) __attribute__ ((format (printf, fmt, list)))
#else
#define _util_printf_format(fmt, list)
#endif

#endif /* U_DEBUG_H_ */
