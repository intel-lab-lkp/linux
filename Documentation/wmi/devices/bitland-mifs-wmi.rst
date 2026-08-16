.. SPDX-License-Identifier: GPL-2.0-or-later

========================================
Bitland MIFS driver (bitland-mifs-wmi)
========================================

Introduction
============


EC WMI interface description
============================

The EC WMI interface description can be decoded from the embedded binary MOF (bmof)
data using the `bmfdec <https://github.com/pali/bmfdec>`_ utility:

::

  class WMIEvent : __ExtrinsicEvent {
  };

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x40A"), Description("Root WMI HID_EVENT20"), guid("{46c93e13-ee9b-4262-8488-563bca757fef}")]
  class HID_EVENT20 : WmiEvent {
    [key, read] string InstanceName;
    [read] boolean Active;
    [WmiDataId(1), read, write, Description("Package Data")] uint8 EventDetail[8];
  };

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x40A"), Description("Root WMI HID_EVENT21"), guid("{fa78e245-2c0f-4ca1-91cf-15f34e474850}")]
  class HID_EVENT21 : WmiEvent {
    [key, read] string InstanceName;
    [read] boolean Active;
    [WmiDataId(1), read, write, Description("Package Data")] uint8 EventDetail[8];
  };

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x40A"), Description("Root WMI HID_EVENT22"), guid("{1dceaf0a-4d63-44bb-bd0c-0d6281bfddc5}")]
  class HID_EVENT22 : WmiEvent {
    [key, read] string InstanceName;
    [read] boolean Active;
    [WmiDataId(1), read, write, Description("Package Data")] uint8 EventDetail[8];
  };

  [WMI, Dynamic, Provider("WmiProv"), Locale("MS\\0x40A"), Description("Root WMI HID_EVENT23"), guid("{3f9e3c26-b077-4f86-91f5-37ff64d8c7ed}")]
  class HID_EVENT23 : WmiEvent {
    [key, read] string InstanceName;
    [read] boolean Active;
    [WmiDataId(1), read, write, Description("Package Data")] uint8 EventDetail[8];
  };

  [WMI, Dynamic, provider("WmiProv"), Locale("MS\\0x409"), Description("Class used to operate firmware interface"), guid("{b60bfb48-3e5b-49e4-a0e9-8cffe1b3434b}")]
  class MICommonInterface {
    [key, read] string InstanceName;
    [read] boolean Active;

    [WmiMethodId(1), Implemented, read, write, Description("Method used to support system functions.")] void MiInterface([in, Description("WMI Interface")] uint8 InData[32], [out] uint8 OutData[30], [out] uint16 Reserved);
  };

Reverse-Engineering the EC WMI interface
========================================

The OEM software can be download from `this link <https://iknow.lenovo.com.cn/detail/429447>`_

Nothing is obfuscated, In this case, `ILSpy <https://github.com/icsharpcode/ILSpy>`_ could be helpful.

WMI Methods (MICommonInterface)
========================================

The ``MICommonInterface`` class (GUID: ``{b60bfb48-3e5b-49e4-a0e9-8cffe1b3434b}``)
is the primary control interface. It uses a 32-byte buffer for both input
(``InData``) and output (``OutData``).

Method Structure
----------------

The data packet follows a standardized format:

+----------+------------------------------------------------------------------+
| Byte     | Description                                                      |
+==========+==================================================================+
| 1        | Method Type: Get (0xFA / 250) or Set (0xFB / 251)                |
+----------+------------------------------------------------------------------+
| 3        | Command ID (Method Name)                                         |
+----------+------------------------------------------------------------------+
| 4 - 31   | Arguments (for Set) or Return Data (for Get)                     |
+----------+------------------------------------------------------------------+


Command IDs
-----------

The following Command IDs are used in the third byte of the buffer:

+----------+-----------------------+------------------------------------------+
| ID       | Name                  | Values / Description                     |
+==========+=======================+==========================================+
| 8        | SystemPerMode         | Gaming line: 0 Balance, 1 Performance,   |
|          |                       | 2 Quiet, 3 Full-speed.                   |
|          |                       | Thin-ultrabook (TM2424): 0x02 Quiet,     |
|          |                       | 0x03 Turbo, 0x04 Full-speed, 0x09 Auto,  |
|          |                       | 0x0A Eco.                                |
+----------+-----------------------+------------------------------------------+
| 9        | GPUMode               | 0: Hybrid, 1: Discrete, 2: UMA           |
+----------+-----------------------+------------------------------------------+
| 10       | KeyboardType          | 0: White, 1: Single RGB, 2: Zone RGB     |
+----------+-----------------------+------------------------------------------+
| 11       | FnLock                | 0: Off, 1: On                            |
+----------+-----------------------+------------------------------------------+
| 12       | TPLock                | 0: Unlock, 1: Lock (Touchpad)            |
+----------+-----------------------+------------------------------------------+
| 13       | CPUGPUSYSFanSpeed     | Returns 12 bytes of fan data:            |
|          |                       | Bytes 4-5: CPU Fan RPM (Little Endian)   |
|          |                       | Bytes 6-7: GPU Fan RPM (Little Endian)   |
|          |                       | Bytes 10-11: SYS Fan RPM (Little Endian) |
+----------+-----------------------+------------------------------------------+
| 16       | RGBKeyboardMode       | 0: Off, 1: Auto Cyclic, 2: Fixed,        |
|          |                       | 3: Custom                                |
+----------+-----------------------+------------------------------------------+
| 17       | RGBKeyboardColor      | Bytes 4, 5, 6: Red, Green, Blue values   |
+----------+-----------------------+------------------------------------------+
| 18       | RGBKeyboardBrightness | 0-10: Brightness Levels, 128: Auto       |
+----------+-----------------------+------------------------------------------+
| 19       | SystemAcType          | 1: Type-C, 2: Circular Hole (DC)         |
+----------+-----------------------+------------------------------------------+
| 20       | MaxFanSpeedSwitch     | Byte 4: Fan Type (0: CPU/GPU, 1: SYS)    |
|          |                       | Byte 5: State (0: Off, 1: On)            |
+----------+-----------------------+------------------------------------------+
| 21       | MaxFanSpeed           | Sets manual fan speed duty cycle         |
+----------+-----------------------+------------------------------------------+
| 22       | CPUThermometer        | Returns CPU Temperature                  |
+----------+-----------------------+------------------------------------------+

WMI Events (HID_EVENT20)
========================

The driver listens for events from the ``HID_EVENT20`` class
(GUID: ``{46c93e13-ee9b-4262-8488-563bca757fef}``). These events are triggered
by hotkeys or system state changes (e.g., plugging in AC power).

Event Structure
---------------

The event data is provided in an 8-byte array (``EventDetail``):

+----------+------------------------------------------------------------------+
| Byte     | Description                                                      |
+==========+==================================================================+
| 0        | Event Type (Always 0x01 for HotKey/Notification)                 |
+----------+------------------------------------------------------------------+
| 1        | Event ID (Corresponds to the Command IDs above)                  |
+----------+------------------------------------------------------------------+
| 2        | Value (The new state or value of the feature)                    |
+----------+------------------------------------------------------------------+

Common Event IDs:
-----------------

Note: reserved event ids are not listed there

+----------+------------------------------------------------------------------+
| Event Id | Description                                                      |
+==========+==================================================================+
| 4        | AirPlane mode change                                             |
+----------+------------------------------------------------------------------+
| 5        | Keyboard brightness change                                       |
+----------+------------------------------------------------------------------+
| 6        | Touchpad state (enabled/disabled) change                         |
+----------+------------------------------------------------------------------+
| 7        | FnLock state (enabled/disabled) change                           |
+----------+------------------------------------------------------------------+
| 8        | Keyboard mode change                                             |
+----------+------------------------------------------------------------------+
| 9        | CapsLock state change                                            |
+----------+------------------------------------------------------------------+
| 13       | NumLock state change                                             |
+----------+------------------------------------------------------------------+
| 14       | ScrollLock state change                                          |
+----------+------------------------------------------------------------------+
| 15       | Performance plan change                                          |
+----------+------------------------------------------------------------------+
| 25       | Display refresh rate change                                      |
+----------+------------------------------------------------------------------+
| 33       | Super key lock state (enabled/disabled) change                   |
+----------+------------------------------------------------------------------+
| 35       | Open control center key                                          |
+----------+------------------------------------------------------------------+

Implementation Details
======================

Performance Modes
-----------------
Changing the performance mode via Command ID 0x08 (SystemPerMode) affects the
power limits (PL1/PL2) and fan curves managed by the Embedded Controller (EC).

Two firmware encodings exist:

* Gaming line (Tongfang/Redmi G): sequential values 0..3. Full-speed and
  Performance typically require a DC barrel jack; the driver returns
  ``-EOPNOTSUPP`` otherwise.
* Thin-ultrabook line (Xiaomi Book Pro 14, DMI ``board_name=TM2424``):
  Quiet=0x02, Turbo=0x03, Full-speed=0x04, Auto=0x09, Eco=0x0A. These
  machines charge over USB-C only. Full-speed is still rejected by firmware
  on battery (status byte != 0x80); the driver then falls back to Turbo.

The firmware status byte is ``OutData[1]`` (0x80 accepted / supported).

Graphics Switching
------------------
The ``GPUMode`` (0x09) allows switching between Hybrid (Muxless) and Discrete
(Muxed) graphics. Changing this value usually requires a system reboot to
take effect in the BIOS/Firmware.

Fan Control
-----------
The system supports both automatic EC control and manual overrides. Command ID
0x14 (``MaxFanSpeedSwitch``) is used to toggle manual control, while ID 0x15
sets the actual PWM duty cycle.
