.. SPDX-License-Identifier: GPL-2.0

=================================================
Firmware of PCIe controller for Renesas R-Car V4H
=================================================

Renesas R-Car V4H (r8a779g0) has a PCIe controller, requiring a specific
firmware download during startup.

Firmware file "rcar_gen4_pcie.bin" is distributed in the linux-firmware repository:
https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/
since linux-firmware.git commit
56bb432a65bc ("rcar_gen4_pcie: add firmware for Renesas R-Car Gen4 PCIe controller")

Download the file and verify the file checksum as follows:

.. code-block:: sh

	$ sha1sum rcar_gen4_pcie.bin
	1d0bd4b189b4eb009f5d564b1f93a79112994945  rcar_gen4_pcie.bin

The resulting binary file called "rcar_gen4_pcie.bin" should be placed in the
"/lib/firmware" directory before the driver runs.
