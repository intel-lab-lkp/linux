.. SPDX-License-Identifier: GPL-2.0

=====================
Ethernet Flow Control
=====================

This document is a practical guide to Ethernet Flow Control in Linux, covering
what it is, how it works, and how to configure it.

What is Flow Control?
=====================

Flow control is a mechanism to prevent a fast sender from overwhelming a
slow receiver with data, which would cause buffer overruns and dropped packets.
The receiver can signal the sender to temporarily stop transmitting, giving it
time to process its backlog.

How It Works: The Two Mechanisms
================================

The method used for flow control depends on the link's duplex mode.

1. Full-Duplex: PAUSE Frames (IEEE 802.3 Annex 31B)
---------------------------------------------------
On full-duplex links, devices can send and receive at the same time. Flow
control is achieved by sending a special **PAUSE frame**.

* **What it is**: A standard Ethernet frame with a globally reserved
    destination MAC address (``01-80-C2-00-00-01``). This address is in a range
    that standard IEEE 802.1D-compliant bridges do not forward. However, some
    unmanaged or misconfigured bridges have been reported to forward these
    frames, which can disrupt flow control across a network.

* **How it works**: The frame contains a `pause_time` value, telling the
    sender how long to wait before sending more data frames.

* **Who uses it**: Any full-duplex link, from 10Mbps to multi-gigabit speeds.

2. Half-Duplex: Collision-Based Flow Control
--------------------------------------------
On half-duplex links, a device cannot send and receive simultaneously, so PAUSE
frames are not used. Flow control is achieved by leveraging the CSMA/CD
(Carrier Sense Multiple Access with Collision Detection) protocol itself.

* **How it works**: To inhibit incoming data, a receiving device can force a
    collision on the line. When the sending station detects this collision, it
    terminates its transmission, sends a "jam" signal, and then executes the
    "Collision backoff and retransmission" procedure as defined in IEEE 802.3,
    Section 4.2.3.2.5. This algorithm makes the sender wait for a random
    period before attempting to retransmit. By repeatedly forcing collisions,
    the receiver can effectively throttle the sender's transmission rate.

.. note::
    While this mechanism is part of the IEEE standard, there is currently no
    generic kernel API to configure or control it. Drivers should not enable
    this feature until a standardized interface is available.

Configuring Flow Control with `ethtool`
=======================================

The standard tool for managing flow control is `ethtool`.

Viewing the Current Settings
----------------------------
Use `ethtool -a <interface>` to see the current configuration.

.. code-block:: text

  $ ethtool -a eth0
  Pause parameters for eth0:
  Autonegotiate:  on
  RX:             on
  TX:             on

* **Autonegotiate**: Shows if flow control settings are being negotiated with
    the link partner.

* **RX**: Shows if we will *obey* PAUSE frames (pause our sending).

* **TX**: Shows if we will *send* PAUSE frames (ask the peer to pause).

If autonegotiation is on, `ethtool` will also show the active, negotiated result.
This result is calculated by `ethtool` itself based on the advertisement masks
from both link partners. It represents the expected outcome according to IEEE
802.3 rules, but the final decision on what is programmed into the MAC hardware
is made by the kernel driver.

.. code-block:: text

  RX negotiated: on
  TX negotiated: on

Changing the Settings
---------------------
Use `ethtool -A <interface>` to change the settings.

.. code-block:: bash

  # Enable RX and TX pause, with autonegotiation
  ethtool -A eth0 autoneg on rx on tx on

  # Force RX pause on, TX pause off, without autonegotiation
  ethtool -A eth0 autoneg off rx on tx off

**Key Configuration Concepts**:

* **Autonegotiation Mode**: The driver programs the PHY to *advertise* the
    `rx` and `tx` capabilities. The final active state is determined by what
    both sides of the link agree on.

    .. note::
        The negotiated outcome may not match the requested configuration. For
        example, if you request asymmetric flow control (e.g., `rx on` `tx off`)
        but the link partner only supports symmetric pause, the result could be
        symmetric pause (`rx on` `tx on`). Conversely, if you request symmetric
        pause but the partner only supports asymmetric, flow control may be
        disabled entirely. Refer to the resolution table in the PHY section
        for the exact negotiation logic.

* **Forced Mode**: This mode is necessary when autonegotiation is not used or
    not possible. This includes links where one or both partners have
    autonegotiation disabled, or in setups without a PHY (e.g., direct
    MAC-to-MAC connections). The driver bypasses PHY advertisement and
    directly forces the MAC into the specified `rx`/`tx` state. The
    configuration on both sides of the link must be complementary. For
    example, if one side is set to `tx on` `rx off`, the link partner must be
    set to `tx off` `rx on` for flow control to function correctly.

Component Roles in Flow Control
===============================

The configuration of flow control involves several components, each with a
distinct role.

The MAC (Media Access Controller)
---------------------------------
The MAC is the hardware component that actually sends and receives PAUSE
frames. Its capabilities define the upper limit of what the driver can support.
Known MAC implementation variations include:

* **Flexible Full Flow Control**: Has separate enable/disable controls for the
    TX and RX paths. This is the most common and desirable implementation.

* **Symmetric-Only Flow Control**: Provides a single control bit to
    enable/disable flow control in both directions simultaneously. It cannot
    support asymmetric configurations.

* **Receive-Only Flow Control**: Can only process incoming PAUSE frames (to
    pause its own transmitter). It cannot generate PAUSE frames. If the system
    needs to send a PAUSE frame, it would have to be generated by software.

* **Transmit-Only Flow Control**: Can only generate PAUSE frames when its own
    buffers are full. It ignores incoming PAUSE frames.

* **Weak Overwrite Variant**: Often found in combined MAC+PHY chips. A single
    control bit determines whether the MAC uses the autonegotiated result or
    a forced, pre-defined setting.

Additionally, some MACs provide a way to configure the `pause_time` value
(quanta) sent in PAUSE frames. This value's duration depends on the link
speed. As there is currently no generic kernel interface to configure this,
drivers often set it to a default or maximum value, which may not be optimal
for all use cases.

Many MACs also implement automatic PAUSE frame transmission based on the fill
level of their internal RX FIFO. This is typically configured with two
thresholds:

* **FLOW_ON (High Water Mark)**: When the RX FIFO usage reaches this
    threshold, the MAC automatically transmits a PAUSE frame to stop the sender.

* **FLOW_OFF (Low Water Mark)**: When the RX FIFO usage drops below this
    threshold, the MAC transmits a PAUSE frame with a quanta of zero to tell
    the sender it can resume transmission.

The optimal values for these thresholds often depend on the bandwidth of the
bus between the MAC and the system's CPU or RAM. Like the pause quanta, there
is currently no generic kernel interface for tuning these thresholds.

The PHY (Physical Layer Transceiver)
------------------------------------
The PHY's role is to manage the autonegotiation process. It does not generate
or interpret PAUSE frames itself; it only communicates capabilities between
the two link partners.

* **Advertisement**: The PHY advertises the MAC's flow control capabilities.
    This is done using two bits in the advertisement register: "Symmetric
    Pause" (Pause) and "Asymmetric Pause" (Asym). These bits should be
    interpreted as a combined value, not as independent flags. The kernel
    converts the user's `rx` and `tx` settings into this two-bit value as
    follows:

    .. code-block:: text

        tx rx  | Pause Asym
        -------+-----------
        0  0  |  0     0
        0  1  |  1     1
        1  0  |  0     1
        1  1  |  1     0

* **Resolution**: After negotiation, the PHY reports the link partner's
    advertised Pause and Asym bits. The final flow control mode is determined
    by the combination of the local and partner advertisements, according to
    the IEEE 802.3 standard:

    .. code-block:: text

        Local Device | Link Partner |
        Pause Asym   | Pause Asym   | Result
        -------------+--------------+-----------
          0     X    |   0     X    | Disabled
          0     1    |   1     0    | Disabled
          0     1    |   1     1    | TX only
          1     0    |   0     X    | Disabled
          1     X    |   1     X    | TX + RX
          1     1    |   0     1    | RX only

    It is important to note that the advertised bits reflect the *current
    configuration* of the MAC, which may not represent its full hardware
    capabilities.

* **Limitations**: A PHY may have its own limitations and may not be able to
    advertise the full set of capabilities that the MAC supports.

User Space Interface
--------------------
The primary user space tool for flow control configuration is `ethtool`. It
communicates with the kernel via netlink messages, specifically
`ETHTOOL_MSG_PAUSE_GET` and `ETHTOOL_MSG_PAUSE_SET`.

These messages use a simple set of attributes that map to the members of the
`struct ethtool_pauseparam`:

* `ETHTOOL_A_PAUSE_AUTONEG` -> `autoneg`
* `ETHTOOL_A_PAUSE_RX` -> `rx_pause`
* `ETHTOOL_A_PAUSE_TX` -> `tx_pause`

The driver's implementation of the `.get_pauseparam` and `.set_pauseparam`
ethtool operations must correctly interpret these fields.

* **On `get_pauseparam`**, the driver must report the user's configured flow
    control policy.

    * The `autoneg` flag indicates the driver's behavior: if `on`, the driver
      will respect the negotiated outcome; if `off`, the driver will use a
      forced configuration.

    * The `rx_pause` and `tx_pause` flags reflect the currently preferred
      configuration state, which depends on multiple factors.

* **On `set_pauseparam`**, the driver must interpret the user's request:

    * The `autoneg` flag acts as a mode selector. If `on`, the driver
      configures the PHY's advertisement based on `rx_pause` and `tx_pause`.

    * If `off`, the driver forces the MAC into the state defined by
      `rx_pause` and `tx_pause`.

Monitoring Flow Control
=======================

The standard way to check if flow control is actively being used is to view the
pause-related statistics with the command:
``ethtool --include-statistics -a <interface>``

.. code-block:: text

  $ ethtool --include-statistics -a eth0
  Pause parameters for eth0:
  Autonegotiate:  on
  RX:             on
  TX:             on
  RX negotiated: on
  TX negotiated: on
  Statistics:
    tx_pause_frames: 0
    rx_pause_frames: 0

The `tx_pause_frames` and `rx_pause_frames` counters show the number of PAUSE
frames sent and received. Non-zero or increasing values indicate that flow
control is active.

If a driver does not support this standard statistics interface, it may expose
its own legacy counters via `ethtool -S <interface>`. The names of these
counters are driver-implementation specific.

Test Plan
=========

This section outlines test cases for verifying flow control configuration. The
`ethtool -s` command is used to set the base link state (autoneg on/off), and
`ethtool -A` is used to configure the pause parameters within that state.

Case 1: Base Link is Autonegotiating
------------------------------------
*Prerequisite*: `ethtool -s eth0 autoneg on`

**Test 1.1: Standard Pause Negotiation**

* **Command**: `ethtool -A eth0 autoneg on rx on tx on`

* **Expected Behavior**: The driver configures the PHY to advertise symmetric
  pause. The MAC will be programmed with the *result* of the negotiation.

* **Verification**: `ethtool -a eth0` shows `Autonegotiate: on`, `RX: on`,
  `TX: on`. The `RX/TX negotiated` lines show the actual result.

**Test 1.2: Forced Pause Overriding Autonegotiation**

* **Command**: `ethtool -A eth0 autoneg off rx on tx off`

* **Expected Behavior**: The driver stops advertising pause capabilities on
  the PHY to avoid misleading the link partner. It then forces the MAC to
  enable RX pause and disable TX pause, ignoring any potential negotiation
  result.

* **Verification**: `ethtool -a eth0` shows `Autonegotiate: off`, `RX: on`,
  `TX: off`. No "negotiated" lines are shown.

Case 2: Base Link is Forced (Not Autonegotiating)
-------------------------------------------------
*Prerequisite*: `ethtool -s eth0 autoneg off speed 1000 duplex full`

**Test 2.1: Configure Pause Policy for Future Autonegotiation**

* **Command**: `ethtool -A eth0 autoneg on rx on tx on`

* **Expected Behavior**: The command succeeds. The driver stores the user's
  intent to use autonegotiation for flow control (`autoneg=on`). Since the
  base link is currently in a forced mode, it also immediately applies the
  requested `rx` and `tx` settings to the MAC's forced configuration. The
  `autoneg=on` policy will only become fully active (i.e., negotiated) if the
  link is later switched to autonegotiation.

* **Verification**: `ethtool -a eth0` now shows `Autonegotiate: on`, `RX: on`,
  `TX: on`, reflecting the newly set policy. If the link is later reconfigured
  with `ethtool -s eth0 autoneg on`, this pause policy will be applied.

**Test 2.2: Forced Pause Configuration**

* **Command**: `ethtool -A eth0 autoneg off rx on tx on`

* **Expected Behavior**: The driver stores the configuration and programs the
  MAC registers directly to enable both RX and TX pause.

* **Verification**: `ethtool -a eth0` shows `Autonegotiate: off`, `RX: on`,
  `TX: on`.

Case 3: Persistence of Settings
-------------------------------
This test verifies that the driver correctly remembers and applies the user's
intent across multiple commands that change the pause mode.

*Prerequisite*: `ethtool -s eth0 autoneg on`

**Test 3.1: Command Sequence**
1.  **`ethtool -A eth0 autoneg on rx on tx off`**

    * **State**: The driver configures the PHY to advertise asymmetric pause
      (RX on, TX off). It stores `pause_autoneg=true`, `pause_rx=true`,
      `pause_tx=false`. The MAC uses the negotiated result.

2.  **`ethtool -A eth0 autoneg off rx on tx on`**

    * **State**: The driver now enters "forced overwrite" mode. It updates
      its stored state to `pause_autoneg=false`, `pause_rx=true`,
      `pause_tx=true`. It then forces the MAC to enable symmetric pause,
      ignoring the PHY's negotiation result.

3.  **`ethtool -A eth0 autoneg on`**

    * **State**: The driver reverts to following the negotiation. It sets its
      stored state to `pause_autoneg=true`. Since `rx` and `tx` were not
      specified, it uses its last known values (`rx=on`, `tx=on`) to update
      the PHY advertisement. The MAC is now programmed with the result of
      this new negotiation.

* **Expected Behavior**: The driver correctly transitions between advertising
  pause, forcing pause, and returning to advertising, always maintaining the
  user's last specified configuration for `rx` and `tx` when toggling the
  `autoneg` mode.

