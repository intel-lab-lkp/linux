.. SPDX-License-Identifier: GPL-2.0

=====================================================
Virtual Grouped LED Driver with Multicolor ABI
=====================================================

:Author: Jonathan Brophy <professor_jonny@hotmail.com>
:Version: 4

Overview
========

The ``leds-group-virtualcolor`` driver provides virtual LED devices that
arbitrate control over shared physical LEDs based on priority. Multiple
virtual LEDs can reference the same physical LEDs, with winner-takes-all
arbitration determining which virtual LED controls the hardware.

This enables complex lighting scenarios where different subsystems (e.g.,
notifications, indicators, effects) can request LED control without explicit
coordination. The driver handles arbitration automatically using a
priority-based system with sequence number tiebreaking.

Key Features
============

* **Winner-takes-all arbitration**: Only ONE virtual LED controls hardware at any time
* **Priority-based selection**: Higher priority virtual LEDs win control
* **Sequence-based tiebreaking**: Most recent update wins among equal priorities
* **Multicolor ABI support**: Standard Linux multicolor LED interface
* **Deterministic channel ordering**: Channels sorted by LED_COLOR_ID value
* **Two operating modes**:

  - Multicolor mode (dynamic color mixing with intensity control)
  - Standard mode (fixed color multipliers, brightness-only control)

* **Gamma correction**: Optional perceptual brightness correction
* **Update batching**: Debounces rapid changes to reduce bus traffic
* **Comprehensive debugfs**: Runtime statistics and diagnostics (when CONFIG_DEBUG_FS enabled)
* **Power management**: Suspend/resume with state preservation

Hardware Support
================

The driver works with any physical LED devices that expose the standard
``led_classdev`` interface. Physical LEDs are referenced via device tree
phandles and can be:

* GPIO LEDs (gpio-leds compatible)
* PWM LEDs (pwm-leds compatible)
* I2C-connected LED controllers
* SPI-connected LED controllers
* Any device using the Linux LED subsystem

Architecture
============

Winner-Takes-All Arbitration
-----------------------------

The driver uses a winner-takes-all arbitration model:

1. Only virtual LEDs with brightness > 0 participate in arbitration
2. The virtual LED with the highest priority wins
3. If priorities are equal, the most recently updated virtual LED wins (sequence number)
4. The winner controls **ALL** physical LEDs
5. Physical LEDs not used by the winner are turned off

Each virtual LED has:

* **Priority** (0 to INT_MAX): Higher values win arbitration
* **Sequence number**: Atomic counter incremented on brightness changes
* **Channel configuration**: Maps physical LEDs to color channels

Channel Ordering
----------------

Physical LEDs are automatically grouped into channels by their color property.
**Channels are ordered by ascending LED_COLOR_ID value** (0, 1, 2, 3, ...).

This ordering is deterministic and can be verified at runtime via the
``multi_index`` sysfs attribute.

Example color ID values:

* LED_COLOR_ID_WHITE = 0
* LED_COLOR_ID_RED = 1
* LED_COLOR_ID_GREEN = 2
* LED_COLOR_ID_BLUE = 3
* LED_COLOR_ID_AMBER = 4
* LED_COLOR_ID_VIOLET = 5

For a virtual LED with ``leds = <&white>, <&red>, <&green>, <&blue>``:

* Channel order: [0]=white (ID 0), [1]=red (ID 1), [2]=green (ID 2), [3]=blue (ID 3)
* multi_index reports: "0 1 2 3"
* multi_intensity order: white red green blue

For a virtual LED with ``leds = <&red>, <&green>, <&blue>`` (no white):

* Channel order: [0]=red (ID 1), [1]=green (ID 2), [2]=blue (ID 3)
* multi_index reports: "1 2 3"
* multi_intensity order: red green blue

Brightness Calculation
----------------------

Final physical LED brightness is calculated as::

    channel_value = intensity * multiplier / 255  (in multicolor mode)
                    multiplier                     (in standard mode)

    scaled_value = channel_value * vled_brightness / vled_max_brightness

    final_brightness = gamma_table[scaled_value]   (if gamma enabled)
                       scaled_value                 (if gamma disabled)

Locking Hierarchy
-----------------

To prevent deadlocks, locks must be acquired in this order:

1. ``vcolor_controller.lock`` (per-controller, protects arbitration state)
2. ``global_owner_rwsem`` (global, protects physical LED ownership)
3. ``virtual_led.lock`` (per-vLED, protects channel data)

Virtual LED locks are never held during arbitration. The driver copies
channel state under lock, then releases before processing.

Device Tree Bindings
====================

Controller Node
---------------

``compatible``
    Must be "leds-group-virtualcolor"

``#address-cells``
    Must be 1

``#size-cells``
    Must be 0

Child Node Properties (Virtual LEDs)
-------------------------------------

Each child node represents one virtual LED.

``reg``
    Unique index for this virtual LED (required)

``color``
    LED_COLOR_ID value (typically LED_COLOR_ID_MULTI for multicolor LEDs)

``function``
    LED function identifier (e.g., LED_FUNCTION_STATUS)

``leds``
    Phandle array referencing physical LED devices (required).
    Physical LEDs are grouped by their color property into channels.
    Channel order is determined by ascending LED_COLOR_ID value.

``priority``
    Integer priority value (0 to 2147483647). Higher values win
    arbitration. Default: 0

``led-mode``
    Operating mode, either "multicolor" or "standard".
    Default: "multicolor"

    * **multicolor**: Intensity can be changed via multi_intensity sysfs
    * **standard**: Color fixed by multipliers, only brightness control available

``mc-channel-multipliers``
    Array of u32 values (0-255), one per color channel. Optional.
    Must be ordered to match the channel order (sorted by color ID).
    Default: 255 for all channels

    * In multicolor mode: Scales intensity values
    * In standard mode: Defines fixed color mix (required)

``linux,default-trigger``
    Default LED trigger (e.g., "heartbeat", "none")

Example Device Tree
-------------------

Basic RGB LED with Priority
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: dts

    #include <dt-bindings/leds/common.h>

    virtualcolor {
        compatible = "leds-group-virtualcolor";
        #address-cells = <1>;
        #size-cells = <0>;

        notification_led: virtual-led@0 {
            reg = <0>;
            color = <LED_COLOR_ID_MULTI>;
            function = LED_FUNCTION_STATUS;
            priority = <100>;
            led-mode = "multicolor";
            leds = <&red_led>, <&green_led>, <&blue_led>;
            /* Channels: [0]=red (ID 1), [1]=green (ID 2), [2]=blue (ID 3) */
        };

        ambient_led: virtual-led@1 {
            reg = <1>;
            color = <LED_COLOR_ID_MULTI>;
            function = LED_FUNCTION_STATUS;
            priority = <10>;
            led-mode = "multicolor";
            leds = <&red_led>, <&green_led>, <&blue_led>;
        };
    };

RGBW LED with White Channel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: dts

    virtualcolor {
        compatible = "leds-group-virtualcolor";
        #address-cells = <1>;
        #size-cells = <0>;

        status_led: virtual-led@0 {
            reg = <0>;
            color = <LED_COLOR_ID_MULTI>;
            function = LED_FUNCTION_STATUS;
            priority = <100>;
            led-mode = "multicolor";
            leds = <&red_led>, <&green_led>, <&blue_led>, <&white_led>;
            /* Channels: [0]=white (ID 0), [1]=red, [2]=green, [3]=blue */
            /* Note: White comes FIRST because LED_COLOR_ID_WHITE = 0 */
        };
    };

Standard Mode with Fixed Color
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: dts

    virtualcolor {
        compatible = "leds-group-virtualcolor";
        #address-cells = <1>;
        #size-sizes = <0>;

        warm_white: virtual-led@0 {
            reg = <0>;
            color = <LED_COLOR_ID_MULTI>;
            function = LED_FUNCTION_STATUS;
            priority = <50>;
            led-mode = "standard";
            leds = <&red_led>, <&green_led>, <&blue_led>;
            mc-channel-multipliers = <255 180 100>;
            /* Channels: [0]=red:255, [1]=green:180, [2]=blue:100 */
            /* Creates warm white: full red, 70% green, 40% blue */
        };
    };

Sysfs Interface
===============

Each virtual LED creates a standard LED class device at::

    /sys/class/leds/<device>:<label>/

Standard LED Attributes
-----------------------

``brightness``
    RW, 0-max_brightness. Master brightness control.

    Writing brightness triggers arbitration which determines which
    virtual LED controls each physical LED based on priority and
    sequence numbers.

``max_brightness``
    RO, maximum brightness value (typically 255 for multicolor mode).

``trigger``
    RW, LED trigger name (standard LED class).

Multicolor Attributes (mc/ subdirectory)
-----------------------------------------

``mc/multi_intensity``
    RW (multicolor mode), RO (standard mode)

    Space-separated intensity values (0-255), one per channel.
    Order matches the channel order shown in multi_index.

    Example usage::

        # Verify channel order first
        $ cat /sys/class/leds/status:multi/mc/multi_index
        1 2 3
        # Shows [0]=red (ID 1), [1]=green (ID 2), [2]=blue (ID 3)

        # Set RGB to 255, 128, 0 (orange)
        $ echo "255 128 0" > /sys/class/leds/status:multi/mc/multi_intensity

``mc/multi_index``
    RO, space-separated LED_COLOR_ID values.
    Shows the color ID for each channel position.

``mc/multi_multipliers``
    RO, space-separated multiplier values (0-255).
    Shows the scale factors applied to each channel.

Module Parameters
=================

``enable_debugfs`` (bool, default: y if CONFIG_DEBUG_FS)
    Enable debugfs diagnostics interface at /sys/kernel/debug/leds-group-virtualcolor-*/

``use_gamma_correction`` (bool, default: n)
    Apply perceptual gamma correction (2.2) to brightness values.

``update_delay_us`` (uint, default: 0)
    Artificial delay in microseconds between physical LED updates.
    Useful for rate-limiting slow buses (I2C). Max: 1000000 (1 second)

``max_phys_leds`` (uint, default: 64)
    Maximum number of unique physical LEDs per controller. Increase if you see
    "Update buffer overflow" errors. Range: 1-1024

``enable_update_batching`` (bool, default: n)
    Enable update batching/debouncing with 10ms delay.
    Reduces overhead for applications doing rapid brightness changes.

Debugfs Interface
=================

When ``enable_debugfs=true`` and ``CONFIG_DEBUG_FS=y``, diagnostics are available at::

    /sys/kernel/debug/leds-group-virtualcolor-<device>/

Available Files
---------------

``stats``
    RO, controller-level statistics:

    * Arbitration cycle count
    * LED update count
    * Last update timestamp
    * Error counters (allocation failures, buffer overflows, rate limits)
    * Arbitration latency (min/max/average in nanoseconds)
    * Configuration (gamma, batching, delays, max_phys_leds)
    * Global sequence counter

``vled_stats``
    RO, per-virtual-LED statistics:

    * Brightness set count
    * Intensity update count
    * Blink requests count
    * Sequence number
    * Arbitration participation/win/loss counts
    * Win rate percentage
    * Error counters (buffer failures, parse errors, rate limit drops)

``phys_led_states``
    RO, current state of each physical LED:

    * LED name
    * Chosen brightness
    * Controlling priority
    * Sequence number
    * Winner virtual LED name

``claimed_leds``
    RO, total count of globally claimed physical LEDs across all controllers.

``selftest``
    RO, driver self-test output showing configuration and status.

``stress_test``
    WO, trigger stress test: ``echo <iterations> > stress_test``

    Runs randomized brightness and intensity updates to test
    arbitration under load. Max 10000 iterations.

``rebuild``
    WO, trigger physical LED list rebuild: ``echo 1 > rebuild``

    Forces re-discovery of all physical LEDs. Useful for debugging.

Example Usage
=============

Basic Control
-------------

.. code-block:: bash

    # Set notification LED to red at full brightness
    echo 255 > /sys/class/leds/platform:notification/brightness
    echo "255 0 0" > /sys/class/leds/platform:notification/mc/multi_intensity

    # Set ambient LED to blue at 50% brightness
    echo 128 > /sys/class/leds/platform:ambient/brightness
    echo "0 0 255" > /sys/class/leds/platform:ambient/mc/multi_intensity

Verifying Channel Order
------------------------

.. code-block:: bash

    # Check which color is which channel
    $ cat /sys/class/leds/status:multi/mc/multi_index
    0 1 2 3
    # This means: [0]=white (ID 0), [1]=red (ID 1), [2]=green (ID 2), [3]=blue (ID 3)

    # Set intensities in that order: white=100, red=255, green=0, blue=0
    $ echo "100 255 0 0" > /sys/class/leds/status:multi/mc/multi_intensity

Priority Arbitration Example
-----------------------------

.. code-block:: bash

    # Both virtual LEDs reference the same physical red LED
    # notification has priority=100, ambient has priority=10

    # Turn on ambient (low priority)
    echo 255 > /sys/class/leds/platform:ambient/brightness
    echo "255 0 0" > /sys/class/leds/platform:ambient/mc/multi_intensity
    # Red LED is on at 255

    # Turn on notification (high priority) - it TAKES OVER
    echo 200 > /sys/class/leds/platform:notification/brightness
    echo "200 0 0" > /sys/class/leds/platform:notification/mc/multi_intensity
    # Red LED changes to 200 (notification wins, ambient ignored)

    # Turn off notification
    echo 0 > /sys/class/leds/platform:notification/brightness
    # Red LED returns to 255 (ambient regains control)

Monitoring with Debugfs
------------------------

.. code-block:: bash

    # Watch arbitration statistics
    watch -n 1 cat /sys/kernel/debug/leds-group-virtualcolor-platform/stats

    # Check which vLED is winning
    cat /sys/kernel/debug/leds-group-virtualcolor-platform/phys_led_states

    # Monitor per-LED stats
    cat /sys/kernel/debug/leds-group-virtualcolor-platform/vled_stats

Performance Tuning
==================

Buffer Sizing
-------------

If you see "Update buffer overflow" errors in dmesg:

.. code-block:: bash

    # Count unique physical LEDs referenced in your device tree
    # Add 25% headroom and set max_phys_leds

    sudo rmmod leds_group_virtualcolor
    sudo modprobe leds_group_virtualcolor max_phys_leds=80

Rate Limiting
-------------

For I2C LED controllers on slow buses:

.. code-block:: bash

    # Add 500µs delay between updates
    sudo modprobe leds_group_virtualcolor update_delay_us=500

    # Or enable update batching (10ms debounce)
    sudo modprobe leds_group_virtualcolor enable_update_batching=1

Gamma Correction
----------------

For LEDs used in human-visible applications:

.. code-block:: bash

    # Enable perceptual brightness correction (gamma 2.2)
    sudo modprobe leds_group_virtualcolor use_gamma_correction=1

Troubleshooting
===============

Common Issues
-------------

**"Update buffer overflow" errors**
    Physical LED count exceeds max_phys_leds. Increase the module parameter
    to match or exceed the number of unique physical LEDs in your device tree.

**"Physical LED already claimed" warnings**
    Another virtualcolor controller instance is already managing this LED.
    Each physical LED can only be claimed by one controller instance.

**Intensity updates rejected in standard mode**
    This is expected behavior. Standard mode locks color to the multipliers
    defined in device tree. Switch to multicolor mode if dynamic color
    control is needed.

**LEDs flicker rapidly during software PWM**
    Enable update batching: ``enable_update_batching=1``

**Slow LED response on I2C**
    Add update delay: ``update_delay_us=500`` (adjust based on bus speed)

**Linear brightness doesn't match perception**
    Enable gamma correction: ``use_gamma_correction=1``

**Wrong color appears for RGBW LED**
    Check channel order with multi_index. White (ID=0) comes before RGB (IDs 1-3).

    Example::

        $ cat /sys/class/leds/myled/mc/multi_index
        0 1 2 3
        # Order is: white, red, green, blue

        $ echo "0 255 0 0" > /sys/class/leds/myled/mc/multi_intensity
        # This sets: white=0, red=255, green=0, blue=0

Debug Logging
-------------

Enable driver debug messages:

.. code-block:: bash

    # Dynamic debug (if enabled in kernel)
    echo "module leds_group_virtualcolor +p" > /sys/kernel/debug/dynamic_debug/control

    # Check kernel messages
    dmesg | grep virtualcolor

Known Limitations
=================

* Physical LEDs cannot be shared between multiple virtualcolor controller
  instances (enforced via global ownership tracking with global_owner_xa)

* Maximum 1024 unique physical LEDs per controller (configurable via
  ``max_phys_leds`` parameter)

* Rate limiting on intensity changes may drop updates under extreme load
  (>100 updates/second per vLED by default)

* Update batching introduces 10ms latency (disable if real-time response
  is critical)

* Debugfs statistics are only available when CONFIG_DEBUG_FS is enabled
  (struct sizes reduced by ~30% when disabled)

Testing
=======

The driver includes built-in self-tests accessible via debugfs:

.. code-block:: bash

    # Run self-test
    cat /sys/kernel/debug/leds-group-virtualcolor-platform/selftest

    # Run stress test (1000 iterations)
    echo 1000 > /sys/kernel/debug/leds-group-virtualcolor-platform/stress_test

    # Watch dmesg for results
    dmesg | tail -20

Development and Debugging
=========================

Adding Instrumentation
----------------------

The driver uses conditional compilation for debug features. To add custom
telemetry:

.. code-block:: c

    #ifdef CONFIG_DEBUG_FS
    vled->my_custom_counter++;
    #endif

This ensures zero overhead in production builds when CONFIG_DEBUG_FS is
disabled.

Memory Layout
-------------

Key structures with memory optimization:

* ``struct virtual_led``: ~200 bytes (non-debug), ~300 bytes (debug)
* ``struct phys_led_entry``: ~120 bytes (non-debug), ~220 bytes (debug)
* ``struct vcolor_controller``: ~400 bytes (non-debug), ~600 bytes (debug)

Contributing
============

Patches should be sent to the LED subsystem maintainers:

* Pavel Machek <pavel@ucw.cz>
* Lee Jones <lee@kernel.org>
* linux-leds@vger.kernel.org

Ensure patches:

1. Follow kernel coding style (checkpatch.pl --strict)
2. Include appropriate Signed-off-by tags
3. Update documentation if ABI/behavior changes
4. Test with CONFIG_DEBUG_FS=y and =n
5. Run sparse/smatch static analysis
6. Test on actual hardware with multiple virtual LED configurations

References
==========

* LED subsystem documentation: Documentation/leds/
* Multicolor LED class: Documentation/leds/leds-class-multicolor.rst
* Device tree bindings: Documentation/devicetree/bindings/leds/
* LED device tree common properties: include/dt-bindings/leds/common.h

License
=======

This driver is licensed under GPL v2. See COPYING for details.
