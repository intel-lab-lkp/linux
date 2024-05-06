.. SPDX-License-Identifier: GPL-2.0

===================================
NVIDIA Tegra Configuration Settings
===================================

Introduction
------------
NVIDIA Tegra SoCs have various I/O controllers and these controllers require
specific register configurations.

They could be due to changes in:
 - Functional mode (eg. speed)
 - Interface properties (eg. signal timings)
 - Manufacturing characteristics (eg. process/package)
 - Thermal characteristics
 - Board characteristics

Some of the configurations can be provided by device specific standard DT
properties like speed of interface in I2C, rising/falling timing etc. However,
there are more device specific configurations required to tune the interface
based on execution mode or other runtime parameters. All such configurations are
defined as 'config' settings of the device. This configures a device to operate
with the optimal settings for a particular mode to improve performance,
stability or reduce power.

These configurations are either static or dynamic:
 - Static configuration which is set once during device boot and controller
   reset
 - Dynamic configuration is applied based on a particular condition like bus
   speed, controller mode, peripheral connected to controller, SoC and platform
   characterization

Static configurations are provided as common config setting and dynamic
configurations are provided as mode/condition specific.

Background
----------
Slew rates, tap delay and other calibration parameters for an interface
controller, are measured through characterization. These values are dynamic
and requires different values for same property / field.

Use case
--------
Tegra device drivers that use these config settings include:
 - I2C uses config settings to configure setup & hold times, clock divider
   values.
 - SDMMC tuning iterations per speed and CQE values can be set with this method.

Device tree
-----------
Config settings of a controller are added under a child node
"config" of the controller's device tree node.
Further subnodes are created under config for each conditional setting.
::

  controller@xyz {
    config {
      common {
        reg-field-a = <val-a1>;
        reg-field-b = <val-b1>;
        reg-field-c = <val-c1>;
      };
      cfg1 {
        reg-field-a = <val-a2>;
        reg-field-b = <val-b2>;
        reg-field-c = <val-c2>;
      };
      cfg2 {
        reg-field-a = <val-a3>;
        reg-field-b = <val-b3>;
        reg-field-c = <val-c3>;
      };
    };
  };

:
 - "config": subnode in device node to hold configuration settings.
 - "common": static configuration that needs to be applied on controller reset.
   Register fields under 'common' node are applied during initialization
   irrespective of any condition.
 - "cfg1": conditional configuration to be applied when controller is set in
   specific functional mode. Conditional configs may override existing settings
   in 'common' or contain settings unique to the config.
 - Properties defined under config must correspond to a register field of
   device controller.
 - Properties are device specific and added to device node.

Example
-------
Ex::

  i2c@3160000 {
    config {
      common {
        nvidia,i2c-hs-sclk-high-period = <0x03>;
        nvidia,i2c-hs-sclk-low-period = <0x08>;
      };
      fast {
        nvidia,i2c-clk-divisor-fs-mode = <0x3c>;
        nvidia,i2c-sclk-high-period = <0x02>;
        nvidia,i2c-sclk-low-period = <0x02>;
        nvidia,i2c-bus-free-time = <0x02>;
        nvidia,i2c-stop-setup-time = <0x02>;
        nvidia,i2c-start-hold-time = <0x02>;
        nvidia,i2c-start-setup-time = <0x02>;
      };
      fastplus {
        nvidia,i2c-clk-divisor-fs-mode = <0x16>;
        nvidia,i2c-sclk-high-period = <0x02>;
        nvidia,i2c-sclk-low-period = <0x02>;
        nvidia,i2c-bus-free-time = <0x02>;
        nvidia,i2c-stop-setup-time = <0x02>;
        nvidia,i2c-start-hold-time = <0x02>;
        nvidia,i2c-start-setup-time = <0x02>;
      };
      standard {
        nvidia,i2c-clk-divisor-fs-mode = <0x4f>;
        nvidia,i2c-sclk-high-period = <0x07>;
        nvidia,i2c-sclk-low-period = <0x08>;
        nvidia,i2c-bus-free-time = <0x08>;
        nvidia,i2c-stop-setup-time = <0x08>;
        nvidia,i2c-start-hold-time = <0x08>;
        nvidia,i2c-start-setup-time = <0x08>;
      };
    };
  };

