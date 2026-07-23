// SPDX-License-Identifier: GPL-2.0-only
///
/// Remove a conditional return that has no effect:
///
///	if (ret)
///		return ret;
///	return ret;
///
/// Both branches return the same variable, so the check has no
/// effect. It can also be a negation or a comparison with a
/// constant.
///
/// When a local variable is assigned right before the check, the
/// assignment and the two returns turn into a single return of the
/// assigned expression, and the declaration is dropped if nothing
/// else uses the variable. Otherwise only the check is removed.
///
// Such code is usually a leftover from removing a statement between
// the two returns.
//
// Confidence: High
// Copyright: (C) 2026 Sang-Heon Jeon
// Comments: The fold can delete comments between the check and the
//           final return, so review the generated patch.
// Options: --no-includes --include-headers

virtual patch
virtual context
virtual org
virtual report

//----------------------------------------------------------
//  For patch mode
//----------------------------------------------------------

@collect depends on patch@
identifier ret;
expression E;
binary operator cmp = {<, <=, >, >=, ==, !=};
constant C;
@@
	ret = E;
	if (\(ret \| !ret \| ret cmp C\))
		return ret;
	return ret;

@depends on patch@
local idexpression ret;
expression E;
binary operator cmp = {<, <=, >, >=, ==, !=};
constant C;
@@
-	ret = E;
-	if (\(ret \| !ret \| ret cmp C\))
-		return ret;
-	return ret;
+	return E;

@depends on patch@
idexpression ret;
binary operator cmp = {<, <=, >, >=, ==, !=};
constant C;
@@
-	if (\(ret \| !ret \| ret cmp C\))
-		return ret;
	return ret;

@depends on patch@
type T;
identifier collect.ret;
@@
-	T ret;
	... when != ret
	    when strict

@depends on patch@
type T;
identifier collect.ret;
constant C;
@@
-	T ret = C;
	... when != ret
	    when strict


//----------------------------------------------------------
//  For context mode
//----------------------------------------------------------

@depends on context@
idexpression ret;
binary operator cmp = {<, <=, >, >=, ==, !=};
constant C;
@@
*	if (\(ret \| !ret \| ret cmp C\))
*		return ret;
	return ret;

//----------------------------------------------------------
//  For org and report mode
//----------------------------------------------------------

@r depends on org || report@
idexpression ret;
binary operator cmp = {<, <=, >, >=, ==, !=};
constant C;
position p;
@@
	if@p (\(ret \| !ret \| ret cmp C\))
		return ret;
	return ret;

@script:python depends on org@
p << r.p;
@@
cocci.print_main("WARNING: conditional return with no effect (both branches return the same value)", p)

@script:python depends on report@
p << r.p;
@@
coccilib.report.print_report(p[0], "WARNING: conditional return with no effect (both branches return the same value)")
