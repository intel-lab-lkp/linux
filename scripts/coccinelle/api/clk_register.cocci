// SPDX-License-Identifier: GPL-2.0
/// Remove error messages after clk registration failures, because
/// clk_register(), clk_hw_register(), and their variants already log
/// an error when they fail. See commit 12a0fd23e870 ("clk: Print an
/// error when clk registration fails").
//
// Confidence: Medium
// Options: --include-headers

virtual patch
virtual context
virtual org
virtual report

@depends on context@
expression clk;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

clk = \(clk_register\|devm_clk_register\)(...);
if ( IS_ERR(clk) )
{
...
*voidfn(...);
...
}

@depends on patch@
expression clk;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

clk = \(clk_register\|devm_clk_register\)(...);
-if ( IS_ERR(clk) )
-voidfn(...);

@depends on patch@
expression clk;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
statement S;
@@

clk = \(clk_register\|devm_clk_register\)(...);
if ( IS_ERR(clk) )
(
-{
-voidfn(...);
S
-}
|
{
...
-voidfn(...);
...
}
)

@r1 depends on org || report@
position p1;
expression clk;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

clk = \(clk_register\|devm_clk_register\)(...);
if ( IS_ERR(clk) )
{
...
voidfn@p1(...);
...
}

@depends on context@
expression ret;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

ret = \(clk_hw_register\|devm_clk_hw_register\|of_clk_hw_register\)(...);
if ( \( ret \| ret < 0 \) )
{
...
*voidfn(...);
...
}

@depends on patch@
expression ret;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

ret = \(clk_hw_register\|devm_clk_hw_register\|of_clk_hw_register\)(...);
-if ( \( ret \| ret < 0 \) )
-voidfn(...);

@depends on patch@
expression ret;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
statement S;
@@

ret = \(clk_hw_register\|devm_clk_hw_register\|of_clk_hw_register\)(...);
if ( \( ret \| ret < 0 \) )
(
-{
-voidfn(...);
S
-}
|
{
...
-voidfn(...);
...
}
)

@r2 depends on org || report@
position p2;
expression ret;
identifier voidfn =~ "^(dev_err|dev_warn|pr_err|pr_warn)$";
@@

ret = \(clk_hw_register\|devm_clk_hw_register\|of_clk_hw_register\)(...);
if ( \( ret \| ret < 0 \) )
{
...
voidfn@p2(...);
...
}

@script:python depends on org@
p1 << r1.p1;
@@

cocci.print_main(p1)

@script:python depends on report@
p1 << r1.p1;
@@

msg = "line %s is redundant because clk_register() already prints an error on failure" % (p1[0].line)
coccilib.report.print_report(p1[0], msg)

@script:python depends on org@
p2 << r2.p2;
@@

cocci.print_main(p2)

@script:python depends on report@
p2 << r2.p2;
@@

msg = "line %s is redundant because clk_hw_register() already prints an error on failure" % (p2[0].line)
coccilib.report.print_report(p2[0], msg)
