.. SPDX-License-Identifier: GPL-2.0
.. Copyright 2025 Google LLC

=========================================
Kernel Fuzz Testing Framework (KFuzzTest)
=========================================

Overview
========

The Kernel Fuzz Testing Framework (KFuzzTest) is a framework designed to expose
internal kernel functions to a userspace fuzzing engine.

It is intended for testing stateless or low-state functions that are difficult
to reach from the system call interface, such as routines involved in file
format parsing or complex data transformations. This provides a method for
in-situ fuzzing of kernel code without requiring that it be built as a separate
userspace library or that its dependencies be stubbed out.

The framework consists of four main components:

1.  An API, based on the ``FUZZ_TEST`` macro, for defining test targets
    directly in the kernel tree.
2.  A binary serialization format for passing complex, pointer-rich data
    structures from userspace to the kernel.
3.  A ``debugfs`` interface through which a userspace fuzzer submits
    serialized test inputs.
4.  Metadata embedded in dedicated ELF sections of the ``vmlinux`` binary to
    allow for the discovery of available fuzz targets by external tooling.

.. warning::
   KFuzzTest is a debugging and testing tool. It exposes internal kernel
   functions to userspace with minimal sanitization and is designed for
   use in controlled test environments only. It must **NEVER** be enabled
   in production kernels.

Supported Architectures
=======================

KFuzzTest is designed for generic architecture support. It has only been
explicitly tested on x86_64.

Usage
=====

To enable KFuzzTest, configure the kernel with::

	CONFIG_KFUZZTEST=y

which depends on ``CONFIG_DEBUGFS`` for receiving userspace inputs, and
``CONFIG_DEBUG_KERNEL`` as an additional guardrail for preventing KFuzzTest
from finding its way into a production build accidentally.

The KFuzzTest sample fuzz targets can be built in with
``CONFIG_SAMPLE_KFUZZTEST``.

KFuzzTest currently only supports targets that are built into the kernel, as the
core module's startup process discovers fuzz targets from a dedicated ELF
section during startup. Furthermore, constraints and annotations emit metadata
that can be scanned from a ``vmlinux`` binary by a userspace fuzzing engine.

Declaring a KFuzzTest target
----------------------------

A fuzz target should be defined in a .c file. The recommended place to define
this is under the subsystem's ``/tests`` directory in a ``<file-name>_kfuzz.c``
file, following the convention used by KUnit. The only strict requirement is
that the function being fuzzed is visible to the fuzz target.

Defining a fuzz target involves three main parts: defining an input structure,
writing the test body using the ``FUZZ_TEST`` macro, and optionally adding
metadata for the fuzzer.

The following example illustrates how to create a fuzz target for a function
``int process_data(const char *data, size_t len)``.

.. code-block:: c

	/*
	 * 1. Define a struct to model the inputs for the function under test.
	 *    Each field corresponds to an argument needed by the function.
	 */
	struct process_data_inputs {
		const char *data;
		size_t len;
	};

	/*
	 * 2. Define the fuzz target using the FUZZ_TEST macro.
	 *    The first parameter is a unique name for the target.
	 *    The second parameter is the input struct defined above.
	 */
	FUZZ_TEST(test_process_data, struct process_data_inputs)
	{
		/*
		 * Within this body, the 'arg' variable is a pointer to a
		 * fully initialized 'struct process_data_inputs'.
		 */

		/*
		 * 3. (Optional) Add constraints to define preconditions.
		 *    This check ensures 'arg->data' is not NULL. If the condition
		 *    is not met, the test exits early. This also creates metadata
		 *    to inform the fuzzer.
		 */
		KFUZZTEST_EXPECT_NOT_NULL(process_data_inputs, data);

		/*
		 * 4. (Optional) Add annotations to provide semantic hints to the
		 *    fuzzer. This annotation informs the fuzzer that the 'len' field is
		 *    the length of the buffer pointed to by 'data'. Annotations do not
		 *    add any runtime checks.
		 */
		KFUZZTEST_ANNOTATE_LEN(process_data_inputs, len, data);

		/*
		 * 5. Call the kernel function with the provided inputs.
		 *    Memory errors like out-of-bounds accesses on 'arg->data' will
		 *    be detected by KASAN or other memory error detection tools.
		 */
		process_data(arg->data, arg->len);
	}

KFuzzTest provides two families of macros to improve the quality of fuzzing:

- ``KFUZZTEST_EXPECT_*``: These macros define constraints, which are
  preconditions that must be true for the test to proceed. They are enforced
  with a runtime check in the kernel. If a check fails, the current test run is
  aborted. This metadata helps the userspace fuzzer avoid generating invalid
  inputs.

- ``KFUZZTEST_ANNOTATE_*``: These macros define annotations, which are purely
  semantic hints for the fuzzer. They do not add any runtime checks and exist
  only to help the fuzzer generate more intelligent and structurally correct
  inputs. For example, KFUZZTEST_ANNOTATE_LEN links a size field to a pointer
  field, which is a common pattern in C APIs.

Metadata
--------

Macros ``FUZZ_TEST``, ``KFUZZTEST_EXPECT_*`` and ``KFUZZTEST_ANNOTATE_*`` embed
metadata into several sections within the main ``.data`` section of the final
``vmlinux`` binary; ``.kfuzztest_target``, ``.kfuzztest_constraint`` and
``.kfuzztest_annotation`` respectively.

This serves two purposes:

1. The core module uses the ``.kfuzztest_target`` section at boot to discover
   every ``FUZZ_TEST`` instance and create its ``debugfs`` directory and
   ``input`` file.
2. Userspace fuzzers can read this metadata from the ``vmlinux`` binary to
   discover targets and learn about their rules and structure in order to
   generate correct and effective inputs.

The metadata in the ``.kfuzztest_*`` sections consists of arrays of fixed-size C
structs (e.g., ``struct kfuzztest_target``). Fields within these structs that
are pointers, such as ``name`` or ``arg_type_name``, contain addresses that
point to other locations in the ``vmlinux`` binary. A userspace tool that
parsing the ELF file must resolve these pointers to read the data that they
reference. For example, to get a target's name, a tool must:

1. Read the ``struct kfuzztest_target`` from the ``.kfuzztest_target`` section.
2. Read the address in the ``.name`` field.
3. Use that address to locate and read null-terminated string from its position
   elsewhere in the binary (e.g., ``.rodata``).

Tooling Dependencies
--------------------

For userspace tools to parse the ``vmlinux`` binary and make use of emitted
KFuzzTest metadata, the kernel must be compiled with DWARF debug information.
This is required for tools to understand the layout of C structs, resolve type
information, and correctly interpret constraints and annotations.

When using KFuzzTest with automated fuzzing tools, either
``CONFIG_DEBUG_INFO_DWARF4`` or ``CONFIG_DEBUG_INFO_DWARF5`` should be enabled.

Input Format
============

KFuzzTest targets receive their inputs from userspace via a write to a dedicated
debugfs file ``/sys/kernel/debug/kfuzztest/<test-name>/input``.

The data written to this file must be a single binary blob that follows a
specific serialization format. This format is designed to allow complex,
pointer-rich C structures to be represented in a flat buffer, requiring only a
single kernel allocation and copy from userspace.

An input is first prefixed by an 8-byte header containing a magic value in the
first four bytes, defined as ``KFUZZTEST_HEADER_MAGIC`` in
`<include/linux/kfuzztest.h>``, and a version number in the subsequent four
bytes.

Version 0
---------

In version 0 (i.e., when the version number in the 8-byte header is equal to 0),
the input format consists of three main parts laid out sequentially: a region
array, a relocation table, and the payload.::

    +----------------+---------------------+-----------+----------------+
    |  region array  |  relocation table   |  padding  |    payload     |
    +----------------+---------------------+-----------+----------------+

Region Array
^^^^^^^^^^^^

This component is a header that describes how the raw data in the Payload is
partitioned into logical memory regions. It consists of a count of regions
followed by an array of ``struct reloc_region``, where each entry defines a
single region with its size and offset from the start of the payload.

.. code-block:: c

	struct reloc_region {
		uint32_t offset;
		uint32_t size;
	};

	struct reloc_region_array {
		uint32_t num_regions;
		struct reloc_region regions[];
	};

By convention, region 0 represents the top-level input struct that is passed
as the arg variable to the ``FUZZ_TEST`` body. Subsequent regions typically
represent data buffers or structs pointed to by fields within that struct.
Region array entries must be ordered by ascending offset, and must not overlap
with one another.

Relocation Table
^^^^^^^^^^^^^^^^

The relocation table contains the instructions for the kernel to "hydrate" the
payload by patching pointer fields. It contains an array of
``struct reloc_entry`` items. Each entry acts as a linking instruction,
specifying:

- The location of a pointer that needs to be patched (identified by a region
  ID and an offset within that region).

- The target region that the pointer should point to (identified by the
  target's region ID) or ``KFUZZTEST_REGIONID_NULL`` if the pointer is ``NULL``.

This table also specifies the amount of padding between its end and the start
of the payload, which should be at least 8 bytes.

.. code-block:: c

	struct reloc_entry {
		uint32_t region_id;
		uint32_t region_offset;
		uint32_t value;
	};

	struct reloc_table {
		uint32_t num_entries;
		uint32_t padding_size;
		struct reloc_entry entries[];
    };

Payload
^^^^^^^

The payload contains the raw binary data for all regions, concatenated together
according to their specified offsets.

- Region specific alignment: The data for each individual region must start at
  an offset that is aligned to its own C type's requirements. For example, a
  ``uint64_t`` must begin on an 8-byte boundary.

- Minimum alignment: The offset of each region, as well as the beginning of the
  payload, must also be a multiple of the overall minimum alignment value. This
  value is determined by the greater of ``ARCH_KMALLOC_MINALIGN`` and
  ``KASAN_GRANULE_SIZE`` (which is represented by ``KFUZZTEST_POISON_SIZE`` in
  ``/include/linux/kfuzztest.h``). This minimum alignment ensures that all
  function inputs respect C calling conventions.

- Padding: The space between the end of one region's data and the beginning of
  the next must be sufficient for padding. The padding must also be at least
  the same minimum alignment value mentioned above. This is crucial for KASAN
  builds, as it allows KFuzzTest to poison this unused space enabling precise
  detection of out-of-bounds memory accesses between adjacent buffers.

The minimum alignment value is architecture-dependent and is exposed to
userspace via the read-only file
``/sys/kernel/debug/kfuzztest/_config/minalign``. The framework relies on
userspace tooling to construct the payload correctly, adhering to all three of
these rules for every region.

KFuzzTest Bridge Tool
=====================

The ``kfuzztest-bridge`` program is a userspace utility that encodes a random
byte stream into the structured binary format expected by a KFuzzTest harness.
It allows users to describe the target's input structure textually, making it
easy to perform smoke tests or connect harnesses to blob-based fuzzing engines.

This tool is intended to be simple, both in usage and implementation. Its
structure and DSL are sufficient for simpler use-cases. For more advanced
coverage-guided fuzzing it is recommended to use
`syzkaller <https://github.com/google/syzkaller>` which implements deeper
support for KFuzzTest targets.

Usage
-----

The tool can be built with ``make tools/testing/kfuzztest-bridge``. In the case
of libc incompatibilities, the tool will have to be linked statically or built
on the target system.

Example:

.. code-block:: sh

    ./tools/testing/kfuzztest-bridge \
        "foo { u32 ptr[bar] }; bar { ptr[data] len[data, u64]}; data { arr[u8, 42] };" \
        "my-fuzz-target" /dev/urandom

The command takes three arguments

1.  A string describing the input structure (see `Textual Format`_ sub-section).
2.  The name of the target test, which corresponds to its directory in
    ``/sys/kernel/debug/kfuzztest/``.
3.  A path to a file providing a stream of random data, such as
    ``/dev/urandom``.

The structure string in the example corresponds to the following C data
structures:

.. code-block:: c

	struct foo {
		u32 a;
		struct bar *b;
	};

	struct bar {
		struct data *d;
		u64 data_len; /* Equals 42. */
	};

	struct data {
		char arr[42];
	};

Textual Format
--------------

The textual format is a human-readable representation of the region-based binary
format used by KFuzzTest. It is described by the following grammar:

.. code-block:: text

	schema     ::= region ( ";" region )* [";"]
	region     ::= identifier "{" type ( " " type )* "}"
	type       ::= primitive | pointer | array | length | string
	primitive  ::= "u8" | "u16" | "u32" | "u64"
	pointer    ::= "ptr" "[" identifier "]"
	array      ::= "arr" "[" primitive "," integer "]"
	length     ::= "len" "[" identifier "," primitive "]"
	string     ::= "str" "[" integer "]"
	identifier ::= [a-zA-Z_][a-zA-Z1-9_]*
	integer    ::= [0-9]+

Pointers must reference a named region.

To fuzz a raw buffer, the buffer must be defined in its own region, as shown
below:

.. code-block:: c

	struct my_struct {
		char *buf;
		size_t buflen;
	};

This would correspond to the following textual description:

.. code-block:: text

	my_struct { ptr[buf] len[buf, u64] }; buf { arr[u8, n] };

Here, ``n`` is some integer value defining the size of the byte array inside of
the ``buf`` region.
