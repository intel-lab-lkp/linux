// SPDX-License-Identifier: GPL-2.0
/*
 * Progress feedback for the stdio (non-TUI/GTK) case, enabled via
 * 'perf report --progress': the perf.data file size based progress for
 * the event processing phase, plus the hist entry based ones for the
 * merging and sorting phases.
 */
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/kernel.h>
#include "../../util/debug.h"
#include "../../util/units.h"
#include "../progress.h"

/*
 * Phases can be nested, e.g. the ordered events flushes that take place
 * while the "Processing events..." phase is still in progress, so keep
 * track of the ones started so far to be able to complete the right one
 * when a phase finishes, as ui_progress__finish() gets no arguments.  The
 * bookkeeping of what was last printed is per phase: when the nested one
 * finishes, the outer one must still know that its own last line was
 * already the complete one, else it would be printed again at finish time.
 */
#define STDIO_PROGRESS__MAX_DEPTH 8

struct stdio_progress_phase {
	struct ui_progress	*p;
	u64			last_printed;
	size_t			last_len;
};

static struct stdio_progress_phase stdio_progress__stack[STDIO_PROGRESS__MAX_DEPTH];
static int stdio_progress__depth;
static bool stdio_progress__is_tty;
/*
 * Phases that started when there was no room left for them on the stack:
 * they are not shown, and their finish() is still to come, see
 * stdio_progress__finish().
 */
static int stdio_progress__dropped;

static void stdio_progress__print_phase(struct stdio_progress_phase *phase,
					u64 curr)
{
	struct ui_progress *p = phase->p;
	char buf_cur[20], buf_tot[20], buf[128];
	double percent = p->total ? 100.0 * (double)curr / (double)p->total : 0.0;
	size_t len;

	/*
	 * Only the completion line shows 100.0%: a 99.99% progress would
	 * round up to it in the display, making the line printed at finish
	 * time look like a duplicate.
	 */
	if (curr < p->total && percent > 99.9)
		percent = 99.9;

	if (p->size) {
		unit_number__scnprintf(buf_cur, sizeof(buf_cur), curr);
		unit_number__scnprintf(buf_tot, sizeof(buf_tot), p->total);
		len = scnprintf(buf, sizeof(buf), "%s [%5.1f%%] %s / %s",
				p->title, percent, buf_cur, buf_tot);
	} else {
		len = scnprintf(buf, sizeof(buf), "%s [%5.1f%%] %" PRIu64 " / %" PRIu64,
				p->title, percent, curr, p->total);
	}

	if (!stdio_progress__is_tty) {
		fprintf(stderr, "%s\n", buf);
		goto out;
	}

	/* Pad to the length of the previous line to erase its leftovers. */
	fprintf(stderr, "\r%s%*s", buf,
		(int)(len < phase->last_len ? phase->last_len - len : 0), "");
	phase->last_len = len;
out:
	phase->last_printed = curr;
	fflush(stderr);
}

static void __stdio_progress__init(struct ui_progress *p)
{
	/*
	 * The default step (total / 16) is meant for the TUI progress
	 * bar, for stdio, where a percentage is printed, use 1% steps.
	 */
	p->next = p->step = p->total / 100 ?: 1;

	if (stdio_progress__depth == STDIO_PROGRESS__MAX_DEPTH) {
		/*
		 * Out of room: don't start this phase, stdio_progress__update()
		 * then ignores its updates, as it doesn't match the innermost
		 * running phase, and its finish() is swallowed below, so that
		 * it doesn't complete the phase that encloses it.  Completing
		 * that one here, to make room, would leave its own finish()
		 * without a phase to complete, which is what would then get
		 * out of sync, finishing the phases above it one by one.
		 */
		pr_warning("progress phases nested deeper than %d, not showing progress for %s\n",
			   STDIO_PROGRESS__MAX_DEPTH, p->title);
		stdio_progress__dropped++;
		return;
	}

	/* Start a nested phase in a line of its own. */
	if (stdio_progress__depth && stdio_progress__is_tty)
		fputc('\n', stderr);

	stdio_progress__stack[stdio_progress__depth++] =
		(struct stdio_progress_phase) {
			.p = p,
			.last_printed = 0,
			.last_len = 0,
		};

	stdio_progress__print_phase(&stdio_progress__stack[stdio_progress__depth - 1],
				    p->curr);
}

static void stdio_progress__update(struct ui_progress *p)
{
	/*
	 * Phases are started/finished via init/finish, if we get an
	 * update that doesn't match the innermost running phase then
	 * something got out of sync, print nothing rather than some
	 * other phase's numbers, or, with no phases at all, reading
	 * stdio_progress__stack[-1].
	 */
	if (!stdio_progress__depth ||
	    stdio_progress__stack[stdio_progress__depth - 1].p != p)
		return;

	stdio_progress__print_phase(&stdio_progress__stack[stdio_progress__depth - 1],
				    p->curr);
}

static void stdio_progress__finish(void)
{
	struct stdio_progress_phase *phase;

	/*
	 * A phase that didn't fit on the stack still gets its finish(), and
	 * being the innermost one, it is the first to come: swallow it, or
	 * it would complete the phase that encloses it.
	 */
	if (stdio_progress__dropped) {
		stdio_progress__dropped--;
		return;
	}

	if (!stdio_progress__depth)
		return;

	phase = &stdio_progress__stack[--stdio_progress__depth];

	/*
	 * As we print only at 1% steps, the last line printed may have
	 * stopped short of the total, so close this phase showing it as
	 * complete, unless that was what got printed already.
	 */
	if (phase->last_printed != phase->p->total)
		stdio_progress__print_phase(phase, phase->p->total);

	phase->last_printed	= 0;
	phase->last_len		= 0;

	if (stdio_progress__is_tty)
		fputc('\n', stderr);

	fflush(stderr);
}

static struct ui_progress_ops stdio_progress__ops = {
	.init	= __stdio_progress__init,
	.update	= stdio_progress__update,
	.finish	= stdio_progress__finish,
};

void stdio_progress__init(void)
{
	stdio_progress__is_tty = isatty(STDERR_FILENO) == 1;
	ui_progress__ops = &stdio_progress__ops;
}
