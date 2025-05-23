.. SPDX-License-Identifier: GPL-2.0

=============
PCS Subsystem
=============

The PCS (Physical Coding Sublayer) subsystem handles the registration and lookup
of PCS devices. These devices contain the upper sublayers of the Ethernet
physical layer, generally handling framing, scrambling, and encoding tasks. PCS
devices may also include PMA (Physical Medium Attachment) components. PCS
devices transfer data between the Link-layer MAC device, and the rest of the
physical layer, typically via a serdes. The output of the serdes may be
connected more-or-less directly to the medium when using fiber-optic or
backplane connections (1000BASE-SX, 1000BASE-KX, etc). It may also communicate
with a separate PHY (such as over SGMII) which handles the connection to the
medium (such as 1000BASE-T).

Looking up PCS Devices
----------------------

There are generally two ways to look up a PCS device. If the PCS device is
internal to a larger device (such as a MAC or switch), and it does not share an
implementation with an existing PCS, then it does not need to be registered with
the PCS subsystem. Instead, you can populate a :c:type:`phylink_pcs`
in your probe function. Otherwise, you must look up the PCS.

If your device has a :c:type:`fwnode_handle`, you can add a PCS using the
``pcs-handle`` property::

    ethernet-controller {
        // ...
        pcs-handle = <&pcs>;
        pcs-handle-names = "internal";
    };

Then, during your probe function, you can get the PCS using :c:func:`pcs_get`::

    mac->pcs = pcs_get(dev, "internal");
    if (IS_ERR(mac->pcs)) {
        err = PTR_ERR(mac->pcs);
        return dev_err_probe(dev, "Could not get PCS\n");
    }

If your device doesn't have a :c:type:`fwnode_handle`, you can get the PCS
based on the providing device using :c:func:`pcs_get_by_dev`. Typically, you
will create the device and bind your PCS driver to it before calling this
function. This allows reuse of an existing PCS driver.

Once you are done using the PCS, you must call :c:func:`pcs_put`.

Using PCS Devices
-----------------

To select the PCS from a MAC driver, implement the ``mac_select_pcs`` callback
of :c:type:`phylink_mac_ops`. In this example, the PCS is selected for SGMII
and 1000BASE-X, and deselected for other interfaces::

    static struct phylink_pcs *mac_select_pcs(struct phylink_config *config,
                                              phy_interface_t iface)
    {
        struct mac *mac = config_to_mac(config);

        switch (iface) {
        case PHY_INTERFACE_MODE_SGMII:
        case PHY_INTERFACE_MODE_1000BASEX:
            return mac->pcs;
        default:
            return NULL;
        }
    }

To do the same from a DSA driver, implement the ``phylink_mac_select_pcs``
callback of :c:type:`dsa_switch_ops`.

Writing PCS Drivers
-------------------

To write a PCS driver, first implement :c:type:`phylink_pcs_ops`. Then,
register your PCS in your probe function using :c:func:`pcs_register`. If you
need to provide multiple PCSs for the same device, then you can pass specific
firmware nodes using :c:macro:`pcs_register_full`.

You must call :c:func:`pcs_unregister` from your remove function. You can avoid
this step by registering with :c:func:`devm_pcs_unregister`.

API Reference
-------------

.. kernel-doc:: include/linux/phylink.h
   :identifiers: phylink_pcs phylink_pcs_ops pcs_validate pcs_inband_caps
      pcs_enable pcs_disable pcs_pre_config pcs_post_config pcs_get_state
      pcs_config pcs_an_restart pcs_link_up pcs_disable_eee pcs_enable_eee
      pcs_pre_init

.. kernel-doc:: include/linux/pcs.h
   :internal:

.. kernel-doc:: drivers/net/pcs/core.c
   :export:

.. kernel-doc:: drivers/net/pcs/core.c
   :internal:
