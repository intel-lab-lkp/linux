// SPDX-License-Identifier: GPL-2.0
/// Omit pm_runtime_mark_last_busy() call before pm_runtime_put_autosuspend().
//
// Keywords: wrapper function access timestamp autosuspend
// Confidence: High
// Options: --no-includes --include-headers

virtual context, patch, report, org

@depends on context@
expression e;
@@
*pm_runtime_mark_last_busy(e);
 pm_runtime_put_autosuspend(e);

@depends on patch@
expression e;
@@
-pm_runtime_mark_last_busy(e);
 pm_runtime_put_autosuspend(e);

@x depends on org || report@
expression e;
position p;
@@
 pm_runtime_mark_last_busy@p(e);
 pm_runtime_put_autosuspend(e);

@script:python depends on org@
p << x.p;
@@
coccilib.org.print_todo(p[0], "WARNING: Delete pm_runtime_mark_last_busy() call before pm_runtime_put_autosuspend()")

@script:python depends on report@
p << x.p;
@@
coccilib.report.print_report(p[0], "WARNING: Delete pm_runtime_mark_last_busy() call before pm_runtime_put_autosuspend()")
