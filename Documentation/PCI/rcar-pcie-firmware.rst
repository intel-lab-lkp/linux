.. SPDX-License-Identifier: GPL-2.0

=================================================
Firmware of PCIe controller for Renesas R-Car V4H
=================================================

Renesas R-Car V4H (r8a779g0) has PCIe controller, and it requires specific
firmware downloading. The firmware file "104_PCIe_fw_addr_data_ver1.05.txt"
is available in the datasheet as a text file. But, Renesas is not able to
distribute the firmware freely. So, we require converting the text file to
a binary before the driver runs by using the following script:

.. code-block:: sh

   $ awk '/^\s*0x[0-9A-Fa-f]{4}\s+0x[0-9A-Fa-f]{4}/ \
   { print substr($2,5,2) substr($2,3,2) }' \
   104_PCIe_fw_addr_data_ver1.05.txt | xxd -p -r > \
   rcar_gen4_pcie.bin

   $ sha1sum rcar_gen4_pcie.bin

   # Example output of the sha1sum:
   1d0bd4b189b4eb009f5d564b1f93a79112994945  rcar_gen4_pcie.bin
