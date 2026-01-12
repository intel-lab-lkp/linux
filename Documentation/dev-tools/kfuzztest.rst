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

The framework consists of two main components:

1.  An API, based on the ``FUZZ_TEST_SIMPLE`` macro, for defining test targets
    directly in the kernel tree.
2.  A ``debugfs`` interface through which a userspace fuzzer submits raw
    binary test inputs.

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
section during startup.

Defining a KFuzzTest target
---------------------------

A fuzz target should be defined in a .c file. The recommended place to define
this is under the subsystem's ``/tests`` directory in a ``<file-name>_kfuzz.c``
file, following the convention used by KUnit. The only strict requirement is
that the function being fuzzed is visible to the fuzz target.

Use the ``FUZZ_TEST_SIMPLE`` macro to define a fuzz target. This macro is
designed for functions that accept a buffer and its length (e.g.,
``(const char *data, size_t datalen)``).

This macro provides ``data`` and ``datalen`` variables implicitly to the test
body.

.. code-block:: c

	/* 1. The kernel function that we want to fuzz. */
	int process_data(const char *data, size_t len);

	/* 2. Define the fuzz target with the FUZZ_TEST_SIMPLE macro. */
	FUZZ_TEST_SIMPLE(test_process_data)
	{
		/* 3. Call the kernel function with the provided input. */
		process_data(data, datalen);
	}

A ``FUZZ_TEST_SIMPLE`` target creates a debugfs directory
(``/sys/kernel/debug/kfuzztest/<test-name>``) containing a single write-only
file ``input_simple``: writing a raw blob to this file will invoke the fuzz
target, passing the blob as ``(data, datalen)``.

Basic Usage
^^^^^^^^^^^

Because the interface accepts raw binary data, targets can be smoke-tested or
fuzzed naively using standard command-line tools without any external
dependencies.

For example, to feed 128 bytes of random data to the target defined above:

.. code-block:: sh

   head -c 128 /dev/urandom > \
       /sys/kernel/debug/kfuzztest/test_process_data/input_simple

Integration with Fuzzers
^^^^^^^^^^^^^^^^^^^^^^^^

The simple interface makes it easy to integrate with userspace fuzzers (e.g.,
LibFuzzer, AFL++, honggfuzz). A LibFuzzer, for example, harness may look like
so:

.. code-block:: c

    /* Path to the simple target's input file */
    const char *filepath = "/sys/kernel/debug/kfuzztest/test_process_data/input_simple";

    extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
        FILE *f = fopen(filepath, "w");
        if (!f) {
            return 0; /* Fuzzer should not stop. */
        }
        /* Write the raw fuzzer input directly. */
        fwrite(Data, 1, Size, f);
        fclose(f);
        return 0;
    }

Note that while it is simple to feed inputs to KFuzzTest targets, kernel
coverage collection is key for the effectiveness of a coverage-guided fuzzer;
setup of KCOV or other coverage mechanisms is outside of KFuzzTest's scope.

Metadata
--------

The ``FUZZ_TEST_SIMPLE`` macro embeds metadata into a dedicated section within
the main ``.data`` section of the final ``vmlinux`` binary:
``.kfuzztest_simple_target``, delimited by ``__kfuzztest_simple_targets_start``
and ``__kfuzztest_simple_targets_end``.

The metadata serves two purposes:

1. The core module uses the ``.kfuzztest_simple_target`` section at boot to
   discover every test instance and create its ``debugfs`` directory and
   ``input_simple`` file.
2. Tooling can use this section for offline discovery. While available fuzz
   targets can be trivially enumerated at runtime by listing the directories
   under ``/sys/kernel/debug/kfuzztest``, the metadata allows fuzzing
   orchestrators to index available fuzz targets directly from the ``vmlinux``
   binary without needing to boot the kernel.

This metadata consists of an array of ``struct kfuzztest_simple_target``. The
``name`` field within this struct references data in other locations of the
``vmlinux`` binary, and therefore a userspace tool that parses the ELF must
resolve these pointers to read the underlying data.
