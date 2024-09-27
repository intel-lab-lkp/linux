Diagnostic Concept for Investigating Twisted Pair Ethernet Variants at OSI Layer 1
==================================================================================

Introduction
------------

This documentation is designed for two primary audiences:

1. **Users and System Administrators**: For those dealing with real-world
   Ethernet issues, this guide provides a practical, step-by-step
   troubleshooting flow to help identify and resolve common problems in Twisted
   Pair Ethernet at OSI Layer 1. If you're facing unstable links, speed drops,
   or mysterious network issues, jump right into the step-by-step guide and
   follow it through to find your solution.

2. **Kernel Developers**: For developers working with network drivers and PHY
   support, this documentation outlines the diagnostic process and highlights
   areas where the Linux kernel’s diagnostic interfaces could be extended or
   improved. By understanding the diagnostic flow, developers can better
   prioritize future enhancements.

Structure
------------

The document is divided into two parts:

- **Part 1: Step-by-Step Troubleshooting Guide**: This is for those who need
  immediate help with their networking issues. We get it - no one wants to sift
  through endless technical descriptions when the link is down. This section
  provides concise, actionable steps to get your network up and running as
  quickly as possible.

- **Part 2: In-Depth Descriptions and Technical Insights**: For those who want
  to dig deeper, this section offers detailed descriptions of hardware
  specifications, typical problems, and diagnostic techniques. It provides
  context for the issues addressed in the troubleshooting guide and serves as a
  reference for developers and network engineers who need to understand the
  reasoning behind each step.

By the end of this documentation, whether you’re a frustrated admin or a
curious developer, we hope you’ll have the tools and understanding necessary to
diagnose and resolve twisted pair Ethernet issues at the physical layer. And if
you discover new or unresolved problems, feel free to extend this documentation
with your findings!

Step-by-Step Diagnostic Guide from Linux (General Ethernet)
-----------------------------------------------------------

This diagnostic guide covers common Ethernet troubleshooting scenarios,
focusing on **link stability and detection** across different Ethernet
environments, including **Single-Pair Ethernet (SPE)** and **Multi-Pair
Ethernet (MPE)**, as well as power delivery technologies like **PoDL** (Power
over Data Line) and **PoE** (Clause 33 PSE).

Diagnostic Scenarios
~~~~~~~~~~~~~~~~~~~~

- **Link is up and stable, but no data transfer**: If the link is stable but
  there are issues with data transmission, refer to the **OSI Layer 2
  Troubleshooting Guide**.

- **Link is unstable**: Link resets, speed drops, or other fluctuations
  indicate potential issues at the hardware or physical layer.

- **No link detected**: The interface is up, but no link is established.

Verify Interface Status
~~~~~~~~~~~~~~~~~~~~~~~

Start by checking the status of the Ethernet interface to see if it is
administratively up.

- **Command:** `ip link show dev <interface>`
- **Expected Output:**

  .. code-block:: bash

    4: eth0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 ...
       link/ether 88:14:2b:00:96:f2 brd ff:ff:ff:ff:ff:ff

- **If Output:**

  - Interface is down: Bring the interface up using `ip link set dev eth0 up`.
  - Interface is up but shows `NO-CARRIER` Proceed to the **Inspect Link Status
    and PHY Configuration** step.

Inspect Link Status and PHY Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use `ethtool` to check the link status, PHY configuration, and supported link
modes.

- **Command:** `ethtool <interface>`
- **Expected Output:**

  .. code-block:: bash

    Settings for eth0:
       Supported link modes:  1000baseT/Full, 100baseT/Full
       Advertised link modes: 1000baseT/Full
       Speed: Unknown!
       Duplex: Unknown! (255)
       Auto-negotiation: on
       Link detected: no

- Information to Gather:

  - Supported link modes. This gives insights into the type of technology used.
    If you see modes like **1000baseT1**, **100baseT1**, or **10baseT1S**, it
    likely indicates that this is a **Single-Pair Ethernet (SPE)** link,
    which typically does not support autonegotiation.
    **Note:** In such cases, you can **skip the comparison of advertised
    link modes**, as neither link partner will advertise any mode.

- **Scenarios:**

  - **Link is up and stable, but transfer issues exist**: This may indicate a
    duplex mismatch. Proceed to the **Verify Link Partner PHY Configuration**
    step.
  - **Unstable Link**: If the link is fluctuating, showing intermittent resets
    or speed drops, proceed to the **Cable and Power Diagnostics** step.
  - **No Link**: If `Link detected: no`, continue with further diagnostics.

Check Power Delivery (PoDL or PoE)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If it is known that **PoDL** or **PoE** is **not implemented** on the system,
or the **PSE** (Power Sourcing Equipment) is managed by proprietary user-space
software or external tools, you can skip this step. In such cases, verify power
delivery through alternative methods, such as checking hardware indicators
(LEDs), using multimeters, or consulting vendor-specific software for
monitoring power status.

If **PoDL** or **PoE** is implemented and managed directly by Linux, follow
these steps to ensure power is being delivered correctly:

- **For PoDL (Single-Pair Ethernet)**:

  - **Command:** `ethtool --show-pse <interface>`
  - **Expected Output (PoDL)**:

    .. code-block:: bash

      PSE attributes for eth1:
      PoDL PSE Admin State: enabled
      PoDL PSE Power Detection Status: delivering power

- **For PoE (Clause 33 PSE)**:

  - **Command:** `ethtool --show-pse <interface>`
  - **Expected Output (PoE)**:

    .. code-block:: bash

      PSE attributes for eth1:
      Clause 33 PSE Admin State: enabled
      Clause 33 PSE Power Detection Status: delivering power
      Clause 33 PSE Available Power Limit: 18000

- **Adjust Power Limit (if needed)**:

  - Sometimes, the available power limit may not be sufficient for the link
    partner. You can increase the power limit as needed.

  - **Command:** `ethtool --set-pse <interface> c33-pse-avail-pw-limit <limit>`

    Example:

    .. code-block:: bash

      ethtool --set-pse eth1 c33-pse-avail-pw-limit 18000
      ethtool --show-pse eth1

    **Expected Output** after adjusting the power limit:

    .. code-block:: bash

      Clause 33 PSE Available Power Limit: 18000

- **Scenarios**:

  - **Power Not Delivered**: If the `Power Detection Status` shows something
    other than "delivering power" (e.g., `over current` or `disabled`),
    troubleshoot the PSE or check for a short circuit in the cable.
  - **Power Delivered but No Link**: If power is being delivered but no link is
    established, proceed to **Cable Diagnostics** or **Inspect Link Status and
    PHY Configuration** for further troubleshooting.

Perform Cable Diagnostics
~~~~~~~~~~~~~~~~~~~~~~~~~

Use `ethtool` to test for physical layer issues such as cable faults. The test
results can vary depending on the cable's condition, the technology in use, and
the state of the link partner. The results from the cable test will help in
diagnosing issues like open circuits, shorts, impedance mismatches, and
noise-related problems.

- **Command:** `ethtool --cable-test <interface>`

The following are the typical outputs for **Single-Pair Ethernet (SPE)** and
**Multi-Pair Ethernet (MPE)**:

- **For Single-Pair Ethernet (SPE)**:
  - **Expected Output (SPE)**:

  .. code-block:: bash

    Cable test completed for device eth1.
    Pair A, fault length: 25.00m
    Pair A code Open Circuit

  This indicates an open circuit or cable fault at the reported distance, but
  results can be influenced by the link partner's state. Refer to the
  **"Troubleshooting Based on Cable Test Results"** section for further
  interpretation of these results.

- **For Multi-Pair Ethernet (MPE)**:
  - **Expected Output (MPE)**:

  .. code-block:: bash

    Cable test completed for device eth0.
    Pair A code OK
    Pair B code OK
    Pair C code Open Circuit

  Here, Pair C is reported as having an open circuit, while Pairs A and B are
  functioning correctly. However, if autonegotiation is in use on Pairs A and
  B, the cable test may be disrupted. Refer to the **"Troubleshooting Based on
  Cable Test Results"** section for a detailed explanation of these issues and
  how to resolve them.

For detailed descriptions of the different possible cable test results, please
refer to the **"Troubleshooting Based on Cable Test Results"** section.

Troubleshooting Based on Cable Test Results
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

After running the cable test, the results can help identify specific issues in
the physical connection. The results can be one of the following:

- **OK**:

  - The cable is functioning correctly, and no issues were detected.

  - **Action**: If you are still experiencing issues, it might be related to
    higher-layer problems, such as duplex mismatches or speed negotiation,
    which are not physical-layer issues.

  - **Special Case for `BaseT1` (1000/100/10BaseT1)**: In `BaseT1` systems, an
    "OK" result typically also means that the link is up and likely in **slave
    mode**, since cable tests usually only pass in this mode. For some
    **10BaseT1L** PHYs, an "OK" result may occur even if the cable is too long
    for the PHY's configured range (for example, when the range is configured
    for short-distance mode).

- **Open Circuit**:

  - An **Open Circuit** result typically indicates that the cable is damaged or
    disconnected at the reported fault length. Consider these possibilities:

    - If the link partner is in **admin down** state or powered off, you might
      still get an "Open Circuit" result even if the cable is functional.

    - **Action**: Inspect the cable at the fault length for visible damage or
      loose connections. Verify the link partner is powered on and in the
      correct mode.

- **Short within Pair**:

  - A **Short within Pair** indicates an unintended connection within the same
    pair of wires, typically caused by physical damage to the cable.

    - **Action**: Replace or repair the cable and check for any physical damage
      or improperly crimped connectors.

- **Short to Another Pair**:

  - A **Short to Another Pair** means the wires from different pairs are
    shorted, which could occur due to physical damage or incorrect wiring.

    - **Action**: Replace or repair the damaged cable. Inspect the cable for
      incorrect terminations or pinched wiring.

- **Impedance Mismatch**:

  - **Impedance Mismatch** indicates a reflection caused by an impedance
    discontinuity in the cable. This can happen when a part of the cable has
    abnormal impedance (e.g., when different cable types are spliced together
    or when there is a defect in the cable).

    - **Action**: Check the cable quality and ensure consistent impedance
      throughout its length. Replace any sections of the cable that do not meet
      specifications.

- **Noise**:

  - **Noise** means that the Time Domain Reflectometry (TDR) test could not
    complete due to excessive noise on the cable, which can be caused by
    interference from electromagnetic sources.

    - **Action**: Identify and eliminate sources of electromagnetic
      interference (EMI) near the cable. Consider using shielded cables or
      rerouting the cable away from noise sources.

- **Resolution Not Possible**:

  - **Resolution Not Possible** means that the TDR test could not detect the
    issue due to the resolution limitations of the test or because the fault is
    beyond the distance that the test can measure.

    - **Action**: Inspect the cable manually if possible, or use alternative
      diagnostic tools that can handle greater distances or higher resolution.

- **Unknown**:

  - An **Unknown** result may occur when the test cannot classify the fault or
    when a specific issue is outside the scope of the tool's detection
    capabilities.

    - **Action**: Re-run the test, verify the link partner's state, and inspect
      the cable manually if necessary.

**Scenarios**:

  - Always take the state of the link partner into account when interpreting
    the results. For **BaseT1** PHYs, an "OK" result usually indicates the link
    is up and in **slave mode**. If the cable is too long (for example, in
    **10BaseT1L** PHYs configured for short-distance mode), it might still pass
    the test but lead to intermittent link issues.
  - In cases where autonegotiation is used (for **Multi-Pair Ethernet**), the
    test results may be disrupted by autonegotiation pulses, leading to a
    "Noise" result or inaccurate reporting for pairs used for autonegotiation
    (e.g., Pairs A and B).

Verify Link Partner PHY Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the cable test passes but the link is still not functioning correctly, it’s
essential to verify the configuration of the link partner’s PHY. Mismatches in
speed, duplex settings, or master-slave roles can cause connection issues.

- **Autonegotiation Mismatch**:

  - If both link partners support autonegotiation, ensure that autonegotiation
    is enabled on both sides. A mismatch can lead to connectivity problems or
    performance degradation.

  - **Quick Fix:** Reset autonegotiation to the default settings, which will
    advertise all default link modes:

  .. code-block:: bash

    ethtool -s <interface> autoneg on

  - **Command to check configuration:** `ethtool <interface>`

  - **Expected Output:** Ensure that both sides advertise compatible link
    modes. If autonegotiation is off, verify that both link partners are
    configured for the same speed and duplex.

    Example:

  .. code-block:: bash

    Settings for eth0:
       Supported link modes:  1000baseT/Full, 100baseT/Full
       Advertised link modes: 1000baseT/Full
       Speed: 1000Mb/s
       Duplex: Full
       Auto-negotiation: on

- **Combined Mode Mismatch (Autonegotiation on one side, Forced on the other)**:

  - One of the most common issues occurs when one side is using
    **autonegotiation** (as in most modern systems), and the other side is set
    to a **forced link mode** (e.g., older hardware with single-speed hubs). In
    such cases, modern PHYs will attempt to detect the forced mode on the other
    side. If the link is established, you may notice:

    - **No or empty "Link partner advertised link modes"**.

    - **"Link partner advertised auto-negotiation:"** will be **"no"**.

  - This type of detection does not always work reliably:

    - If the link partner with the forced mode switches speed, the modern PHY
      may not always detect the change correctly.

    - Typically, the modern PHY will default to **Half Duplex**, even if the
      link partner is actually configured for **Full Duplex**.

  - **Action**: Set both sides to the same fixed speed and duplex mode to avoid
    potential detection issues.

  .. code-block:: bash

    ethtool -s <interface> speed 1000 duplex full autoneg off

- **Master/Slave Role Mismatch (BaseT1 and 1000BaseT PHYs)**:

  - In **BaseT1** systems (e.g., 1000BaseT1, 100BaseT1), link establishment
    requires that one device is configured as **master** and the other as
    **slave**. A mismatch in this master-slave configuration can prevent the
    link from being established. However, **1000BaseT** also supports
    configurable master/slave roles and can face similar issues.
  - **Role Preference in 1000BaseT**: The **1000BaseT** specification allows
    link partners to negotiate master-slave roles during autonegotiation. Some
    PHYs have hardware limitations or bugs that prevent them from functioning
    properly in certain modes. In such cases, drivers may force these PHYs into
    a specific role (e.g., **forced master** or **forced slave**). If both link
    partners have the same issue and are forced into the same mode (e.g., both
    forced into master mode), they will not be able to establish a link.

  - **Action**: Ensure that one side is configured as **master** and the other
    as **slave** to avoid this issue, particularly when hardware limitations
    are involved. Check for any driver-related restrictions or forced modes.

  - **Command to force master/slave mode**:

    .. code-block:: bash

      ethtool -s <interface> master-slave forced-master speed 1000 duplex full autoneg off

  - **Check the current master/slave status**:

    .. code-block:: bash

      ethtool <interface>

    Example Output:

    .. code-block:: bash

      master-slave cfg: forced-master
      master-slave status: slave

  - **Hardware Bugs and Driver Forcing**: If a known hardware issue forces the
    PHY into a specific mode, it’s essential to check the driver or hardware
    documentation for details. Ensure that the roles are compatible across both
    link partners, and if both PHYs are forced into the same mode, adjust one
    side accordingly to resolve the mismatch.

Monitor Link Resets and Speed Drops
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the link is unstable, showing frequent resets or speed drops, this may
indicate issues with the cable, PHY configuration, or environmental factors.
Currently, there is no unified way in Linux to directly monitor downshift
events or link speed changes via user space tools. However, the Linux kernel
logs provide valuable insights if the driver supports reporting downshifts or
other link changes.

- **Monitor Kernel Logs for Link Resets and Speed Drops**:

  - The Linux kernel will print link status changes, including downshift
    events, in the system logs. The message typically includes speed changes,
    duplex mode, and downshifted link speed (if the driver supports it).
  - **Command to monitor kernel logs in real-time:**

    .. code-block:: bash

      dmesg -w | grep "Link is Up\|Link is Down"

  - Example Output (if a downshift occurs):

    .. code-block:: bash

      eth0: Link is Up - 100Mbps/Full (downshifted) - flow control rx/tx
      eth0: Link is Down

    This indicates that the link has been established but has downshifted from
    a higher speed.

  - **Note**: Not all drivers or PHYs support downshift reporting, so you may
    not see this information for all devices.

- **Check Link Status and Speed**:

  - Even though downshift counts or events are not easily tracked, you can still
    use `ethtool` to manually check the current link speed and status.

  - **Command:** `ethtool <interface>`

  - **Expected Output:**

    .. code-block:: bash

      Speed: 1000Mb/s
      Duplex: Full
      Auto-negotiation: on
      Link detected: yes

    Any inconsistencies in the expected speed or duplex setting could indicate
    an issue.

- **Disable Energy-Efficient Ethernet (EEE) for Diagnostics**:

  - **EEE** (Energy-Efficient Ethernet) can be a source of link instability
    due to transitions in and out of low-power states. For diagnostic purposes,
    it may be useful to **temporarily** disable EEE to determine if it is
    contributing to link instability. This is **not a generic recommendation**
    for disabling power management.

  - **Action**: Disable EEE and monitor if the link becomes stable. If
    disabling EEE resolves the issue, report the bug so that the driver can be
    fixed.

  - **Command:**

    .. code-block:: bash

      ethtool --set-eee <interface> eee off

    - **Important**: If disabling EEE resolves the instability, the issue
      should be reported to the maintainers as a bug, and the driver should be
      corrected to handle EEE properly without causing instability. Disabling
      EEE permanently should not be seen as a solution.

- **Monitor Error Counters**:

  - While some NIC drivers and PHYs provide error counters, there is no unified
    set of PHY-specific counters across all hardware. Additionally, not all PHYs
    provide useful information related to errors like CRC errors, frame drops,
    or link flaps. Therefore, this step is dependent on the specific hardware
    and driver support.

  - **Action**: Use `ethtool -S <interface>` to check if your driver provides
    useful error counters. In some cases, counters may provide information about
    errors like link flaps or physical layer problems (e.g., excessive CRC
    errors), but results can vary significantly depending on the PHY.

  - **Command:** `ethtool -S <interface>`

  - **Example Output (if supported)**:

    .. code-block:: bash

      rx_crc_errors: 123
      tx_errors: 45
      rx_frame_errors: 78

  - **Note**: If no meaningful error counters are available or if counters are
    not supported, you may need to rely on physical inspections (e.g., cable
    condition) or kernel log messages (e.g., link up/down events) to further
    diagnose the issue.

When All Else Fails...
~~~~~~~~~~~~~~~~~~~~~~

So you've checked the cables, monitored the logs, disabled EEE, and still...
nothing? Don’t worry, you’re not alone. Sometimes, Ethernet gremlins just don’t
want to cooperate.

But before you throw in the towel (or the Ethernet cable), take a deep breath.
It’s always possible that:

1. Your PHY has a unique, undocumented personality.

2. The problem is lying dormant, waiting for just the right moment to magically
   resolve itself (hey, it happens!).

3. Or, it could be that the ultimate solution simply hasn’t been invented yet.

If none of the above bring you comfort, there’s one final step: contribute! If
you've uncovered new or unusual issues, or have creative diagnostic methods,
feel free to share your findings and extend this documentation. Together, we
can hunt down every elusive network issue - one twisted pair at a time.

Remember: sometimes the solution is just a reboot away, but if not, it’s time to
dig deeper - or report that bug!

Technical Overview and Detailed Explanations
--------------------------------------------

This section provides an in-depth exploration of twisted pair Ethernet
variants, typical Layer 1 issues, pair assignments, and hardware
specifications. While the first part of this document focuses on immediate
troubleshooting, the following details aim to help developers, network
engineers, and advanced users understand the underlying problems and
considerations when diagnosing twisted pair Ethernet at OSI Layer 1.

List of Twisted Pair Ethernet Link Modes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Twisted pair Ethernet variants utilize copper cabling with pairs of wires
twisted together to reduce electromagnetic interference (EMI). These link modes
are widely used in local area networks (LANs) due to their balance of
cost-effectiveness and performance.

Below is a list of Ethernet link modes that operate over twisted pair copper
cabling. Half and Full duplex variants are combined where applicable.

- **10baseT Half/Full**:

  - The original Ethernet standard over twisted pair cabling.
  - Supports both half-duplex and full-duplex modes.

- **10baseT1L Full**:

  - Long-reach variant of Ethernet over a single twisted pair.
  - Supports **autonegotiation** and offers two signal amplitude options:

    - **2.4 Vpp** for distances up to **1000 meters**.
    - **1 Vpp** for distances up to **200 meters** (used in hazardous
      environments).

  - Primarily used in industrial and building automation environments.

- **10baseT1S Half/Full**:

  - Short-reach variant of Ethernet over a single twisted pair.
  - Does not support autonegotiation, targeting **fast link establishment within
    ~10 ms**.
  - Primarily designed for compact locations, such as automotive environments,
    where sensors and actuators are clustered.
  - Supports **multidrop (point-to-multipoint)** configurations, typically used
    to connect clusters of sensors.

- **100baseT Half/Full**:

  - Also known as Fast Ethernet.
  - Operates at 100 Mbps over twisted pair cabling.
  - Supports both half-duplex and full-duplex modes.

- **100baseT1 Full**:

  - Operates at 100 Mbps over a single twisted pair.
  - Does not support autonegotiation, targeting **fast link creation within
    ~10 ms**.
  - Primarily used in automotive and industrial applications.

- **1000baseT Full**:

  - Gigabit Ethernet over twisted pair cabling.
  - Full-duplex mode is standard and widely used.
  - Half-duplex mode is not supported by the IEEE 802.3ab standard but may be
    present in some hardware implementations.

- **1000baseT1 Full**:

  - Gigabit Ethernet over a single twisted pair.
  - Does not support autonegotiation, targeting **fast link creation within
    ~10 ms**.
  - Primarily targeted for automotive and industrial use cases.

- **2500baseT and 5000baseT Full**:

  - Multi-Gigabit Ethernet standards.
  - Designed to provide higher speeds over existing Cat5e/Cat6 cabling.
  - Operate at 2.5 Gbps and 5 Gbps respectively.

- **10000baseT Full**:

  - 10 Gigabit Ethernet over twisted pair.
  - Requires Cat6a or better cabling to achieve full distance (up to 100 meters).

Potential Layer 1 Related Issues
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

OSI Layer 1 issues pertain to the physical aspects of network communication.
Some of these issues are interrelated or subsets of larger problems, impacting
network performance and connectivity. Below is a structured overview of common
Layer 1 issues, grouped by their relationships:

Cable Damage and Related Issues
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- **Cable Damage**:

  - **Description**: Physical damage to the Ethernet cable, including cuts,
    bends, or degradation due to environmental factors such as heat, moisture,
    or mechanical stress.
  - **Symptoms**: Intermittent connectivity, reduced speed, or no link.
  - **Detection**: Cable testers or PHY diagnostics with time-domain
    reflectometry (TDR) support.

  - **Subsets of Cable Damage**:

    - **Open Circuit**:

      - **Description**: A break or discontinuity in the cable or connector
        resulting in no electrical connection.
      - **Symptoms**: No link is detected.
      - **Detection**: PHY diagnostics can report "Open Circuit".
    - **Short Circuit**:

      - **Description**: An unintended electrical connection between two wires
        that should be separate.
      - **Symptoms**: The link may not establish, or the link may drop repeatedly.
      - **Detection**: Cable testers or PoE/PoDL power detection circuits may
        detect excessive current draw.
    - **Impedance Mismatch**:

      - **Description**: Poor cable quality or incorrect termination causes
        reflections of the signal due to impedance variations.
      - **Symptoms**: Reduced signal quality, intermittent connectivity at
        higher speeds.
      - **Detection**: TDR diagnostics can detect impedance mismatches.

Wiring Issues
^^^^^^^^^^^^^

- **Incorrect Wiring or Pinout**:

  - **Description**: Incorrect pair wiring or non-standard pin assignments can
    cause link failure or degraded performance.
  - **Symptoms**: No link, reduced speed, or high error rates, especially in
    multi-pair Ethernet standards (e.g., 1000BASE-T).
  - **Detection**: Modern PHYs may detect and correct some wiring errors
    (e.g., MDI/MDI-X auto-crossover), but cable testers provide the most
    reliable diagnostics.

  - **Subsets of Incorrect Wiring**:

    - **Miswired Pairs in Multi-Pair Link Modes**:

      - **Description**: In multi-pair standards like 10BASE-T, 100BASE-TX, or
        1000BASE-T, miswired pairs can cause link failures.
      - **Symptoms**: Incompatible wiring may work for some speeds (e.g.,
        100BASE-TX) but fail for higher speeds (e.g., 1000BASE-T).
      - **Detection**: Cable testers or PHY diagnostics may identify the issue.

    - **Polarity Reversal within Pairs**:

      - **Description**: The positive and negative wires within a pair are
        swapped.
      - **Symptoms**: No link or intermittent connection unless modern PHYs with
        automatic polarity correction are in use.
      - **Detection**: Modern PHYs can detect and correct polarity reversal.
        Some expose polarity status in diagnostic registers.

    - **Split Pairs**:

      - **Description**: The two wires of a pair are split across different
        pairs, reducing the effectiveness of signal twisting.
      - **Symptoms**: Increased crosstalk, higher error rates, and intermittent
        link drops, particularly at higher speeds like 1000BASE-T.
      - **Detection**: Cable testers can detect split pairs, and error counters
        in the PHY may provide an indication.

Environmental and External Factors
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- **Electromagnetic Interference (EMI)**:

  - **Description**: External electromagnetic fields can interfere with Ethernet
    signals, particularly in unshielded twisted pair (UTP) cables.
  - **Symptoms**: Increased transmission errors, reduced speed, or intermittent
    link drops.
  - **Detection**: Error counters in the PHY or signal quality indicators (SQI)
    may help diagnose EMI issues.

- **Environmental Factors**:

  - **Description**: External environmental conditions such as temperature
    extremes, moisture, UV exposure, or mechanical stress can degrade the cable
    or connectors, leading to signal degradation.
  - **Symptoms**: Increased error rates, intermittent connectivity, or link
    failure.
  - **Detection**: Error counters and physical inspection can reveal issues
    related to environmental degradation.

  - **Related Issues**:

    - **Excessive Cable Length**:

      - **Description**: Exceeding the maximum allowed cable length for a given
        standard can lead to signal loss and degradation.
      - **Symptoms**: Intermittent connectivity, reduced speed, or no link.
      - **Detection**: TDR diagnostics can measure the cable length. Error
        counters may show performance degradation.

Cable Quality and Type
^^^^^^^^^^^^^^^^^^^^^^

- **Use of Incorrect Cable Type**:

  - **Description**: Using a cable that doesn’t meet the required standards for
    a specific Ethernet mode (e.g., using CAT5e for 10GBASE-T) or improper
    shielding.
  - **Symptoms**: Reduced link speed, increased errors, or no link.
  - **Detection**: PHY diagnostics such as SQI and cable testers can help detect
    cable quality issues.

  - **Related Issue**:

    - **Shielding Problems**: Improper or incomplete attachment of the shield
      can lead to similar symptoms as EMI issues. Variants include:

      - **Unattached Shielding**: Shielding present but not connected at the
        connector.
      - **Unconnected Device Ports**: Even if the shield is attached, the device
        port may not provide a connection.

Hardware Issues
^^^^^^^^^^^^^^^

- **Faulty Network Interface Cards (NICs) or PHYs**:

  - **Description**: Malfunctioning hardware components such as NICs or PHYs may
    cause link problems.
  - **Symptoms**: Network performance degradation or complete failure.
  - **Detection**: Some PHYs and NICs perform self-tests and may report errors
    in system logs. Swapping hardware may be required to diagnose these issues.

Pair Assignment Issues in Multi-Pair Link Modes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Ethernet standards that use **two or more pairs** of wires - such as
**10BASE-T**, **100BASE-TX**, **1000BASE-T**, and higher - require correct pair
assignments for proper operation. Incorrect pair assignments can cause
significant network problems, especially as data rates increase.

Multi-Pair Link Modes
^^^^^^^^^^^^^^^^^^^^^

- **Applicable Ethernet Standards**:

  - **10BASE-T** (10 Mbps Ethernet)
  - **100BASE-TX** (Fast Ethernet)
  - **1000BASE-T** (Gigabit Ethernet)
  - **2.5GBASE-T**, **5GBASE-T**, **10GBASE-T**

Pin and Pair Naming Conventions
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

In Ethernet troubleshooting, understanding pin, pair, and color-coding
conventions is essential, especially when physical cable repairs are necessary.
One major challenge arises in the field when a damaged cable pair needs to be
identified and fixed without the ability to replace the entire cable. While
Linux diagnostics typically only provide pair names (e.g., "Pair A"), these
names do not directly map to the color codes commonly used for cable
identification in the field.

To further complicate the issue, different standards—such as **TIA-568** and
**IEEE 802.3**—use varying conventions for assigning pins to pairs, and pairs
to color codes. For example, the pair names reported in diagnostics must be
translated into physical wire colors, which differ between **TIA-568A** and
**TIA-568B** layouts. This translation process is crucial for accurately
identifying and repairing the correct cable pair.

Although Linux diagnostic tools provide valuable information, their focus on
pair names can make it challenging to map these names to the physical cable
layout, particularly in fieldwork where color-coded wires are the primary means
of identification. This section aims to highlight this problem and provide
enough background on pin, pair, and color-coding conventions to assist with
analyzing and addressing these issues. While this guide may not fully resolve
the difficulties, it offers important context to help bridge the gap between
diagnostics and physical cable repair.

TIA-568 Pair and Pin Assignments
""""""""""""""""""""""""""""""""

The **TIA-568** standard defines two wiring schemes for terminating twisted
pair Ethernet cables: **T568A** and **T568B**. Each scheme assigns wire colors
and pair numbers to specific pin numbers on the RJ45 connector.

There are multiple opinions available online about why T568A and T568B
co-exist. However, according to the **TIA/EIA-568-B.2-2001** specification, no
explanation is provided for the existence of both variants. The only additional
information found is that:

- **T568A** is recommended by the U.S. Federal Government as the standard
  wiring scheme, as noted in the specification.

The specification does not provide any specific reasoning for the use of
**T568B**. Both schemes are considered electrically equivalent and acceptable
for telecommunications installations.

Additionally, per TIA/EIA-568-B.2-2001, there is no formal definition of
crossover cabling for T568A or T568B. The specification states: "Because of
their identical pair groupings, patch cords terminated in either T568A or T568B
may be used interchangeably, provided that both ends are terminated with the
same pin/pair scheme."

Due to the coexistence of these standards, it presents an additional challenge
for in-field repair attempts. Repair technicians must be aware of which
standard is in use to avoid mixing pin-to-pair assignments. Misinterpreting the
information provided by Linux diagnostic tools can result in incorrect
identification of the pairs. This could lead to technicians mistakenly opening
or modifying the wrong pair, potentially increasing the time needed for repairs
and causing further connectivity issues.

Pin and Pair Layout According to TIA-568 Specification
""""""""""""""""""""""""""""""""""""""""""""""""""""""

+----------------+-------------------+-------------------+--------------------+--------------------+
| **Pin Number** | **TIA-568A Pair** | **TIA-568B Pair** | **TIA-568A Color** | **TIA-568B Color** |
+================+===================+===================+====================+====================+
| 1              | Pair 3            | Pair 2            | White/Green        | White/Orange       |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 2              | Pair 3            | Pair 2            | Green              | Orange             |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 3              | Pair 2            | Pair 3            | White/Orange       | White/Green        |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 4              | Pair 1            | Pair 1            | Blue               | Blue               |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 5              | Pair 1            | Pair 1            | White/Blue         | White/Blue         |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 6              | Pair 2            | Pair 3            | Orange             | Green              |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 7              | Pair 4            | Pair 4            | White/Brown        | White/Brown        |
+----------------+-------------------+-------------------+--------------------+--------------------+
| 8              | Pair 4            | Pair 4            | Brown              | Brown              |
+----------------+-------------------+-------------------+--------------------+--------------------+

- **Pair Numbers in TIA-568A and TIA-568B**:

  - **TIA-568A**:

    - **Pair 1:** Pins 4 & 5
    - **Pair 2:** Pins 3 & 6
    - **Pair 3:** Pins 1 & 2
    - **Pair 4:** Pins 7 & 8

  - **TIA-568B**:

    - **Pair 1:** Pins 4 & 5
    - **Pair 2:** Pins 1 & 2
    - **Pair 3:** Pins 3 & 6
    - **Pair 4:** Pins 7 & 8

IEEE 802.3 Pair and Pin Assignments
"""""""""""""""""""""""""""""""""""

The **IEEE 802.3** standard specifies the Medium Dependent Interface (**MDI**)
pinouts for various Ethernet physical layers. Below are the pin assignments for
different Ethernet standards, including both MDI and MDI-X configurations where
applicable. **MDI-X** is often associated with crossover configurations, and
the signal names in MDI and MDI-X modes reflect whether the pair is used for
transmit (TX), receive (RX), or bidirectional communication (BI for
bidirectional data).

In Ethernet connections, **PHYs cannot be used directly in a one-to-one
connection** without switching the pair connections. This pair switching can be
done either:

- **Externally by the cable/connector layout** (commonly referred to as a
  crossover cable), or
- **Internally by the PHY**, where one device operates in **MDI** mode and the
  other in **MDI-X** mode.

If the pair switching is done externally (with a crossover cable), both PHYs
will be in the **same mode** (both in MDI or both in MDI-X). If the switching
is not done externally, one PHY must be in **MDI** mode, and the other in
**MDI-X** mode to ensure proper communication between the devices.

It is important to note that IEEE 802.3 does not provide pin-to-color mapping,
and pin-to-pair mapping has evolved over time. For example, older link types
like **10BASE-T** or **100BASE-TX** did not have official pair names.

It is important to note that IEEE 802.3 does not provide pin-to-color mapping,
and pin-to-pair mapping has evolved over time. For example, older link types
like **10BASE-T** or **100BASE-TX** did not have official pair names. In this
documentation, we have extended the pin-to-signal tables with pin-to-pair
mapping in a way that aligns with the Linux implementation for easier
interpretation and diagnostic purposes.

10BASE-T (10 Mbps Ethernet)
"""""""""""""""""""""""""""

According to **IEEE 802.3 - 2018, Section 14.5.1 (MDI connectors for
10BASE-T)**, the Medium Dependent Interface (MDI) defines the pin assignments
for 10BASE-T Ethernet connections. Below are the pin assignments for both MDI
and MDI-X modes, with different pair layouts for each.

+---------+----------------+------------------+---------------+----------------+
| **Pin** | **MDI Signal** | **MDI-X Signal** | **MDI Pair**  | **MDI-X Pair** |
+=========+================+==================+===============+================+
| 1       | TD+            | RD+              | Pair A        | Pair B         |
+---------+----------------+------------------+---------------+----------------+
| 2       | TD−            | RD−              | Pair A        | Pair B         |
+---------+----------------+------------------+---------------+----------------+
| 3       | RD+            | TD+              | Pair B        | Pair A         |
+---------+----------------+------------------+---------------+----------------+
| 4       | Not used       | Not used         | Not used      | Not used       |
+---------+----------------+------------------+---------------+----------------+
| 5       | Not used       | Not used         | Not used      | Not used       |
+---------+----------------+------------------+---------------+----------------+
| 6       | RD−            | TD−              | Pair B        | Pair A         |
+---------+----------------+------------------+---------------+----------------+
| 7       | Not used       | Not used         | Not used      | Not used       |
+---------+----------------+------------------+---------------+----------------+
| 8       | Not used       | Not used         | Not used      | Not used       |
+---------+----------------+------------------+---------------+----------------+

- **Used Pairs**:

  - **MDI**:

    - **Pair A (Transmit)**: Pins 1 & 2
    - **Pair B (Receive)**: Pins 3 & 6

  - **MDI-X**:

    - **Pair A (Transmit)**: Pins 3 & 6
    - **Pair B (Receive)**: Pins 1 & 2

**Reference**: IEEE 802.3 - 2018, Section 14.5.1 MDI connectors (10BASE-T)

100BASE-TX (Fast Ethernet)
""""""""""""""""""""""""""

**100BASE-TX**, used in Fast Ethernet, transmits data over two pairs of twisted
pair cables. It operates in full-duplex mode and uses separate pairs for
transmission and reception. The pin assignments differ between **MDI** and
**MDI-X** modes, where the transmit and receive pairs are swapped to facilitate
communication without needing a crossover cable.

+---------+------------------+-------------------+---------------+----------------+
| **Pin** | **MDI Signal**   | **MDI-X Signal**  | **MDI Pair**  | **MDI-X Pair** |
+=========+==================+===================+===============+================+
| 1       | TX_D1+           | RX_D2+            | Pair A        | Pair B         |
+---------+------------------+-------------------+---------------+----------------+
| 2       | TX_D1−           | RX_D2−            | Pair A        | Pair B         |
+---------+------------------+-------------------+---------------+----------------+
| 3       | RX_D2+           | TX_D1+            | Pair B        | Pair A         |
+---------+------------------+-------------------+---------------+----------------+
| 4       | Not used         | Not used          | Not used      | Not used       |
+---------+------------------+-------------------+---------------+----------------+
| 5       | Not used         | Not used          | Not used      | Not used       |
+---------+------------------+-------------------+---------------+----------------+
| 6       | RX_D2−           | TX_D1−            | Pair B        | Pair A         |
+---------+------------------+-------------------+---------------+----------------+
| 7       | Not used         | Not used          | Not used      | Not used       |
+---------+------------------+-------------------+---------------+----------------+
| 8       | Not used         | Not used          | Not used      | Not used       |
+---------+------------------+-------------------+---------------+----------------+

- **Used Pairs**:

  - **MDI**:

    - **Pair A (Transmit)**: Pins 1 & 2
    - **Pair B (Receive)**: Pins 3 & 6

  - **MDI-X**:

    - **Pair A (Transmit)**: Pins 3 & 6
    - **Pair B (Receive)**: Pins 1 & 2

**Reference**: IEEE 802.3 - 2018, Section 32.8.1 MDI connectors (100BASE-T2)

1000BASE-T (Gigabit Ethernet)
"""""""""""""""""""""""""""""

**1000BASE-T**, used in Gigabit Ethernet, transmits data over all four pairs of
twisted pair cables. It operates in full-duplex mode and uses bidirectional
communication on each pair. The pin assignments differ between **MDI** and
**MDI-X** modes, where the transmit and receive signals are swapped for
compatibility with different devices.

+---------+------------------+------------------+---------------+----------------+
| **Pin** | **MDI Signal**   | **MDI-X Signal** | **MDI Pair**  | **MDI-X Pair** |
+=========+==================+==================+===============+================+
| 1       | BI_DA+           | BI_DB+           | Pair A        | Pair B         |
+---------+------------------+------------------+---------------+----------------+
| 2       | BI_DA−           | BI_DB−           | Pair A        | Pair B         |
+---------+------------------+------------------+---------------+----------------+
| 3       | BI_DB+           | BI_DA+           | Pair B        | Pair A         |
+---------+------------------+------------------+---------------+----------------+
| 4       | BI_DC+           | BI_DD+           | Pair C        | Pair D         |
+---------+------------------+------------------+---------------+----------------+
| 5       | BI_DC−           | BI_DD−           | Pair C        | Pair D         |
+---------+------------------+------------------+---------------+----------------+
| 6       | BI_DB−           | BI_DA−           | Pair B        | Pair A         |
+---------+------------------+------------------+---------------+----------------+
| 7       | BI_DD+           | BI_DC+           | Pair D        | Pair C         |
+---------+------------------+------------------+---------------+----------------+
| 8       | BI_DD−           | BI_DC−           | Pair D        | Pair C         |
+---------+------------------+------------------+---------------+----------------+

- **Used Pairs**:

  - **MDI**:

    - **Pair A (Transmit/Receive)**: Pins 1 & 2
    - **Pair B (Transmit/Receive)**: Pins 3 & 6
    - **Pair C (Transmit/Receive)**: Pins 4 & 5
    - **Pair D (Transmit/Receive)**: Pins 7 & 8

  - **MDI-X**:

    - **Pair A (Transmit/Receive)**: Pins 3 & 6
    - **Pair B (Transmit/Receive)**: Pins 1 & 2
    - **Pair C (Transmit/Receive)**: Pins 7 & 8
    - **Pair D (Transmit/Receive)**: Pins 4 & 5

**Note**: In **1000BASE-T**, all four pairs are used for bidirectional
communication. In **MDI-X** mode, the transmit and receive pairs are swapped,
allowing direct connections without requiring a crossover cable. If no
crossover cable is used, one device should be in **MDI** mode and the other in
**MDI-X** mode for correct communication.

**Reference**: IEEE 802.3 - 2018, Section 40.8.1 MDI connectors (1000BASE-T)

Summary of Pair Assignment Issues
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Miswired Pairs
""""""""""""""

- **Description**: Miswired pairs occur when the wire pairs are connected to
  the wrong pins in the connector. While this can lead to link failures, modern
  PHYs are often capable of handling such cases as long as the pairs follow the
  pair assignment conventions of the standards being used.

  - If direct cables are used with identical pin assignments on both ends,
    modern PHYs can detect this and automatically switch one side to **MDI**
    mode and the other to **MDI-X** mode. Most modern PHYs offer diagnostics to
    indicate whether **MDI** or **MDI-X** mode is in use and allow a choice
    between automatic or forced MDI/MDI-X modes.
  - **Older hardware**, such as old hubs, often lacked automatic switching
    capabilities. These devices had predefined configurations: "Normal" mode
    (MDI) for client devices like PCs or printers, and "Uplink" mode (MDI-X)
    for connecting between hubs. Such setups required manually ensuring the
    correct cable type was used (e.g., straight-through or crossover).

- **Mixed or Incompatible Crossover Pin/Pair Assignments**:

  - Miswiring or incorrect crossover configurations can result in various
    failure modes depending on the link speed. For example, if one end of a
    cable uses **T568A** and the other end uses **T568B** (creating a crossover
    connection), this may work for **10BASE-T** and **100BASE-TX**, but will
    fail for **1000BASE-T**.
  - This is because in **1000BASE-T**, all four pairs (A, B, C, and D) are
    used, and the mixed configuration would result in Pairs A and B being
    crossed while Pairs C and D remain straight, causing a communication
    breakdown.

- **Detection**:

  - Modern PHYs often have the ability to detect miswired pairs and may provide
    diagnostic information indicating whether the device is operating in
    **MDI** or **MDI-X** mode.
  - Cable testers can also be used to identify miswiring issues, especially in
    cases where automatic MDI/MDI-X switching is not functioning or supported.
    Old hubs or other legacy equipment that lack automatic MDI/MDI-X
    functionality may exhibit connectivity problems in such cases.

Split Pairs
"""""""""""

**Split Pairs** occur when the two wires of a twisted pair (e.g., **TX+** and
**TX−**) are incorrectly placed in different pairs within the cable. Instead of
being twisted together, they are paired with wires from other pairs. Twisting
is essential because it cancels out electromagnetic interference, ensuring
signal integrity.

- **Description**: A split pair happens when the wires that should form a pair
  are misconnected so that they end up in different pairs, usually due to
  improper wiring. This disrupts the twisting effect that minimizes crosstalk
  and interference.

  - **Example**: In a correctly wired Ethernet cable, the two wires of Pair A
    (Pins 1 & 2) are twisted together. In a split pair, one wire from Pair A is
    twisted with a wire from Pair B (Pins 3 & 6), significantly degrading
    signal integrity.

- **Symptoms**:

  - **Increased crosstalk and interference**: Without proper twisting,
    electromagnetic interference increases, leading to a higher error rate.

  - **Reduced link speed**: The link may fall back to a lower speed, especially
    at higher data rates like **1000BASE-T**, where all pairs are used.

  - **Link failure at higher speeds**: While split pairs may still allow
    **10BASE-T** or **100BASE-TX** to function, they usually cause failures at
    **1000BASE-T** or higher due to the increased sensitivity to interference.

  - **Intermittent connectivity**: Split pairs can cause intermittent link
    drops due to signal degradation.

- **Detection**:

  - **Cable testers**: Advanced cable testers can detect split pairs by
    checking if the wires in each pair are twisted together correctly.

  - **Error Counters**: Monitoring error counters, such as CRC errors, can help
    identify issues caused by split pairs, especially at higher speeds where
    interference is more problematic.

  - **Link Drop Counters**: Frequent link drops may indicate split pairs,
    particularly if they are affecting higher-speed links like **1000BASE-T**.

Polarity Reversal within Pairs
""""""""""""""""""""""""""""""

- **Description**: Polarity reversal happens when the positive and negative
  wires within a pair are swapped (e.g., **TX+** and **TX−** reversed). This
  can happen during improper termination or incorrect pin assignments.

- **Modern PHYs**: Many modern PHYs have automatic polarity detection and
  correction features, allowing them to adjust for this issue without any
  manual intervention. This ensures that even if the polarity within a pair is
  reversed, the link can still establish and function correctly. - Polarity
  detection indicators are specified for both **multi-pair** and **single-pair
  Ethernet** variations, such as in the **MultiGBASE-T** standards. - However,
  this feature may not be present in older PHYs, leading to potential failures
  or degraded performance if polarity reversal occurs.

- **Older Hardware**: Older devices without automatic polarity correction may
  fail to establish a link or exhibit reduced performance if polarity reversal
  occurs within the pairs.

- **Detection**: Polarity reversal can be detected using PHY diagnostics or
  cable testers. Advanced diagnostics in modern PHYs can indicate the polarity
  status of each pair.

Linux Kernel Recommendations for Improved Diagnostic Interfaces
---------------------------------------------------------------

As of **Linux kernel v6.11**, several improvements could be implemented to
enhance the diagnostic capabilities for Ethernet connections, particularly for
twisted pair Ethernet variants. These recommendations aim to address gaps in
diagnostics for OSI Layer 1 issues and provide more detailed insights for users
and developers.

This list will evolve with future kernel versions, reflecting new features or
refinements. Below are the current suggestions:

Unified PHY Error Counters and SQI Interface for Multi-Pair Ethernet (MPE)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Problem:**

  - Currently, PHY driver authors have the freedom to implement various
    PHY-supported counters, and the names of these counters often vary based on
    specific PHY documentation. This lack of standardization creates challenges
    for automated diagnostics and makes step-by-step troubleshooting guides
    inconsistent, as different counters may have different names across PHYs.
  - Additionally, while the **Signal Quality Indicator (SQI)** interface
    exists, it was primarily designed for **Single Pair Ethernet (SPE)** use
    cases and supports only one channel. This interface does not extend to
    **Multi-Pair Ethernet (MPE)** variants, which need multi-channel support.

- **Recommendation:**

  - Introduce a **unified interface for PHY error counters** with predictable,
    standardized counter names that can be consistently referenced across
    different drivers and PHY implementations. This will ensure that error
    counters are universally accessible and allow for more reliable automated
    diagnostics and step-by-step guides.
  - Extend the existing **SQI interface** to support **multi-pair Ethernet
    (MPE)** use cases by enabling **multi-channel SQI reporting**. For MPE,
    each pair/channel should have its own SQI value, allowing for better
    diagnostics in environments using higher-bandwidth Ethernet connections
    such as **1000Base-T** or **10GBase-T**, where multiple pairs carry
    bidirectional data.

- **Rationale:**

  - A unified interface for error counters would make it easier for tools like
    `ethtool` to provide consistent diagnostic output across different PHYs,
    improving both manual and automated diagnostics.
  - Extending the SQI interface to support multiple channels for MPE would
    significantly improve diagnostics for higher-speed Ethernet variants,
    providing more granular data about signal quality across all pairs. This
    would make it possible to detect and isolate issues affecting specific
    pairs, which is crucial in environments where bandwidth and performance are
    critical.

Improved Granularity for Cable Test Results with Hardware-Specific Quirks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Problem:**

  - The granularity of cable testing is often limited by the specific hardware
    (PHY) implementation, particularly with Time Domain Reflectometry (TDR)
    variations. Some PHYs may struggle to differentiate between certain states,
    such as:

    - **Open Circuit** and **Link Partner Powered Down** (link partner is
      powered down, but the cable is intact).
    - **OK** and **Cable Too Long** (link is functional but exceeds the maximum
      supported cable length for the PHY).

  - Additionally, certain PHYs or link modes, such as **Base-T1** variants, may
    require the link partner to be in **forced slave mode** for the cable test
    to yield reliable results. If the link partner is in **forced master
    mode**, test results might not be usable.

- **Recommendation:**

  - Introduce a mechanism to report **hardware-specific quirks** or potential
    **diagnostic variations** in cable test results. This could be done via
    additional **flags or metadata** in the kernel-to-user space interface,
    notifying user space tools (e.g., `ethtool`) about specific quirks in the
    PHY's diagnostic capabilities. These flags could include:

    - **PHY_CABLE_OPEN_OR_PARTNER_DOWN**: Notifies that the PHY cannot reliably
      distinguish between an open circuit and the link partner being powered
      down or in a low-power state.
    - **PHY_CABLE_OK_ON_LENGTH_WARNING**: Indicates that the PHY reports the
      cable as OK, but the actual cable length exceeds the PHY's supported
      limit, potentially causing performance issues.
    - **PHY_CABLE_TEST_PARTNER_REQUIRED_SLAVE_MODE**: Suggests that the link
      partner needs to be in **forced slave mode** for a reliable cable test.
      If the local PHY is in **forced slave mode**, the partner is likely in
      **forced master mode**, making the cable test results unreliable if the
      link partner is up.

- **Potential Flags:**

  - **PHY_CABLE_OPEN_OR_PARTNER_DOWN**: For cases where the PHY cannot
    differentiate between an open circuit or a link partner that is powered
    down.
  - **PHY_CABLE_OK_ON_LENGTH_WARNING**: Indicates that the test reported the
    cable as OK, but the length is approaching or exceeding limits, potentially
    causing future performance issues.
  - **PHY_CABLE_TEST_PARTNER_REQUIRED_SLAVE_MODE**: Suggests that for accurate
    results, the link partner must be in forced slave mode; otherwise, the test
    may be unreliable if the link partner is in master mode.

- **Rationale:**

  - By providing this additional layer of information, user space tools can
    offer more context to users, helping them understand whether the diagnostic
    results may be influenced by hardware-specific limitations or link
    configurations. This would improve the accuracy and trustworthiness of
    diagnostic output, particularly in cases where the limitations of a
    specific PHY might otherwise obscure the real issue.
  - Standardizing these flags ensures that user space tools provide clearer
    diagnostics across different hardware implementations, improving
    troubleshooting efficiency and reducing unnecessary repair attempts.

Master/Slave Role Mismatch Detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Problem:**
  Master/slave role mismatch occurs when both link partners are configured in
  incompatible roles (e.g., both set to master or both to slave), preventing
  link establishment. This issue is especially relevant in BaseT1 modes and
  other Ethernet standards that support configurable roles. However, detecting
  such a mismatch requires specific PHY hardware capabilities, which are not
  universally available.

- **Recommendation:**

  For PHYs capable of detecting and reporting a role mismatch, introduce a flag
  in the `ethtool` UAPI under the link state reporting interface. If a role
  mismatch is detected, it should be reported through:

  - **ETHTOOL_LINK_EXT_STATE_ROLE_MISMATCH**: A new flag to indicate that a
    role mismatch has been detected by the PHY, based on the comparison of the
    local role configuration and the expected role of the link partner.

- **Rationale:**

  While the current kernel exposes local role configurations (e.g., through
  `ethtool`), detecting and reporting role mismatches in real-time requires
  support from the PHY hardware. For PHYs with this capability, implementing
  this flag allows for quicker diagnostics of role mismatches, helping users
  and administrators identify and resolve the issue without manually checking
  role configurations on both ends. This also minimizes potential downtime due
  to misconfigurations.

Link Speed Downshift Status Indicator
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Problem:**

  - While a downshift counter might indicate a reduction in link speed, what
    would be more helpful is a **flag** or **status interface** that indicates
    whether the link could potentially operate at a higher speed, but is
    currently downshifted due to physical layer issues.

- **Recommendation:**

  - Introduce a **downshift status flag** in the kernel and expose it through
    `ethtool`. This flag should indicate whether the link is running below the
    maximum possible speed due to issues like cable quality, interference, or
    excessive length.

- **Rationale:**

  - This would allow users to quickly diagnose that something is limiting their
    link speed without needing to monitor for downshift events. The presence of
    this flag would signal a potential issue with the cable or environment that
    needs further investigation.


