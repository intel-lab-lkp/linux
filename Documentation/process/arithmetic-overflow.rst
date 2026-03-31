.. SPDX-License-Identifier: GPL-2.0

.. _arithmetic_overflow:

Arithmetic Overflow Resolutions for Linux
=========================================

Background
----------

When a calculation's result exceeds the involved storage ranges, several
strategies can be followed to handle such an overflow (or underflow),
including:

  - Undefined (i.e. pretend it's impossible and the result depends on hardware)
  - Wrap around (this is what 2s-complement representation does by default)
  - Trap (create an exception so the problem can be handled in another way)
  - Saturate (explicitly hold the maximum or minimum representable value)

In the C standard, three basic types can be involved in arithmetic, and each
has a default strategy for solving the overflow problem:

  - Signed overflow is undefined
  - Unsigned overflow explicitly wraps around
  - Pointer overflow is undefined

The Linux kernel uses ``-fno-strict-overflow`` which implies ``-fwrapv``
and ``-fwrapv-pointer`` which treats signed integer overflow and pointer
overflow respectively as being consistent with two's complement. This flag
allows for consistency within the codebase about the expectations of
overflowing arithmetic as well as prevents eager compiler optimizations.
Note that :ref:`open-coded intentional arithmetic wrap-around is deprecated <open_coded_wrap_around>`.

From here on, arithmetic overflow concerning signed, unsigned, or
pointer types will be referred to as "wrap-around" since it is the
default strategy for the kernel. There is no such thing as "undefined
behavior" for arithmetic in Linux: it always wraps.

Overflow Behavior Types
-----------------------

The newly available ``__ob_trap`` and ``__ob_wrap`` annotations provide
fine-grained control over overflow behavior. These can be applied to
integer types to unambiguously specify how arithmetic operations are
expected to behave upon overflow. Currently, only Clang supports these
annotations. The annotation defines two possible overflow behaviors:

* ``wrap``: Ensures arithmetic operations wrap on overflow, providing
  well-defined two's complement semantics according to the bitwidth of the
  underlying type, regardless of any associated ``-fwrapv`` options or
  sanitizers (integer overflow and truncation checks are suppressed)
* ``trap``: Enables overflow and truncation checking for the type, even when
  associated ``-fwrapv`` options are enabled. Without the sanitizer enabled
  the compiler emits a trap instruction, otherwise the integer overflow
  and truncation warnings are emitted but execution continues.

Note that the sanitizer infrastructure is used for instrumentation shows
up in logs as the "Undefined Behavior" sanitizer (UBSan), which may be
confusing. Instead this should be thought of as the "Unexpected Behavior"
sanitizer. Its infrastructure is used to report on unexpected wrapping
behaviors: none of integer operations are "undefined" any more, as per
the use of ``-fno-strict-overflow``, but instead UBSan will kick in when
a type is explicitly marked as non-wrapping (i.e. trapping).


Enablement
~~~~~~~~~~

When supported by the compiler, kernels can build with either
``CONFIG_OVERFLOW_BEHAVIOR_TYPES_TRAP=y`` for trapping mode (i.e.
mitigation enabled), or ``CONFIG_OVERFLOW_BEHAVIOR_TYPES_WARN=y`` which
enables warn-only mode, which logs the overflows but will continue as
if the type was not marked with ``__ob_trap``.

Compiler Options
^^^^^^^^^^^^^^^^

Usage of the ``overflow_behavior`` attribute is gated behind the
``-fexperimental-overflow-behavior-types`` compiler flag which
is a ``-cc1`` flag, meaning the kernel passes it as ``-Xclang
-fexperimental-overflow-behavior-types``.

Sanitizer Case Lists
^^^^^^^^^^^^^^^^^^^^

Linux uses a Sanitizer Case List file to selectively enable certain
sanitizers for specific types. Specifically, the overflow and truncation
sanitizers have had their standard instrumentation disabled for all
types. To "allowlist" specific types for instrumentation the kernel
makes use of the in-source ``__ob_trap`` annotations to gain reporting
by the sanitizers.

Currently, type-based entries within a sanitizer case list are only
supported by Clang. For more information on the syntax for SCL files
refer to the Clang docs:
https://clang.llvm.org/docs/SanitizerSpecialCaseList.html

Syntax
~~~~~~

Creating Overflow Behavior Types is possible via two syntactic forms;

**Attribute syntax:**

.. code-block:: c

  typedef unsigned int __attribute__((overflow_behavior(trap))) safe_uint;
  typedef unsigned int __attribute__((overflow_behavior(wrap))) wrapping_uint;

**Keyword syntax:**

.. code-block:: c

  typedef unsigned int __ob_trap safe_uint;
  typedef unsigned int __ob_wrap wrapping_uint;

Both forms are semantically identical. The keyword syntax is shorter and can
appear in the same positions as ``const`` or ``volatile``. The attribute syntax
is more self-documenting, so Linux uses this form.

When ``-fexperimental-overflow-behavior-types`` is not enabled, both the
keywords (``__ob_wrap``, ``__ob_trap``) and the attribute are ignored with a
warning.

The feature can be queried with either
``__has_extension(overflow_behavior_types)`` or
``__has_attribute(overflow_behavior)``.

Basic Usage
^^^^^^^^^^^

.. code-block:: c

  typedef unsigned int __ob_wrap counter_t;
  typedef unsigned long __ob_trap safe_size_type;

  counter_t increment_counter(counter_t count) {
    return count + 1;
  }

  safe_size_type calculate_buffer_size(safe_size_type base,
                                       safe_size_type extra) {
    return base + extra;
  }

In the first function, arithmetic on ``counter_t`` is well-defined
wrapping. Its overflow will never be reported and unlike an plain
``unsigned int`` its purpose is unambiguous: it is expected to wrap
around. In the second function, arithmetic on ``safe_size_type`` is
checked -- overflow will result in a trap or sanitizer report depending
on the build configuration.

Variables can be annotated directly:

.. code-block:: c

  void foo(int num) {
    int __ob_trap a = num;
    a += 42;

    unsigned char __ob_wrap b = 255;
    b += 10;
  }


Interaction with Compiler Options
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Overflow behavior annotations override many global compiler flags and
sanitizer configurations:

* ``wrap`` types suppress UBSan's integer overflow checks
  (``signed-integer-overflow``, ``unsigned-integer-overflow``) and implicit
  truncation checks (``implicit-signed-integer-truncation``,
  ``implicit-unsigned-integer-truncation``). They also suppress ``-ftrapv``
  for the annotated type.
* ``trap`` types enable overflow checking even when ``-fwrapv``
  is globally enabled. When no sanitizer runtime is available, the compiler
  emits a trap instruction directly.
* Both forms override Sanitizer Special Case List (SSCL) entries.

Common overflow idioms are excluded from instrumentation via
``-fsanitize-undefined-ignore-overflow-pattern=``. These overflow idioms have
their instrumentation withheld even under the presence of overflow behavior
annotations. For more details see:
https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#disabling-instrumentation-for-common-overflow-patterns

Truncation Semantics
^^^^^^^^^^^^^^^^^^^^

Truncation and overflow are related -- both are often desirable in some
contexts and undesirable in others. Overflow behavior types control truncation
instrumentation at the type level as well:

* If a ``trap`` type is involved as source or destination of a truncation, the
  compiler inserts truncation checks. These will either trap or report via
  sanitizer depending on the build configuration.
* If a ``wrap`` type is involved as source or destination, truncation checks
  are suppressed regardless of compiler flags.
* If both ``trap`` and ``wrap`` are involved in the same truncation, ``wrap``
  takes precedence (truncation checks are suppressed) since the explicit
  wrapping intent covers truncation as well.

.. code-block:: c

  void checked(char a, int __ob_trap b) {
    a = b;
  }

  void wrapping(char a, int __ob_wrap b) {
    a = b;
  }


Promotion Rules
^^^^^^^^^^^^^^^

Overflow behavior types follow standard C integer promotion rules while
preserving the overflow behavior annotation through expressions:

* When an overflow behavior type is mixed with a standard integer type, the
  result carries the overflow behavior annotation. Standard C conversion rules
  determine the resulting width and signedness.
* When two overflow behavior types of the same kind (both ``wrap`` or both
  ``trap``) are mixed, the result follows standard C arithmetic conversion
  rules with that behavior applied.
* When ``wrap`` and ``trap`` are mixed, ``trap`` dominates. The result follows
  standard C conversions with ``trap`` behavior.

.. code-block:: c

  typedef int __ob_wrap wrap_int;
  typedef int __ob_trap trap_int;

  wrap_int a = 1;
  trap_int b = 2;
  /* a + b results in __ob_trap int (trap dominates) */


Type Compatibility
^^^^^^^^^^^^^^^^^^

Overflow behavior types are distinct from their underlying types for type
checking purposes. Assigning between types with different overflow behaviors
(``wrap`` vs ``trap``) is an error:

.. code-block:: c

  int __ob_wrap w;
  int __ob_trap t;
  w = t; /* error: incompatible overflow behavior types */

Assigning from an overflow behavior type to a plain integer type discards the
overflow behavior. The compiler can warn about this with
``-Wimplicit-overflow-behavior-conversion`` (implied by ``-Wconversion``).

Intentionally discarding the overflow behavior should use an explicit
cast:

.. code-block:: c

  unsigned long __ob_trap checked_size;
  unsigned long regular_size;

  regular_size = checked_size; /* warning: discards overflow behavior */
  regular_size = (unsigned long)checked_size; /* OK, explicit cast */

If truncation should be allowed for a cast away from ``trap``, an
explicit ``wrap`` cast is needed to suppress run-time instrumentation:

.. code-block:: c

  unsigned long __ob_trap checked_size;
  unsigned char small_size;

  small_size = checked_size; /* may trap at run-time on truncation */
  small_size = (unsigned long __ob_wrap)checked_size; /* OK, explicit cast */


Pointer Semantics
^^^^^^^^^^^^^^^^^

Pointers to overflow behavior types are treated as distinct from pointers to
their underlying types. Converting between them produces a warning controlled
by ``-Wincompatible-pointer-types-discards-overflow-behavior``:

.. code-block:: c

  unsigned long *px;
  unsigned long __ob_trap *py;

  px = py; /* warning: discards overflow behavior */
  py = px; /* warning: discards overflow behavior */


Best Practices
^^^^^^^^^^^^^^

1. **Use ``__ob_trap`` for sizes and counts** where overflow indicates bugs:

   .. code-block:: c

     typedef unsigned long long __ob_trap no_wrap_u64;
     no_wrap_u64 buffer_len = kmalloc_size + header_size;

2. **Use ``__ob_wrap`` for arithmetic that intentionally overflows**:

   .. code-block:: c

     typedef u32 __ob_wrap hash_t;
     hash_t hash = (hash * 31) + byte;

3. **Don't mix different overflow behavior types**:

   .. code-block:: c

    int __ob_wrap a;
    int __ob_trap b;

    a = b; /* error: incompatible overflow behavior types */

    a = (int __ob_wrap)b; /* OK, explicit cast */
    a = (int)b; /* OK, cast to underlying type */
