.. SPDX-License-Identifier: GPL-2.0

Ethtool Pause testing
=====================

This document describes the test plan to be used for the ethtool selftest, aimed
at providing a validation framework for Ethernet drivers that implement flow-control.

To ease readability, the document uses the "ethtool" command to describe test cases,
actual tests may use the netlink API directly.

This document was written after surveying the common error patterns for pause-related
patches on the netdev mailing list. The common mistakes are usually found in either
MAC drivers that don't make use of phylink, or PHY drivers that interfere with
pause settings.

The main classes of problems are :

 - MAC drivers incorrectly configuring the Pause/Asym capabilities in the attached
   PHY device through calling phy_support_asym_pause() and phy_support_sym_pause().

   These are used to allow the PHY do know what Pause settings the MAC supports, in
   order to advertise it to the LP.

   They aren't to be used to configure the Pause advertisement, and the MAC driver
   needs to actually configure Pause on its side after negotiation finishes.

   Using phylink is the easiest fix for this kind of problem.

 - PHY drivers overriding the Pause/AsymPause bits in the supported/advertising
   fields. Pause is a MAC feature, but the PHY negotiates the settings with the
   partner. The PHY driver shouldn't have to tweak Pause settings, only advertise
   them and report the negotiation results.

 - MAC drivers confusing phydev->autoneg and pause autoneg. Pause autoneg is about
   whether or not we want to negotiate the Pause settings with the partner, or use
   locally enforced settings. phydev->autoneg, also referred to as link-wide autoneg
   is about whether or not we want to negotiate anything with the link partner,
   including speed, duplex, pause, etc. Both phydev->autoneg and pause autoneg can
   be configured independently.

 - MAC drivers mishandling the pause parameters in the ethtool get/set_pauseparam
   callbacks, usually by ignoring the pause autoneg settings.

The hardware capabilities regarding Pause support are reported through the
linkmodes bits 'ETHTOOL_LINK_MODE_Pause' and 'ETHTOOL_LINK_MODE_Asym_Pause', the
four different combinations are accepted and together describe the ability for
the hardware to send TX pause frames, and process the received ones :

 - **Pause** bit : the interface has the ability to Receive and process Pause frames
 - **Asym** bit (Or AsymDir) : The TX and RX pause setting can be different.

This translates to :

 - **Pause** & **Asym** : We can support **ALL** combinations of rx/tx

 - **Pause only** : only **rx == tx** configurations are valid

 - **Asym** only : **rx** must be **off**, **tx** can be **on or off**

Similarly, the link partner can advertise its capabilities through the same
bits. These parameters are exchanged through the negotiation process, but can
also be enforced locally by disabling **pause autoneg**, thus ignoring the
link partner's capabilities.

The local resolution of the pause configuration after receiving the link-partner
abilities is done according to the following table, from 802.3 Annex 28B.3 :

+-------------+--------------+--------------------------+
|Local device | Link partner | Pause settings resolution|
+------+------+-------+------+-----------+--------------+
|Pause | Asym | Pause | Asym | RX        | TX           |
+======+======+=======+======+===========+==============+
| 0    | 0    | Any   | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 0     | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 1     | 0    | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 1     | 1    | No        | Yes          |
+------+------+-------+------+-----------+--------------+
| 1    | 0    | 0     | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 1    | Any  | 1     | Any  | Yes       | Yes          |
+------+------+-------+------+-----------+--------------+
| 1    | 1    | 0     | 0    | No        | No           |
+------+------+-------+------+-----------+--------------+
| 1    | 1    | 0     | 1    | Yes       | No           |
+------+------+-------+------+-----------+--------------+

The currently configured and advertised settings can be queried with ::

  ethtool <iface>

  Settings for eth0:
        ...
	Supported pause frame use: Symmetric Receive-only
        ...
	Advertised pause frame use: Symmetric Receive-only
        ...
	Link partner advertised pause frame use: Symmetric Receive-only

While testing can be done directly with the Netlink API, as a reminder here's
what the ethtool output translates to :

+------------------------+-------+---------+
|     Ethtool output     | Pause | AsymDir |
+========================+=======+=========+
|          No            |   0   |    0    |
+------------------------+-------+---------+
|     Transmit-only      |   0   |    1    |
+------------------------+-------+---------+
|       Symmetric        |   1   |    0    |
+------------------------+-------+---------+
| Symmetric Receive-only |   1   |    1    |
+------------------------+-------+---------+

Mac drivers's current pause behaviour is reported through the
ethtool -a/--show-pause command, e.g.::

        ethtool -a <iface>

        Autonegotiate:	on
        RX:		off
        TX:		off
        RX negotiated: on
        TX negotiated: on

When **pause autoneg** is on, the currently used pause parameters are the
*negotiated* ones, that may differ from the user-specified rx and tx values.

When **pause autoneg** is off, the rx and tx values are the ones used by the
hardware.

A : Standalone driver testing
=============================

Requirements : The interface under test must be connected to a link-partner whose
interface is admin-up. We don't require the link-partner to be configured in any
other specific manner for these tests.

A.1 : Sanity Checks
~~~~~~~~~~~~~~~~~~~

Pause autoneg is set to off::

  ethtool -A <iface> autoneg off

The 'supported' fields retrieved using the ETHTOOL_MSG_LINKMODES_GET includes
a "Pause" bit and an "Asym" bit.

The ETHTOOL_MSG_PAUSE_GET command returns the currently configured pause modes
in the "tx" and "rx" attributes.

These parameters must validate against the following truth table :

   +--------------+-----------------+
   | linkmodes    | pauseparam      |
   +-------+------+--------+--------+
   | Pause | Asym | rx     | tx     |
   +=======+======+========+========+
   | 0     | 0    | 0      | 0      |
   +-------+------+--------+--------+
   | 0     | 1    | 0      | 0 or 1 |
   +-------+------+--------+--------+
   | 1     | 0    |     rx == tx    |
   +-------+------+--------+--------+
   | 1     | 1    | 0 or 1 | 0 or 1 |
   +-------+------+--------+--------+

Test scenario
-------------

Following the reported value from::

        ethtool <iface>
        Settings for <iface>:
                ...
	        Supported pause frame use: <value>

Iterate over all the 4 combinations of rx and tx pause parameters::

        ethtool -A <iface> autoneg off rx <rx_val> tx <tx_val>

The settings must be accepted or rejected, according to the above truth table.

Failing this test likely means the MAC driver is incorrectly setting the PHY's
Pause and AsymDir bits, or that the PHY driver overwrites the Pause and AsymDir
supported bits.

A.2 : Half-duplex operation
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Pause settings as exposed with the ethtool API only concern full-duplex modes.

Test scenario:
--------------

Set the interface under test in half-duplex mode with::

  ethtool -s <iface> duplex half

Expected behaviour::

 - ethtool <iface>

shows no Pause settings advertised.

Should this test fail, it likely means that either the MAC driver incorrectly
calls phydev_set_asym_pause(), or that the PHY driver overrides the settings.

B : Combined devices testing
============================

Requirements : The interface under test must be connected to a link-partner that
can be actively configured during the tests. It must at least support full-duplex
modes, and optionally (but ideally) symmetric and asymmetric flow-control, as
well as autonegotiation of the Pause parameters.

B.1 : Autoneg advertising
~~~~~~~~~~~~~~~~~~~~~~~~~

Goal: Validate that the *advertised* Pause and AsymDir bits match the configured
pausemarams.

The link-level autonegotiation must be enabled::

  ethtool <iface> autoneg on

Pause parameters are set with::

  ethtool -A <iface> rx <val> tx <val> autoneg on

Pause advertising is retrieved with::

  ethtool <iface>

Case 1
------

Pause parameters : rx **off** tx **off**

Expected advertisement : **None** (Pause = 0, AsymDir = 0)

Case 2
------

Pause parameters : rx **off** tx **on**

Expected advertisement : **Transmit-only** (Pause = 0, AsymDir = 1)

Case 3
------

Pause parameters : rx **on** tx **off**

Expected advertisement : **Symmetric receive-only** (Pause = 1, Asymdir = 1)

Case 4
------

Pause parameters : rx **on** tx **on**

Expected advertisement : **Symmetric** (Pause = 1, Asymdir = 0)

B.2 : Autoneg resolution
~~~~~~~~~~~~~~~~~~~~~~~~

Goal: Validate that the Pause and AsymDir negotiation translates to the right
TX and RX pause parameters.

The following table, from the 802.3 standard, exposes the autoneg resolution
result for the advertised pause parameters by each link partner.

+-------------+--------------+--------------------------+
|Local device | Link partner | Pause settings resolution|
+------+------+-------+------+-----------+--------------+
|Pause | Asym | Pause | Asym | RX        | TX	        |
+======+======+=======+======+===========+==============+
| 0    | 0    | Any   | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 0     | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 1     | 0    | No        | No           |
+------+------+-------+------+-----------+--------------+
| 0    | 1    | 1     | 1    | No        | Yes          |
+------+------+-------+------+-----------+--------------+
| 1    | 0    | 0     | Any  | No        | No           |
+------+------+-------+------+-----------+--------------+
| 1    | Any  | 1     | Any  | Yes       | Yes          |
+------+------+-------+------+-----------+--------------+
| 1    | 1    | 0     | 0    | No        | No           |
+------+------+-------+------+-----------+--------------+
| 1    | 1    | 0     | 1    | Yes       | No           |
+------+------+-------+------+-----------+--------------+

The mapping between the configured pause parameters and advertised modes follow
the following truth table :

+----+----+-------+---------+
| tx | rx | Pause | AsymDir |
+====+====+=======+=========+
| 0  | 0  | 0     | 0       |
+----+----+-------+---------+
| 0  | 1  | 1     | 1       |
+----+----+-------+---------+
| 1  | 0  | 0     | 1       |
+----+----+-------+---------+
| 1  | 1  | 1     | 0       |
+----+----+-------+---------+

We can boil that down to the following cases to test, keeping the number small
to avoid dealing with the whole combinatory::

        ethtool -A <iface> autoneg on rx <rx_param> tx <tx_param>

Failing tests here would likely indicate that the MAC driver doesn't correctly
accounts for the negotiated Pause parameters in its .adjust_link() callback. A
MAC driver that uses phylink should pass these tests.

Case 1
------

Local device : rx **off**, tx **off**
Remote device : rx **on** tx **on**

Expected result on local device after autonegotiation completes :
        rx negotiated **off**
        tx negotiated **off**

Case 2
------

Local device : rx **off** tx **on**
Remote device : rx **off** tx **on**

Expected result on local device after autonegotiation completes :
        rx negotiated **off**
        tx negotiated **off**

Case 3
------

Local device : rx **off** tx **on**
Remote device : rx **on** tx **off**

Expected result on local device after autonegotiation completes :
        rx negotiated **off**
        tx negotiated **on**

Case 4
------

Local device : rx **off** tx **on**
Remote device : rx **on** tx **on**

Expected result on local device after autonegotiation completes :
        rx negotiated **off**
        tx negotiated **off**

This case looks surprising but boils down to the fact that we advertise Asym only,
whereas the LP is advertising Pause (and not Pause + Asym).

As per 802.3 this resolves to all "Off".

Case 5
------

Local device : rx **on** tx **off**
Remote device : rx **off** tx **on**

Expected result on local device after autonegotiation completes :
        rx negotiated **on**
        tx negotiated **off**

Case 6
------

Local device : rx **on** tx **on**
Remote device : rx **on** tx **off**

Expected result on local device after autonegotiation completes :
        rx negotiated **on**
        tx negotiated **on**

This is also not an obvious result. We advertise Pause, the Link partner advertises
Pause + Asym, so it resolves to all "On".

Case 7
------

Local device : rx **on** tx **on**
Remote device : rx **off** tx **off**

Expected result on local device after autonegotiation completes :
        rx negotiated **off**
        tx negotiated **off**

Case 8
------

Local device : rx **on** tx **on**
Remote device : rx **off** tx **on**

Expected result on local device after autonegotiation completes :
        rx negotiated **on**
        tx negotiated **off**

B.3 : Pause Autoneg
~~~~~~~~~~~~~~~~~~~

Goal: Validate that the Pause autonegotiation flag correctly toggles the
advertised Pause and AsymDir link parameters.

Test scenario:
--------------

 - Enable pause autoneg and at least rx or tx pause::

        ethtool -A <iface> rx on tx on autoneg on

 - Check the Advertised pause frame use::

        ethtool <iface>

        ...
        Advertised pause frame use: Symmetric Receive-only

 - Disable pause autoneg::

        ethtool -A <iface> autoneg off

 - Check the Advertised pause frame use, which must be 'No'::

        ethtool <iface>

        ...
        Advertised pause frame use: No

B.4 : Pause autoneg transition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Goal: Validate the advertising and pause parameters upon toggling pause
      autonegotiation. When pause autoneg is disabled, the local pause settings
      are dictated by the user configuration, without relying on negotiated
      parameters.

This validation focuses on cases where the pause autoneg result differs from the
user-specified rx and tx pauseparams, e.g. :

Local device : rx **on** tx **off**
Remote device : rx **on** tx **on**

This will negotiate into rx and tx being **on**, contrary to the user intent.

Test scenario:
--------------

 - Enable link-wide autonegotiation::

        ethtool -s <iface> autoneg on

 - Enable Pause autonegotiation, setting RX on and TX off::

        ethtool -A <iface> rx on tx off autoneg on

 - Expected result::

        ethtool -a <iface>

        Pause parameters for <iface>:
        Autonegotiate:	on
        RX:		on
        TX:		off
        RX negotiated: on
        TX negotiated: on

Note that the currently applied configuration here is expressed by the "negotiated"
parameters, so the link is currently using RX and TX Pauses.

 - Disable pause autonegotiation::

        ethtool -A <iface> autoneg off

 - Expected result::

        ethtool -a <iface>

        Pause parameters for <iface>:
        Autonegotiate:	off
        RX:		on
        TX:		off

Note that now, the currently applied configuration really is RX on TX off. It is
expected that upon running the above command, the link flips down and up again
as flow-control parameters are updated.

 - Re-enable pause autonegotiation::

        ethtool -A <iface> autoneg on

 - Expected result::

        ethtool -a <iface>

        Pause parameters for <iface>:
        Autonegotiate:	on
        RX:		on
        TX:		off
        RX negotiated: on
        TX negotiated: on

The user-defined pause parameters needs to be again converted to advertised Pause
and Asym parameters, negotiated and the final outcome must be the same as the original
conditions.

B.5 : Pause autoneg only to Pause and Link autoneg transition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Goal: Validate that drivers store the user intent in the pause rx/tx attributes
when disabling link autoneg, but *keeping the pause autoneg enabled*.

When pause autoneg is enabled, the rx and tx pause parameters are the user intent,
but can be different to the actual pause parameters actively in use.

Test scenario:
--------------

 - Configure rx and tx pause parameters to **on** and pause autoneg to **on**::

        ethtool -A <iface> autoneg on rx on tx on

 - Disable link autonegotiation::

        ethtool -s <iface> autoneg off

 - Configure rx and tx pause to **rx off**, **tx on** and autoneg to **on**::

        ethtool -A <iface> autoneg on rx off tx on

 - No change in the pause parameters should occur
 - Re-enable link negotiation::

        ethtool -s <iface> autoneg on

 - Link must now re-negotiate pause to **rx off** and **tx on**::

        ethtool -a <iface>

        Pause parameters for <iface>:
        Autonegotiate:	on
        RX:		off
        TX:		on
        RX negotiated: off
        TX negotiated: on

