.. SPDX-License-Identifier: GPL-2.0

=======================================================
PCI Express Lane Margining at Receiver (LMR) Subsystem
=======================================================

:Author: Priyank Rathod <rathodpriyank@google.com>
:Copyright: 2026 Google LLC

Overview
========

Lane Margining at Receiver (LMR), specified in the PCI Express Base
Specification (r6.0+ sec 8.4.4), allows system software to evaluate high-speed
link physical signal integrity and eye margins. LMR measures available timing
(jitter/phase) and voltage margin offsets for each physical lane and receiver
independently while the link is operating in active L0 state.

Lane Margining Extended Capability (ID 0x27) is optional for links operating at
16.0 GT/s (PCIe Gen 4) and 32.0 GT/s (Gen 5), and is mandatory for receivers
operating at 64.0 GT/s (Gen 6) and higher.

Target Receivers
================

Each physical lane can margin up to 7 distinct receivers per PCIe link:

* **Receiver 0 (Local Receiver)**: The receiver in the immediate link partner.
* **Receivers 1 to 6 (Retimers)**: Retimer pseudo-ports along the physical link
  (up to 3 retimers, each with upstream and downstream pseudo-ports).
* **Receiver 7**: Reserved per PCIe Base Specification.

Kernel Configuration
====================

Enable the kernel configuration option under PCI support:

.. code-block:: none

   CONFIG_PCIE_LMR=y (or =m)

Dependencies:
* ``CONFIG_PCI``
* ``CONFIG_DEBUG_FS``

Debugfs Interface Guide
=======================

When an LMR-capable device is enumerated on a Gen4+ link, the kernel exposes
per-device control and status files under debugfs:

.. code-block:: none

   /sys/kernel/debug/pci/pcie_lmr_<domain>:<bus>:<dev>.<func>/

Device-Level Attributes
-----------------------

* ``capabilities`` (read-only):
  Displays the 16-bit Margining Port Capabilities register and whether the
  device uses the Software Ready handshake bit.

* ``port_status`` (read-only):
  Displays the Margining Port Status register, indicating Margining Ready and
  SW Ready states.

* ``enable`` (read-write):
  Enables (``1``) or disables (``0``) Lane Margining on the device.
  Enabling margining locks the link into D0, prevents runtime PM suspend,
  disables ASPM L0s/L1, and verifies that the link is operating at >= 16.0 GT/s.
  Disabling margining restores ASPM and runtime PM, and returns all lanes to
  nominal (demargined) state.

Lane-Level Attributes
---------------------

For each physical lane (``lane0``, ``lane1``, ...):

* ``receiver`` (read-write):
  Gets or sets the active target receiver number (``0`` for local receiver,
  ``1..6`` for retimers). Switching receivers automatically demargins previous
  offsets per PCIe single-receiver margining requirements.

* ``caps`` (read-only):
  Reports the target receiver's margining capabilities:
  - Margining uses Driver Software (vs hardware autonomous)
  - Independent Left/Right Timing Margining support
  - Independent Up/Down Voltage Margining support
  - Error Sampler vs Main Sampler
  - Sample Multiple Receivers support

* ``num_timing_steps`` (read-only):
  Maximum timing margin steps supported by the receiver (0..63).

* ``num_voltage_steps`` (read-only):
  Maximum voltage margin steps supported by the receiver (0..127).

* ``margin_timing`` (read-write):
  Applies timing margin step offset (+/-). Writing ``0`` clears timing margin
  back to nominal.

* ``margin_voltage`` (read-write):
  Applies voltage margin step offset (+/-). Writing ``0`` clears voltage margin
  back to nominal.

Manual Margining Example
========================

1. Inspect device capabilities and status:

.. code-block:: sh

   cat /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/capabilities
   cat /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/port_status

2. Enable Lane Margining mode:

.. code-block:: sh

   echo 1 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/enable

3. Configure target receiver and inspect step limits on lane 0:

.. code-block:: sh

   echo 0 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/receiver
   cat /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/caps
   cat /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/num_timing_steps
   cat /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/num_voltage_steps

4. Apply timing and voltage margin steps:

.. code-block:: sh

   # Step timing margin +2 steps
   echo 2 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/margin_timing

   # Step voltage margin +1 step
   echo 1 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/margin_voltage

5. Reset margins back to nominal:

.. code-block:: sh

   echo 0 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/margin_timing
   echo 0 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/lane0/margin_voltage

6. Disable Lane Margining when complete:

.. code-block:: sh

   echo 0 > /sys/kernel/debug/pci/pcie_lmr_0000:01:00.0/enable

Automated Testing via Kselftest
===============================

The kernel includes an automated kselftest script under
``tools/testing/selftests/pcie_lmt/pcie_lmt.sh`` to probe, validate, and exercise
debugfs controls across all enumerated LMR devices.

Run directly as root:

.. code-block:: sh

   sudo ./tools/testing/selftests/pcie_lmt/pcie_lmt.sh

Or run via the kselftest test harness:

.. code-block:: sh

   make -C tools/testing/selftests TARGETS=pcie_lmt run_tests
