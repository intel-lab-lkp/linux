=================
/proc/sys/crypto/
=================

Currently, these files might (depending on your configuration)
show up in ``/proc/sys/crypto/``:

.. contents:: :local:

fips_enabled
============

This file contains a read-only flag that indicates whether FIPS mode is
enabled.

- ``0``: FIPS mode is disabled (default).
- ``1``: FIPS mode is enabled.

This value is set at boot time via the ``fips=1`` kernel command line
parameter. When enabled, the cryptographic API will restrict the use
of certain algorithms and perform self-tests to ensure compliance with
FIPS (Federal Information Processing Standards) requirements, such as
FIPS 140-2 and the newer FIPS 140-3, depending on the kernel
configuration and the module in use.

fips_name
=========

This read-only file contains the name of the FIPS module currently in use.
The value is typically configured via the ``CONFIG_CRYPTO_FIPS_NAME``
kernel configuration option.

fips_version
============

This read-only file contains the version string of the FIPS module.
If ``CONFIG_CRYPTO_FIPS_CUSTOM_VERSION`` is set, it uses the value from
``CONFIG_CRYPTO_FIPS_VERSION``. Otherwise, it defaults to the kernel
release version (``UTS_RELEASE``).

Copyright (c) 2026, Shubham Chakraborty <chakrabortyshubham66@gmail.com>

For general info and legal blurb, please look in
Documentation/admin-guide/sysctl/index.rst.

.. See scripts/check-sysctl-docs to keep this up to date:
.. scripts/check-sysctl-docs -vtable="crypto" \
..         Documentation/admin-guide/sysctl/crypto.rst \
..         $(git grep -l register_sysctl_)
