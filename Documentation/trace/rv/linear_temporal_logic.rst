Introduction
============

Deterministic automaton runtime verification monitor is a verification technique which checks that
the kernel follows a specification in the form of deterministic automaton (DA). It does so by using
tracepoints to monitor the kernel's execution trace, and verifying that the execution trace
sastifies the specification.

However, while attempting to implement DA monitors for some complex specifications, deterministic
automaton is found to be inappropriate as the specification language. The automaton is complicated,
hard to understand, and error-prone.

Thus, RV monitors based on linear temporal logic (LTL for short) are introduced. This type of
monitor uses LTL as specification, instead of DA. For some cases, writing the specification as LTL
is more concise and intuitive.

Documents regarding LTL are widely available on the internet, this document will not go into
details.

Grammar
========

Unlike some existing syntax, kernel's implementation of LTL is more verbose. This is motivated by
considering that the people who read the LTL specifications may not be well-versed in LTL.

Grammar:
    ltl ::= opd | ( ltl ) | ltl binop ltl | unop ltl

Operands (opd):
    true, false, user-defined names consisting of upper-case characters, digits, and underscore.

Unary Operators (unop):
    always
    eventually
    not

Binary Operators (binop):
    until
    and
    or
    imply
    equivalent

This grammar is ambiguous: operator precedence is not defined. Parentheses must be used.

Monitor synthesis
=================

To synthesize an LTL into a kernel monitor, conversion scripts are provided:
`tools/verification/ltl2ba`. The specification needs to be provided as a file, and it must have a
"RULE = LTL" assignment, which specifies the LTL property to verify. For example:

   .. code-block::

    RULE = always (ACQUIRE imply ((not KILLED and not CRASHED) until RELEASE))

The LTL can be broken down if required using sub-expressions. For example, the above is equivalent
to:

   .. code-block::

    RULE = always (ACQUIRE imply (ALIVE until RELEASE))
    ALIVE = not KILLED and not CRASHED

The ltl file can be converted into C code:

   .. code-block::

    .tools/verification/ltl2ba/generate.py -l <ltl file> -n <model name> -o <output diretory>

The above command generates `ba.c` and `ba.h`, the Buchi automaton that verifies the LTL property.
The Buchi automaton needs to be manually glued to the kernel. Please see the comments in `ba.h` for
further details.
