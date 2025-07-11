.. SPDX-License-Identifier: GPL-2.0

======================================
Kernel API Specification Framework
======================================

:Author: Sasha Levin <sashal@kernel.org>
:Date: June 2025

.. contents:: Table of Contents
   :depth: 3
   :local:

Introduction
============

The Kernel API Specification Framework (KAPI) provides a comprehensive system for
formally documenting, validating, and introspecting kernel APIs. This framework
addresses the long-standing challenge of maintaining accurate, machine-readable
documentation for the thousands of internal kernel APIs and system calls.

Purpose and Goals
-----------------

The framework aims to:

1. **Improve API Documentation**: Provide structured, inline documentation that
   lives alongside the code and is maintained as part of the development process.

2. **Enable Runtime Validation**: Optionally validate API usage at runtime to catch
   common programming errors during development and testing.

3. **Support Tooling**: Export API specifications in machine-readable formats for
   use by static analyzers, documentation generators, and development tools.

4. **Enhance Debugging**: Provide detailed API information at runtime through debugfs
   for debugging and introspection.

5. **Formalize Contracts**: Explicitly document API contracts including parameter
   constraints, execution contexts, locking requirements, and side effects.

Architecture Overview
=====================

Components
----------

The framework consists of several key components:

1. **Core Framework** (``kernel/api/kernel_api_spec.c``)

   - API specification registration and storage
   - Runtime validation engine
   - Specification lookup and querying

2. **DebugFS Interface** (``kernel/api/kapi_debugfs.c``)

   - Runtime introspection via ``/sys/kernel/debug/kapi/``
   - JSON and XML export formats
   - Per-API detailed information

3. **IOCTL Support** (``kernel/api/ioctl_validation.c``)

   - Extended framework for IOCTL specifications
   - Automatic validation wrappers
   - Structure field validation

4. **Specification Macros** (``include/linux/kernel_api_spec.h``)

   - Declarative macros for API documentation
   - Type-safe parameter specifications
   - Context and constraint definitions

Data Model
----------

The framework uses a hierarchical data model::

    kernel_api_spec
    ├── Basic Information
    │   ├── name (API function name)
    │   ├── version (specification version)
    │   ├── description (human-readable description)
    │   └── kernel_version (when API was introduced)
    │
    ├── Parameters (up to 16)
    │   └── kapi_param_spec
    │       ├── name
    │       ├── type (int, pointer, string, etc.)
    │       ├── direction (in, out, inout)
    │       ├── constraints (range, mask, enum values)
    │       └── validation rules
    │
    ├── Return Value
    │   └── kapi_return_spec
    │       ├── type
    │       ├── success conditions
    │       └── validation rules
    │
    ├── Error Conditions (up to 32)
    │   └── kapi_error_spec
    │       ├── error code
    │       ├── condition description
    │       └── recovery advice
    │
    ├── Execution Context
    │   ├── allowed contexts (process, interrupt, etc.)
    │   ├── locking requirements
    │   └── preemption/interrupt state
    │
    └── Side Effects
        ├── memory allocation
        ├── state changes
        └── signal handling

Usage Guide
===========

Basic API Specification
-----------------------

To document a kernel API, use the specification macros in the implementation file:

.. code-block:: c

    #include <linux/kernel_api_spec.h>

    KAPI_DEFINE_SPEC(kmalloc_spec, kmalloc, "3.0")
    KAPI_DESCRIPTION("Allocate kernel memory")
    KAPI_PARAM(0, size, KAPI_TYPE_SIZE_T, KAPI_DIR_IN,
               "Number of bytes to allocate")
    KAPI_PARAM_RANGE(0, 0, KMALLOC_MAX_SIZE)
    KAPI_PARAM(1, flags, KAPI_TYPE_FLAGS, KAPI_DIR_IN,
               "Allocation flags (GFP_*)")
    KAPI_PARAM_MASK(1, __GFP_BITS_MASK)
    KAPI_RETURN(KAPI_TYPE_POINTER, "Pointer to allocated memory or NULL")
    KAPI_ERROR(ENOMEM, "Out of memory")
    KAPI_CONTEXT(KAPI_CTX_PROCESS | KAPI_CTX_SOFTIRQ | KAPI_CTX_HARDIRQ)
    KAPI_SIDE_EFFECT("Allocates memory from kernel heap")
    KAPI_LOCK_NOT_REQUIRED("Any lock")
    KAPI_END_SPEC

    void *kmalloc(size_t size, gfp_t flags)
    {
        /* Implementation */
    }

System Call Specification
-------------------------

System calls use specialized macros:

.. code-block:: c

    KAPI_DEFINE_SYSCALL_SPEC(open_spec, open, "1.0")
    KAPI_DESCRIPTION("Open a file")
    KAPI_PARAM(0, pathname, KAPI_TYPE_USER_STRING, KAPI_DIR_IN,
               "Path to file")
    KAPI_PARAM_PATH(0, PATH_MAX)
    KAPI_PARAM(1, flags, KAPI_TYPE_FLAGS, KAPI_DIR_IN,
               "Open flags (O_*)")
    KAPI_PARAM(2, mode, KAPI_TYPE_MODE_T, KAPI_DIR_IN,
               "File permissions (if creating)")
    KAPI_RETURN(KAPI_TYPE_INT, "File descriptor or -1")
    KAPI_ERROR(EACCES, "Permission denied")
    KAPI_ERROR(ENOENT, "File does not exist")
    KAPI_ERROR(EMFILE, "Too many open files")
    KAPI_CONTEXT(KAPI_CTX_PROCESS | KAPI_CTX_SLEEPABLE)
    KAPI_SIGNAL(EINTR, "Open can be interrupted by signal")
    KAPI_END_SYSCALL_SPEC

IOCTL Specification
-------------------

IOCTLs have extended support for structure validation:

.. code-block:: c

    KAPI_DEFINE_IOCTL_SPEC(vidioc_querycap_spec, VIDIOC_QUERYCAP,
                           "VIDIOC_QUERYCAP",
                           sizeof(struct v4l2_capability),
                           sizeof(struct v4l2_capability),
                           "video_fops")
    KAPI_DESCRIPTION("Query device capabilities")
    KAPI_IOCTL_FIELD(driver, KAPI_TYPE_CHAR_ARRAY, KAPI_DIR_OUT,
                     "Driver name", 16)
    KAPI_IOCTL_FIELD(card, KAPI_TYPE_CHAR_ARRAY, KAPI_DIR_OUT,
                     "Device name", 32)
    KAPI_IOCTL_FIELD(version, KAPI_TYPE_U32, KAPI_DIR_OUT,
                     "Driver version")
    KAPI_IOCTL_FIELD(capabilities, KAPI_TYPE_FLAGS, KAPI_DIR_OUT,
                     "Device capabilities")
    KAPI_END_IOCTL_SPEC

Runtime Validation
==================

Enabling Validation
-------------------

Runtime validation is controlled by kernel configuration:

1. Enable ``CONFIG_KAPI_SPEC`` to build the framework
2. Enable ``CONFIG_KAPI_RUNTIME_CHECKS`` for runtime validation
3. Optionally enable ``CONFIG_KAPI_SPEC_DEBUGFS`` for debugfs interface

Validation Modes
----------------

The framework supports several validation modes:

.. code-block:: c

    /* Enable validation for specific API */
    kapi_enable_validation("kmalloc");

    /* Enable validation for all APIs */
    kapi_enable_all_validation();

    /* Set validation level */
    kapi_set_validation_level(KAPI_VALIDATE_FULL);

Validation Levels:

- ``KAPI_VALIDATE_NONE``: No validation
- ``KAPI_VALIDATE_BASIC``: Type and NULL checks only
- ``KAPI_VALIDATE_NORMAL``: Basic + range and constraint checks
- ``KAPI_VALIDATE_FULL``: All checks including custom validators

Custom Validators
-----------------

APIs can register custom validation functions:

.. code-block:: c

    static bool validate_buffer_size(const struct kapi_param_spec *spec,
                                     const void *value, void *context)
    {
        size_t size = *(size_t *)value;
        struct my_context *ctx = context;

        return size > 0 && size <= ctx->max_buffer_size;
    }

    KAPI_PARAM_CUSTOM_VALIDATOR(0, validate_buffer_size)

DebugFS Interface
=================

The debugfs interface provides runtime access to API specifications:

Directory Structure
-------------------

::

    /sys/kernel/debug/kapi/
    ├── apis/                    # All registered APIs
    │   ├── kmalloc/
    │   │   ├── specification   # Human-readable spec
    │   │   ├── json           # JSON format
    │   │   └── xml            # XML format
    │   └── open/
    │       └── ...
    ├── summary                  # Overview of all APIs
    ├── validation/              # Validation controls
    │   ├── enabled             # Global enable/disable
    │   ├── level               # Validation level
    │   └── stats               # Validation statistics
    └── export/                  # Bulk export options
        ├── all.json            # All specs in JSON
        └── all.xml             # All specs in XML

Usage Examples
--------------

Query specific API::

    $ cat /sys/kernel/debug/kapi/apis/kmalloc/specification
    API: kmalloc
    Version: 3.0
    Description: Allocate kernel memory

    Parameters:
      [0] size (size_t, in): Number of bytes to allocate
          Range: 0 - 4194304
      [1] flags (flags, in): Allocation flags (GFP_*)
          Mask: 0x1ffffff

    Returns: pointer - Pointer to allocated memory or NULL

    Errors:
      ENOMEM: Out of memory

    Context: process, softirq, hardirq

    Side Effects:
      - Allocates memory from kernel heap

Export all specifications::

    $ cat /sys/kernel/debug/kapi/export/all.json > kernel-apis.json

Enable validation for specific API::

    $ echo 1 > /sys/kernel/debug/kapi/apis/kmalloc/validate

Performance Considerations
==========================

Memory Overhead
---------------

Each API specification consumes approximately 2-4KB of memory. With thousands
of kernel APIs, this can add up to several megabytes. Consider:

1. Building with ``CONFIG_KAPI_SPEC=n`` for production kernels
2. Using ``__init`` annotations for APIs only used during boot
3. Implementing lazy loading for rarely used specifications

Runtime Overhead
----------------

When ``CONFIG_KAPI_RUNTIME_CHECKS`` is enabled:

- Each validated API call adds 50-200ns overhead
- Complex validations (custom validators) may add more
- Use validation only in development/testing kernels

Optimization Strategies
-----------------------

1. **Compile-time optimization**: When validation is disabled, all
   validation code is optimized away by the compiler.

2. **Selective validation**: Enable validation only for specific APIs
   or subsystems under test.

3. **Caching**: The framework caches validation results for repeated
   calls with identical parameters.

Documentation Generation
------------------------

The framework exports specifications via debugfs that can be used
to generate documentation. Tools for automatic documentation generation
from specifications are planned for future development.

IDE Integration
---------------

Modern IDEs can use the JSON export for:

- Parameter hints
- Type checking
- Context validation
- Error code documentation

Testing Framework
-----------------

The framework includes test helpers::

    #ifdef CONFIG_KAPI_TESTING
    /* Verify API behaves according to specification */
    kapi_test_api("kmalloc", test_cases);
    #endif

Best Practices
==============

Writing Specifications
----------------------

1. **Be Comprehensive**: Document all parameters, errors, and side effects
2. **Keep Updated**: Update specs when API behavior changes
3. **Use Examples**: Include usage examples in descriptions
4. **Validate Constraints**: Define realistic constraints for parameters
5. **Document Context**: Clearly specify allowed execution contexts

Maintenance
-----------

1. **Version Specifications**: Increment version when API changes
2. **Deprecation**: Mark deprecated APIs and suggest replacements
3. **Cross-reference**: Link related APIs in descriptions
4. **Test Specifications**: Verify specs match implementation

Common Patterns
---------------

**Optional Parameters**::

    KAPI_PARAM(2, optional_arg, KAPI_TYPE_POINTER, KAPI_DIR_IN,
               "Optional argument (may be NULL)")
    KAPI_PARAM_OPTIONAL(2)

**Variable Arguments**::

    KAPI_PARAM(1, fmt, KAPI_TYPE_FORMAT_STRING, KAPI_DIR_IN,
               "Printf-style format string")
    KAPI_PARAM_VARIADIC(2, "Format arguments")

**Callback Functions**::

    KAPI_PARAM(1, callback, KAPI_TYPE_FUNCTION_PTR, KAPI_DIR_IN,
               "Callback function")
    KAPI_PARAM_CALLBACK(1, "int (*)(void *data)", "data")

Troubleshooting
===============

Common Issues
-------------

**Specification Not Found**::

    kernel: KAPI: Specification for 'my_api' not found

    Solution: Ensure KAPI_DEFINE_SPEC is in the same translation unit
    as the function implementation.

**Validation Failures**::

    kernel: KAPI: Validation failed for kmalloc parameter 'size':
            value 5242880 exceeds maximum 4194304

    Solution: Check parameter constraints or adjust specification if
    the constraint is incorrect.

**Build Errors**::

    error: 'KAPI_TYPE_UNKNOWN' undeclared

    Solution: Include <linux/kernel_api_spec.h> and ensure
    CONFIG_KAPI_SPEC is enabled.

Debug Options
-------------

Enable verbose debugging::

    echo 8 > /proc/sys/kernel/printk
    echo 1 > /sys/kernel/debug/kapi/debug/verbose

Future Directions
=================

Planned Features
----------------

1. **Automatic Extraction**: Tool to extract specifications from existing
   kernel-doc comments

2. **Contract Verification**: Static analysis to verify implementation
   matches specification

3. **Performance Profiling**: Measure actual API performance against
   documented expectations

4. **Fuzzing Integration**: Use specifications to guide intelligent
   fuzzing of kernel APIs

5. **Version Compatibility**: Track API changes across kernel versions

Research Areas
--------------

1. **Formal Verification**: Use specifications for mathematical proofs
   of correctness

2. **Runtime Monitoring**: Detect specification violations in production
   with minimal overhead

3. **API Evolution**: Analyze how kernel APIs change over time

4. **Security Applications**: Use specifications for security policy
   enforcement

Contributing
============

Submitting Specifications
-------------------------

1. Add specifications to the same file as the API implementation
2. Follow existing patterns and naming conventions
3. Test with CONFIG_KAPI_RUNTIME_CHECKS enabled
4. Verify debugfs output is correct
5. Run scripts/checkpatch.pl on your changes

Review Criteria
---------------

Specifications will be reviewed for:

1. **Completeness**: All parameters and errors documented
2. **Accuracy**: Specification matches implementation
3. **Clarity**: Descriptions are clear and helpful
4. **Consistency**: Follows framework conventions
5. **Performance**: No unnecessary runtime overhead

Contact
-------

- Maintainer: Sasha Levin <sashal@kernel.org>
