// SPDX-License-Identifier: GPL-2.0
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ttydefaults.h>
#include <linux/kernel.h>

#include "../browser.h"
#include "../keysyms.h"
#include "../helpline.h"
#include "../ui.h"
#include "../util.h"
#include "../libslang.h"
#include "units.h"

static void ui_browser__argv_write(struct ui_browser *browser,
				   void *entry, int row)
{
	char **arg = entry;
	bool current_entry = ui_browser__is_current_entry(browser, row);

	ui_browser__set_color(browser, current_entry ? HE_COLORSET_SELECTED :
						       HE_COLORSET_NORMAL);
	ui_browser__write_nstring(browser, *arg, browser->width);
}

static int popup_menu__run(struct ui_browser *menu, int *keyp)
{
	int key;

	if (ui_browser__show(menu, " ", "ESC: exit, ENTER|->: Select option") < 0)
		return -1;

	while (1) {
		key = ui_browser__run(menu, 0);

		switch (key) {
		case K_RIGHT:
		case K_ENTER:
			key = menu->index;
			break;
		case K_LEFT:
		case K_ESC:
		case 'q':
		case CTRL('c'):
			key = -1;
			break;
		default:
			if (keyp) {
				*keyp = key;
				key = menu->nr_entries;
				break;
			}
			continue;
		}

		break;
	}

	ui_browser__hide(menu);
	return key;
}

int ui__popup_menu(int argc, char * const argv[], int *keyp)
{
	struct ui_browser menu = {
		.entries    = (void *)argv,
		.refresh    = ui_browser__argv_refresh,
		.seek	    = ui_browser__argv_seek,
		.write	    = ui_browser__argv_write,
		.nr_entries = argc,
	};
	return popup_menu__run(&menu, keyp);
}

int ui_browser__input_window(const char *title, const char *text, char *input,
			     const char *exit_msg, int delay_secs)
{
	int x, y, len, key;
	int max_len = 60, nr_lines = 0;
	static char buf[50];
	const char *t;

	t = text;
	while (1) {
		const char *sep = strchr(t, '\n');

		if (sep == NULL)
			sep = strchr(t, '\0');
		len = sep - t;
		if (max_len < len)
			max_len = len;
		++nr_lines;
		if (*sep == '\0')
			break;
		t = sep + 1;
	}

	mutex_lock(&ui__lock);

	max_len += 2;
	nr_lines += 8;
	y = SLtt_Screen_Rows / 2 - nr_lines / 2;
	x = SLtt_Screen_Cols / 2 - max_len / 2;

	SLsmg_set_color(0);
	SLsmg_draw_box(y, x++, nr_lines, max_len);
	if (title) {
		SLsmg_gotorc(y, x + 1);
		SLsmg_write_string(title);
	}
	SLsmg_gotorc(++y, x);
	nr_lines -= 7;
	max_len -= 2;
	SLsmg_write_wrapped_string((unsigned char *)text, y, x,
				   nr_lines, max_len, 1);
	y += nr_lines;
	len = 5;
	while (len--) {
		SLsmg_gotorc(y + len - 1, x);
		SLsmg_write_nstring(" ", max_len);
	}
	SLsmg_draw_box(y++, x + 1, 3, max_len - 2);

	SLsmg_gotorc(y + 3, x);
	SLsmg_write_nstring(exit_msg, max_len);
	SLsmg_refresh();

	mutex_unlock(&ui__lock);

	x += 2;
	len = 0;
	key = ui__getch(delay_secs);
	while (key != K_TIMER && key != K_ENTER && key != K_ESC) {
		mutex_lock(&ui__lock);

		if (key == K_BKSPC) {
			if (len == 0) {
				mutex_unlock(&ui__lock);
				goto next_key;
			}
			SLsmg_gotorc(y, x + --len);
			SLsmg_write_char(' ');
		} else {
			buf[len] = key;
			SLsmg_gotorc(y, x + len++);
			SLsmg_write_char(key);
		}
		SLsmg_refresh();

		mutex_unlock(&ui__lock);

		/* XXX more graceful overflow handling needed */
		if (len == sizeof(buf) - 1) {
			ui_helpline__push("maximum size of symbol name reached!");
			key = K_ENTER;
			break;
		}
next_key:
		key = ui__getch(delay_secs);
	}

	buf[len] = '\0';
	strncpy(input, buf, len+1);
	return key;
}

void __ui__info_window(const char *title, const char *text, const char *exit_msg)
{
	int x, y;
	int max_len = 0, nr_lines = 0;
	const char *t;

	t = text;
	while (1) {
		const char *sep = strchr(t, '\n');
		int len;

		if (sep == NULL)
			sep = strchr(t, '\0');
		len = sep - t;
		if (max_len < len)
			max_len = len;
		++nr_lines;
		if (*sep == '\0')
			break;
		t = sep + 1;
	}

	max_len += 2;
	nr_lines += 2;
	if (exit_msg)
		nr_lines += 2;
	y = SLtt_Screen_Rows / 2 - nr_lines / 2,
	x = SLtt_Screen_Cols / 2 - max_len / 2;

	SLsmg_set_color(0);
	SLsmg_draw_box(y, x++, nr_lines, max_len);
	if (title) {
		SLsmg_gotorc(y, x + 1);
		SLsmg_write_string(title);
	}
	SLsmg_gotorc(++y, x);
	if (exit_msg)
		nr_lines -= 2;
	max_len -= 2;
	SLsmg_write_wrapped_string((unsigned char *)text, y, x,
				   nr_lines, max_len, 1);
	if (exit_msg) {
		SLsmg_gotorc(y + nr_lines - 2, x);
		SLsmg_write_nstring(" ", max_len);
		SLsmg_gotorc(y + nr_lines - 1, x);
		SLsmg_write_nstring(exit_msg, max_len);
	}
}

void ui__info_window(const char *title, const char *text)
{
	mutex_lock(&ui__lock);
	__ui__info_window(title, text, NULL);
	SLsmg_refresh();
	mutex_unlock(&ui__lock);
}

int ui__question_window(const char *title, const char *text,
			const char *exit_msg, int delay_secs)
{
	mutex_lock(&ui__lock);
	__ui__info_window(title, text, exit_msg);
	SLsmg_refresh();
	mutex_unlock(&ui__lock);
	return ui__getch(delay_secs);
}

int ui__help_window(const char *text)
{
	return ui__question_window("Help", text, "Press any key...", 0);
}

int ui__dialog_yesno(const char *msg)
{
	return ui__question_window(NULL, msg, "Enter: Yes, ESC: No", 0);
}

static int __ui__warning(const char *title, const char *format, va_list args)
{
	char *s;

	if (vasprintf(&s, format, args) > 0) {
		int key;

		key = ui__question_window(title, s, "Press any key...", 0);
		free(s);
		return key;
	}

	fprintf(stderr, "%s\n", title);
	vfprintf(stderr, format, args);
	return K_ESC;
}

static int perf_tui__error(const char *format, va_list args)
{
	return __ui__warning("Error:", format, args);
}

static int perf_tui__warning(const char *format, va_list args)
{
	return __ui__warning("Warning:", format, args);
}

struct perf_error_ops perf_tui_eops = {
	.error		= perf_tui__error,
	.warning	= perf_tui__warning,
};

/*
 * The debuginfod fetch progress window, drawn over the browser while a
 * fetch that can be big, such as the vmlinux for a kernel profiled on
 * another machine, is in progress: with nothing on screen the browser
 * looks hung, which is what makes users interrupt perf, see
 * util/debuginfo.c, that polls the 's'/'d' keys with ui__key_pending()
 * /ui__key_read().
 *
 * The client calls the progress callback at every write chunk, so only
 * redraw when the fetched bytes change: a resize is picked up by the
 * ui__refresh_dimensions() on the next redraw, and taking the window
 * down erases exactly the rows it was drawn at, saved at draw time.
 *
 * The text is word wrapped to the available columns: constrained
 * terminals, such as a smartphone running termux, have far fewer of
 * them and would crop the tail of a long line, hiding the 'd' option,
 * leaving no visible way out of the fetch.
 */
#define PROGRESS_WINDOW_MAX_LINES 12

static bool progress_window__shown;
static char progress_window__bytes[64];
static int progress_window__y, progress_window__rows;

/*
 * Word wrap @text, on spaces, into lines of at most @width characters,
 * breaking words that don't fit whole, returning the number of lines.
 */
static int progress_window__wrap(const char *text, int width,
				 char lines[PROGRESS_WINDOW_MAX_LINES][256])
{
	int nr_lines = 0;
	const char *p = text;

	while (*p && nr_lines < PROGRESS_WINDOW_MAX_LINES) {
		char *line = lines[nr_lines++];
		int len = 0;
		bool space = false;

		line[0] = '\0';
		while (*p) {
			const char *word;
			int wlen, avail;

			while (*p == ' ' || *p == '\n')
				++p;
			if (*p == '\0')
				break;
			word = p;
			while (*p && *p != ' ' && *p != '\n')
				++p;
			wlen = p - word;
			avail = width - len - (space ? 1 : 0);
			if (wlen > avail) {
				if (len > 0) {
					p = word;
					break;
				}
				if (avail > 0) {
					/* The word doesn't fit whole */
					memcpy(line, word, avail);
					len = avail;
					line[len] = '\0';
					p = word + avail;
				}
				break;
			}
			if (space)
				line[len++] = ' ';
			memcpy(line + len, word, wlen);
			len += wlen;
			line[len] = '\0';
			space = true;
		}
	}

	return nr_lines;
}

void ui__progress_window(const char *title, const char *text,
			 u64 fetched, u64 total)
{
	static char lines[PROGRESS_WINDOW_MAX_LINES][256];
	char buf_cur[20], buf_tot[20], bytes[64];
	size_t len;
	int y, height, nr_lines, inner, i;

	if (use_browser != 1)
		return;

	unit_number__scnprintf(buf_cur, sizeof(buf_cur), fetched);
	if (total) {
		unit_number__scnprintf(buf_tot, sizeof(buf_tot), total);
		scnprintf(bytes, sizeof(bytes), "  %s / %s fetched",
			  buf_cur, buf_tot);
	} else {
		scnprintf(bytes, sizeof(bytes),
			  "  %s fetched, size unknown", buf_cur);
	}

	if (progress_window__shown && !strcmp(bytes, progress_window__bytes))
		return;

	scnprintf(progress_window__bytes, sizeof(progress_window__bytes),
		  "%s", bytes);
	progress_window__shown = true;

	ui__refresh_dimensions(false);
	mutex_lock(&ui__lock);
	inner = SLtt_Screen_Cols - 2;
	if (inner > 255)
		inner = 255;
	nr_lines = progress_window__wrap(text, inner, lines);
	height = nr_lines + 3;

	SLsmg_set_color(0);
	if (progress_window__rows)
		SLsmg_fill_region(progress_window__y, 0, progress_window__rows,
				  SLtt_Screen_Cols, ' ');
	y = (SLtt_Screen_Rows - height) / 2;
	if (y < 0)
		y = 0;
	progress_window__y = y;
	progress_window__rows = height;

	SLsmg_draw_box(y, 0, height, SLtt_Screen_Cols);
	SLsmg_gotorc(y++, 1);
	len = strlen(title);
	if (len > (size_t)inner)
		len = inner;
	SLsmg_write_nchars(title, len);
	for (i = 0; i < nr_lines; i++, y++) {
		SLsmg_gotorc(y, 1);
		SLsmg_write_nstring(lines[i], SLtt_Screen_Cols - 2);
	}
	SLsmg_gotorc(y, 1);
	SLsmg_write_nstring(bytes, SLtt_Screen_Cols - 2);
	SLsmg_refresh();
	mutex_unlock(&ui__lock);
}

void ui__progress_window_end(void)
{
	if (!progress_window__shown || use_browser != 1)
		return;

	progress_window__shown = false;
	progress_window__bytes[0] = '\0';

	mutex_lock(&ui__lock);
	SLsmg_set_color(0);
	SLsmg_fill_region(progress_window__y, 0, progress_window__rows,
			  SLtt_Screen_Cols, ' ');
	progress_window__rows = 0;
	SLsmg_refresh();
	mutex_unlock(&ui__lock);
}

/*
 * The keys typed while the fetch blocks the thread that runs the
 * browser are in the TUI input queue: nobody else is reading it, the
 * browser is the thread doing the fetch, so the progress callback in
 * util/debuginfo.c can drain it through these, that keep slang behind
 * the ui/ layer.
 */
bool ui__key_pending(void)
{
	return use_browser == 1 && SLang_input_pending(0) > 0;
}

int ui__key_read(void)
{
	return SLang_getkey();
}
