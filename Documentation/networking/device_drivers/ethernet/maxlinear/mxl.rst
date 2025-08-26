.. SPDX-License-Identifier: GPL-2.0

===============================================
MaxLinear Multi-MAC Network Processor (NP)
===============================================

Copyright(c) 2025 MaxLinear, Inc.

Overview
========

This document describes the Linux driver for the MaxLinear Network Processor
(NP), a high-performance controller supporting multiple MACs and
advanced packet processing capabilities.

The MaxLinear Network processor integrates programmable hardware accelerators
for tasks such as Layer 2, 3, 4 forwarding, flow steering, and traffic shaping.
It is designed to operate in high-throughput applications, including data
center switching, virtualized environments, and telco infrastructure.

Key Features
============

- Support for up to 4 independent 10 Gbit/s MAC interfaces
- Full-duplex 10G operation
- Multiqueue support for parallel RX/TX paths (per MAC)

Supported Devices
=================

The driver supports the following MaxLinear NPU family devices:
- MaxLinear LGM

Each device supports multiple MACs and high-performance data pipelines managed
through internal firmware and programmable engines.

Kernel Configuration
====================

The driver is located in the menu structure at:

  -> Device Drivers
    -> Network device support
      -> Ethernet driver support
        -> MaxLinear NPU Ethernet driver

Or set in your kernel config:
  CONFIG_NET_VENDOR_MAXLINEAR=y
  CONFIG_MAXLINEAR_ETH=y

Maintainers
===========

See the MAINTAINERS file:

    MAXLINEAR ETHERNET DRIVER
    M: Jack Ping Chng <jchng@maxlinear.com>
    L: netdev@vger.kernel.org
    S: Supported
    F: drivers/net/ethernet/maxlinear/

