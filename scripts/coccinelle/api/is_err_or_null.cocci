// SPDX-License-Identifier: GPL-2.0-only
///
/// Use IF_ERR_OR_NULL() instead of IS_ERR() plus a check for (not) NULL
///
// Copyright: (C) 2026 Philipp Hahn, FRITZ! Technology GmbH.
// Confidence: High
// Options: --no-includes --include-headers
// Keywords: IS_ERR, IS_ERR_OR_NULL

virtual patch
virtual report
virtual org

@p1 depends on patch@
expression E;
@@
(
-	E != NULL && !IS_ERR(E)
+	!IS_ERR_OR_NULL(E)
|
-	E == NULL || IS_ERR(E)
+	IS_ERR_OR_NULL(E)
|
-	!IS_ERR(E) && E != NULL
+	!IS_ERR_OR_NULL(E)
|
-	IS_ERR(E) || E == NULL
+	IS_ERR_OR_NULL(E)
)

@p2 depends on patch@
expression E;
@@
(
-	E == NULL || WARN_ON(IS_ERR(E))
+	WARN_ON(IS_ERR_OR_NULL(E))
|
-	E == NULL || WARN_ON_ONCE(IS_ERR(E))
+	WARN_ON_ONCE(IS_ERR_OR_NULL(E))
)

@p3 depends on patch@
expression E,e1;
@@
(
-	e1 && E != NULL && !IS_ERR(E)
+	e1 && !IS_ERR_OR_NULL(E)
|
-	e1 || E == NULL || IS_ERR(E)
+	e1 || IS_ERR_OR_NULL(E)
|
-	e1 && !IS_ERR(E) && E != NULL
+	e1 && !IS_ERR_OR_NULL(E)
|
-	e1 || IS_ERR(E) || E == NULL
+	e1 || IS_ERR_OR_NULL(E)
)

@r1 depends on report || org@
expression E;
position p;
@@
(
 	E != NULL && ... && !IS_ERR@p(E)
|
 	E == NULL || ... || IS_ERR@p(E)
|
 	!IS_ERR@p(E) && ... && E != NULL
|
 	IS_ERR@p(E) || ... || E == NULL
)

@script:python depends on report@
p << r1.p;
@@
coccilib.report.print_report(p[0], "opportunity for IS_ERR_OR_NULL()")

@script:python depends on org@
p << r1.p;
@@
coccilib.org.print_todo(p[0], "opportunity for IS_ERR_OR_NULL()")

@p4 depends on patch@
identifier I;
expression E;
@@
(
-	(I = E) != NULL && !IS_ERR(I)
+	!IS_ERR_OR_NULL((I = E))
|
-	(I = E) == NULL || IS_ERR(I)
+	IS_ERR_OR_NULL((I = E))
)

@r2 depends on report || org@
identifier I;
expression E;
position p;
@@
(
*	(I = E) != NULL && ... && !IS_ERR@p(I)
|
*	(I = E) == NULL || ... || IS_ERR@p(I)
)

@script:python depends on report@
p << r2.p;
@@
coccilib.report.print_report(p[0], "opportunity for IS_ERR_OR_NULL()")

@script:python depends on org@
p << r2.p;
@@
coccilib.org.print_todo(p[0], "opportunity for IS_ERR_OR_NULL()")

@p5 depends on patch disable unlikely @
expression E;
@@
-\( likely \| unlikely \)(
(
 IS_ERR_OR_NULL(E)
|
 !IS_ERR_OR_NULL(E)
)
-)
