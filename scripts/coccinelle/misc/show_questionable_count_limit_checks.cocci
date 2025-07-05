// SPDX-License-Identifier: GPL-2.0
/// Reconsider questionable count limit checks.
//
// Keywords: detection programming mistakes
// Confidence: Medium
// Options: --no-includes --include-headers

virtual context, patch, report, org

@depends on context@
expression limit;
identifier i;
statement s;
@@
 for (
      <+... i = 0 ...+> ;
*     i <= limit
      ;
      <+...
(     i++
|     ++i
)     ...+>
     )
 s

@depends on patch disable gtr_lss_eq@
expression limit;
identifier i;
statement s;
@@
 for (
      <+... i = 0 ...+> ;
(
-     i <= limit
+     i < limit
|
-     limit >= i
+     limit > i
)     ;
      <+...
(     i++
|     ++i
)     ...+>
     )
 s

@x depends on org || report@
expression limit;
identifier i;
position p;
statement s;
@@
 for (
      <+... i = 0 ...+> ;
      i <= limit
      ;@p
      <+...
(     i++
|     ++i
)     ...+>
     )
 s

@script:python depends on org@
p << x.p;
@@
coccilib.org.print_todo(p[0], "WARNING: Reconsider questionable count limit check once more.")

@script:python depends on report@
p << x.p;
@@
coccilib.report.print_report(p[0], "WARNING: Reconsider questionable count limit check once more.")
