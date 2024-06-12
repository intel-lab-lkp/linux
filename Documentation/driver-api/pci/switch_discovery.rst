=================================
Linux PCI Switch discovery module
=================================

Modern PCI switches support inter switch Peer-to-Peer(P2P) data transfer
without using host resources. For example, Broadcom(PLX) PCIe Switches have a
capability where a single physical switch can be divided up into multiple
virtual switches at SOD. PCIe switch discovery module detects the virtual links
between the switches that belong to the same physical switch.
This allows user space applications to discover these virtual links that belong
to the same physical switch and configure optimized data paths.

Userspace Interface
===================

The module exposes sysfs entries for user space applications like MPI, NCCL,
UCC, RCCL, HCCL, etc to discover the virtual switch links.

Consider the below topology

                             Host root bridge
                ---------------------------------------
                |                                     |
  NIC1 --- PCI Switch1 --- Inter-switch link --- PCI Switch2 --- NIC2
(af:00.0)   (ad:00.0)                             (8b:00.0)   (8d:00.0)
                |                                     |
               GPU1                                  GPU2
            (b0:00.0)                             (8e:00.0)
                               SERVER 1

The simple topology above shows SERVER1, has Switch1 and Switch2 which are
virtual switches that belong to the same physical switch that support
Inter switch P2P.
Switch1 and Switch2 have a GPU and NIC each connected.
The module will detect the virtual P2P link existing between the two switches
and create the sysfs entries as below.

/sys/kernel/pci_switch_link/virtual_switch_links
├── 0000:8b:00.0
│   └── 0000:ad:00.0 -> ../0000:ad:00.0
└── 0000:ad:00.0
    └── 0000:8b:00.0 -> ../0000:8b:00.0

The HPC/AI libraries that analyze the topology can decide the optimal data
path like: NIC1->GPU1->GPU2->NIC1 which would have otherwise take a
non-optimal path like NIC1->GPU1->GPU2->GPU1->NIC1.

Enable P2P DMA to discover virtual links
----------------------------------------
The module also enhances :c:func:`pci_p2pdma_distance()` to determine a virtual
link between the upstream PCI-to-PCI bridges of the devices and detect optimal
path for applications using P2P DMA API.
