.. SPDX-License-Identifier: GPL-2.0

===========================================
PCI NVMe Endpoint Function (EPF) User Guide
===========================================

:Author: Damien Le Moal <dlemoal@kernel.org>

This document is a guide to help users use the pci-epf-nvme function driver to
create PCIe NVMe controllers. For a high-level description of the NVMe function
driver internals, see Documentation/PCI/endpoint/pci-nvme-function.rst.

Hardware and Kernel Requirements
================================

To use the NVMe PCI endpoint driver, at least one endpoint controller device
is required.

To find the list of endpoint controller devices in the system::

	# ls /sys/class/pci_epc/
        a40000000.pcie-ep

If PCI_ENDPOINT_CONFIGFS is enabled::

	# ls /sys/kernel/config/pci_ep/controllers
        a40000000.pcie-ep

Compiling the NVMe endpoint function driver depends on the target support of
the NVMe driver being enabled (CONFIG_NVME_TARGET). It is also recommended to
enable CONFIG_NVME_TARGET_LOOP to enable the use of loop targets (to use files
or block devices as storage for the NVMe target device). If the board used also
supports ethernet, CONFIG_NVME_TCP can be set to enable the use of remote TCP
NVMe targets.

To facilitate testing, enabling the null-blk driver (CONFIG_BLK_DEV_NULL_BLK)
is also recommended. With this, a simple setup using a null_blk block device
with an NVMe loop target can be used.


NVMe Endpoint Device
====================

Creating an NVMe endpoint device is a two step process. First, an NVMe target
device must be defined. Second, the NVMe endpoint device must be setup using
the defined NVMe target device.

Creating a NVMe Target Device
-----------------------------

Details about how to configure and NVMe target are outside the scope of this
document. The following only provides a simple example of a loop target setup
using a null_blk device for storage.

First, make sure that configfs is enabled::

	# mount -t configfs none /sys/kernel/config

Next, create a null_blk device (default settings give a 250 GB device without
memory backing). The block device created will be /dev/nullb0 by default::

        # modprobe null_blk
        # ls /dev/nullb0
        /dev/nullb0

The NVMe loop target driver must be loaded::

        # modprobe nvme_loop
        # lsmod | grep nvme
        nvme_loop              16384  0
        nvmet                 106496  1 nvme_loop
        nvme_fabrics           28672  1 nvme_loop
        nvme_core             131072  3 nvme_loop,nvmet,nvme_fabrics

Now, create the NVMe loop target, starting with the NVMe subsystem, specifying
a maximum of 4 queue pairs::

        # cd /sys/kernel/config/nvmet/subsystems
        # mkdir pci_epf_nvme.0.nqn
        # echo -n "Linux-pci-epf" > pci_epf_nvme.0.nqn/attr_model
        # echo 4 > pci_epf_nvme.0.nqn/attr_qid_max
        # echo 1 > pci_epf_nvme.0.nqn/attr_allow_any_host

Next, create the target namespace using the null_blk block device::

        # mkdir pci_epf_nvme.0.nqn/namespaces/1
        # echo -n "/dev/nullb0" > pci_epf_nvme.0.nqn/namespaces/1/device_path
        # echo 1 > "pci_epf_nvme.0.nqn/namespaces/1/enable"

Finally, create the target port and link it to the subsystem::

        # cd /sys/kernel/config/nvmet/ports
        # mkdir 1
        # echo -n "loop" > 1/addr_trtype
        # ln -s /sys/kernel/config/nvmet/subsystems/pci_epf_nvme.0.nqn
                1/subsystems/pci_epf_nvme.0.nqn


Creating a NVMe Endpoint Device
-------------------------------

With the NVMe target ready for use, the NVMe PCI endpoint device can now be
created and enabled. The first step is to load the NVMe function driver::

        # modprobe pci_epf_nvme
        # ls /sys/kernel/config/pci_ep/functions
        pci_epf_nvme

Next, create function 0::

        # cd /sys/kernel/config/pci_ep/functions/pci_epf_nvme
        # mkdir pci_epf_nvme.0
        # ls pci_epf_nvme.0/
        baseclass_code    msix_interrupts   secondary
        cache_line_size   nvme              subclass_code
        deviceid          primary           subsys_id
        interrupt_pin     progif_code       subsys_vendor_id
        msi_interrupts    revid             vendorid

Configure the function using any vendor ID and device ID::

        # cd /sys/kernel/config/pci_ep/functions/pci_epf_nvme/pci_epf_nvme.0
        # echo 0x15b7 > vendorid
        # echo 0x5fff > deviceid
        # echo 32 > msix_interrupts
        # echo -n "transport=loop,nqn=pci_epf_nvme.0.nqn,nr_io_queues=4" > \
                ctrl_opts

The ctrl_opts attribute must be set using equivalent arguments as used for a
norma NVMe target connection using "nvme connect" command. For the example
above, the equivalen target connection command is::

        # nvme connect --transport=loop --nqn=pci_epf_nvme.0.nqn --nr-io-queues=4

The endpoint function can then be bound to the endpoint controller and the
controller started::

        # cd /sys/kernel/config/pci_ep
        # ln -s functions/pci_epf_nvme/pci_epf_nvme.0 controllers/a40000000.pcie-ep/
        # echo 1 > controllers/a40000000.pcie-ep/start

Kernel messages will show information as the NVMe target device and endpoint
device are created and connected.

.. code-block:: text

        pci_epf_nvme: Registered nvme EPF driver
        nvmet: adding nsid 1 to subsystem pci_epf_nvme.0.nqn
        pci_epf_nvme pci_epf_nvme.0: DMA RX channel dma3chan2, maximum segment size 4294967295 B
        pci_epf_nvme pci_epf_nvme.0: DMA TX channel dma3chan0, maximum segment size 4294967295 B
        pci_epf_nvme pci_epf_nvme.0: DMA supported
        nvmet: creating nvm controller 1 for subsystem pci_epf_nvme.0.nqn for NQN nqn.2014-08.org.nvmexpress:uuid:0aa34ec6-11c0-4b02-ac9b-e07dff4b5c84.
        nvme nvme0: creating 4 I/O queues.
        nvme nvme0: new ctrl: "pci_epf_nvme.0.nqn"
        pci_epf_nvme pci_epf_nvme.0: NVMe fabrics controller created, 4 I/O queues
        pci_epf_nvme pci_epf_nvme.0: NVMe PCI controller supports MSI-X, 32 vectors
        pci_epf_nvme pci_epf_nvme.0: NVMe PCI controller: 4 I/O queues


PCI RootComplex Host
====================

Booting the host, the NVMe endpoint device will be discoverable as a PCI device::

        # lspci -n
        0000:01:00.0 0108: 15b7:5fff

An this device will be recognized as an NVMe device with a single namespace::

        # lsblk
        NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINTS
        nvme0n1     259:0    0   250G  0 disk

The NVMe endpoint block device can then be used as any other regular NVMe
device. The nvme command line utility can be used to get more detailed
information about the endpoint device::

        # nvme id-ctrl /dev/nvme0
        NVME Identify Controller:
        vid       : 0x15b7
        ssvid     : 0x15b7
        sn        : 0ec249554579a1d08fb5
        mn        : Linux-pci-epf
        fr        : 6.12.0-r
        rab       : 6
        ieee      : 000000
        cmic      : 0
        mdts      : 5
        ...
