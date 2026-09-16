/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PERF_UI_UTIL_H_
#define _PERF_UI_UTIL_H_ 1

#include <stdbool.h>
#include <stdarg.h>
#include <linux/compiler.h>
#include <linux/types.h>

int ui__getch(int delay_secs);

/*
 * The TUI owns the terminal and its input queue, so the rest of perf
 * asks the ui/ layer for progress display and for the keys typed while
 * it is not reading them, instead of touching slang or stdin directly:
 * slang stays behind these, as the plan is to reduce the number of
 * libraries perf needs to build, or swap slang for something else,
 * such as ncurses, at some point.
 *
 * ui__progress_window() draws and updates a window over the browser
 * telling that a long operation, for now the debuginfod fetch in
 * util/debuginfo.c, is in progress, with @fetched/@total as the bytes
 * fetched so far and the total when known, 0 otherwise, while
 * ui__progress_window_end() takes the window down.  The keys typed
 * while the fetch blocks the browser are drained with
 * ui__key_pending()/ui__key_read().
 */
#ifndef HAVE_SLANG_SUPPORT
/*
 * Without the TUI there is no browser to draw over and no input queue
 * owned by a UI: the stdio progress and keys for the debuginfod fetch
 * live in util/debuginfo.c, so these are no-ops, keeping callers free
 * of #ifdefs.
 */
static inline bool ui__key_pending(void) { return false; }
static inline int ui__key_read(void) { return -1; }
static inline void ui__progress_window(const char *title __maybe_unused,
				       const char *text __maybe_unused,
				       u64 fetched __maybe_unused,
				       u64 total __maybe_unused) {}
static inline void ui__progress_window_end(void) {}
#else /* HAVE_SLANG_SUPPORT */
bool ui__key_pending(void);
int ui__key_read(void);
void ui__progress_window(const char *title, const char *text,
			 u64 fetched, u64 total);
void ui__progress_window_end(void);
#endif /* HAVE_SLANG_SUPPORT */

int ui__popup_menu(int argc, char * const argv[], int *keyp);
int ui__help_window(const char *text);
int ui__dialog_yesno(const char *msg);
void __ui__info_window(const char *title, const char *text, const char *exit_msg);
void ui__info_window(const char *title, const char *text);
int ui__question_window(const char *title, const char *text,
			const char *exit_msg, int delay_secs);

struct perf_error_ops {
	int (*error)(const char *format, va_list args);
	int (*warning)(const char *format, va_list args);
};

int perf_error__register(struct perf_error_ops *eops);
int perf_error__unregister(struct perf_error_ops *eops);

#endif /* _PERF_UI_UTIL_H_ */
