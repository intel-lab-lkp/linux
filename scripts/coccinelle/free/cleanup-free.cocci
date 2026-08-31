// SPDX-License-Identifier: GPL-2.0-only
///
/// Find __cleanup(kfree)
/// Using __cleanup(kfree) will pass the stack address of the annotated
/// local variable to kfree, causing an invalid free.
/// I.e., `T v __cleanup(kfree);` --> `kfree(&v);`
/// Such usage is impossible to be correct.
///
// Confidence: High
// Copyright: (C) 2026 Ella Ma
// Options: --no-includes --include-headers


// Suggesting using __free for the functions with a DEFINE_FREE definition.
// Update this list when new DEFINE_FREE definitions are added.
@free@
attribute name __cleanup;
symbol kfree, kfree_sensitive, kvfree, kvfree_atomic;
type T;
identifier v, n;
position p;
@@

(
  T v __cleanup@p(
(
  n
&
(
  kfree \| kfree_sensitive \| kvfree \| kvfree_atomic
)
)
  );
|
  T v __cleanup@p(
(
  n
&
(
  kfree \| kfree_sensitive \| kvfree \| kvfree_atomic
)
)
  ) = ...;
)

@script:python depends on free@
p << free.p;
n << free.n;
@@

coccilib.report.print_report(
  p[0], f"ERROR: found `__cleanup({n})', do you mean `__free({n})'?")


// Reporting __cleanup usage for the functions without a DEFINE_FREE definition.
// The following list only contains frequently used functions. Supplement it if
// necessary.
@cleanup@
attribute name __cleanup;
symbol kvfree_sensitive, vfree, vfree_atomic;
type T;
identifier v, n;
position p;
@@

(
  T v __cleanup@p(
(
  n
&
(
  kvfree_sensitive \| vfree \| vfree_atomic
)
)
  );
|
  T v __cleanup@p(
(
  n
&
(
  kvfree_sensitive \| vfree \| vfree_atomic
)
)
  ) = ...;
)

@script:python depends on cleanup@
p << cleanup.p;
n << cleanup.n;
@@

coccilib.report.print_report(
  p[0], f"ERROR: `__cleanup({n})' will free the annotated variable, rather than its pointee")
