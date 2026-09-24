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

virtual report
virtual org

// Suggesting using __free for the functions with a DEFINE_FREE definition.
// Update this list when new DEFINE_FREE definitions are added.
@free@
attribute name __cleanup;
symbol kfree, kfree_sensitive, kvfree, kvfree_atomic, free_percpu;
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
  kfree \| kfree_sensitive \| kvfree \| kvfree_atomic \| free_percpu
)
)
  );
|
  T v __cleanup@p(
(
  n
&
(
  kfree \| kfree_sensitive \| kvfree \| kvfree_atomic \| free_percpu
)
)
  ) = ...;
)

@script:python depends on report@
p << free.p;
n << free.n;
@@

coccilib.report.print_report(
  p[0], f"ERROR: found `__cleanup({n})', do you mean `__free({n})'?")

@script:python depends on org@
p << free.p;
n << free.n;
@@

coccilib.org.print_todo(
  p[0], f"ERROR: found `__cleanup({n})', do you mean `__free({n})'?")


// Reporting __cleanup usage for the functions without a DEFINE_FREE definition.
// The following list only contains frequently used functions. Supplement it if
// necessary.
@cleanup@
attribute name __cleanup;
symbol vfree, vfree_atomic, kfree_const;
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
  vfree \| vfree_atomic \| kfree_const
)
)
  );
|
  T v __cleanup@p(
(
  n
&
(
  vfree \| vfree_atomic \| kfree_const
)
)
  ) = ...;
)

@script:python depends on report@
p << cleanup.p;
n << cleanup.n;
@@

coccilib.report.print_report(
  p[0], f"ERROR: `__cleanup({n})' will free the annotated variable, rather than its pointee")

@script:python depends on org@
p << cleanup.p;
n << cleanup.n;
@@

coccilib.org.print_todo(
  p[0], f"ERROR: `__cleanup({n})' will free the annotated variable, rather than its pointee")
