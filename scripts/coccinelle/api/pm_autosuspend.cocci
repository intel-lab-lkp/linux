// SPDX-License-Identifier: GPL-2.0-only
/// Find drivers that enable autosuspend via pm_runtime_use_autosuspend()
/// but never call pm_runtime_dont_use_autosuspend() anywhere in the file,
/// while not using devm_pm_runtime_enable().
//
// Confidence: High
// Copyright: (C) 2026 Joshua Crofts
// URL: https://coccinelle.gitlabpages.inria.fr/website
// Options: --no-includes

virtual context
virtual org
virtual report

//----------------------------------------------------------
// Detection
//----------------------------------------------------------

@has_use@
expression dev;
position p;
@@

pm_runtime_use_autosuspend@p(dev)

@has_dont@
expression dev;
@@

pm_runtime_dont_use_autosuspend(dev)

@has_devm@
expression dev;
@@

devm_pm_runtime_enable(dev)

//----------------------------------------------------------
// Context mode
//----------------------------------------------------------

@depends on context && has_use && !has_dont && !has_devm@
expression dev;
@@

* pm_runtime_use_autosuspend(dev)

//----------------------------------------------------------
// Org and report mode
//----------------------------------------------------------

@script:python depends on org && !has_dont && !has_devm@
p << has_use.p;
@@

msg = "WARNING: pm_runtime_use_autosuspend() called without matching pm_runtime_dont_use_autosuspend() or devm_pm_runtime_enable()"
cocci.print_main(msg, p)

@script:python depends on report && !has_dont && !has_devm@
p << has_use.p;
@@

msg = "WARNING: pm_runtime_use_autosuspend() called without matching pm_runtime_dont_use_autosuspend() or devm_pm_runtime_enable()"
coccilib.report.print_report(p[0], msg)
