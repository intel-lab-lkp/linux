.. SPDX-License-Identifier: GPL-2.0

======================================
CoreSight System Configuration Manager
======================================

    :Author:   Mike Leach <mike.leach@linaro.org>
    :Date:     October 2020

Introduction
============

The CoreSight System Configuration manager is an API that allows the
programming of the CoreSight system with pre-defined configurations that
can then be easily enabled from sysfs or perf.

Many CoreSight components can be programmed in complex ways - especially ETMs.
In addition, components can interact across the CoreSight system, often via
the cross trigger components such as CTI and CTM. These system settings can
be defined and enabled as named configurations.


Basic Concepts
==============

This section introduces the basic concepts of a CoreSight system configuration.


Features
--------

A feature is a named set of programming for a CoreSight device. The programming
is device dependent, and can be defined in terms of absolute register values,
resource usage and parameter values.

The feature is defined using a descriptor. This descriptor is used to load onto
a matching device, either when the feature is loaded into the system, or when the
CoreSight device is registered with the configuration manager.

The load process involves interpreting the descriptor into a set of register
accesses in the driver - the resource usage and parameter descriptions
translated into appropriate register accesses. This interpretation makes it easy
and efficient for the feature to be programmed onto the device when required.

The feature will not be active on the device until the feature is enabled, and
the device itself is enabled. When the device is enabled then enabled features
will be programmed into the device hardware.

A feature is enabled as part of a configuration being enabled on the system.


Parameter Value
~~~~~~~~~~~~~~~

A parameter value is a named value that may be set by the user prior to the
feature being enabled that can adjust the behaviour of the operation programmed
by the feature.

For example, this could be a count value in a programmed operation that repeats
at a given rate. When the feature is enabled then the current value of the
parameter is used in programming the device.

The feature descriptor defines a default value for a parameter, which is used
if the user does not supply a new value.

Users can update parameter values using the configfs API for the CoreSight
system - which is described below.

The current value of the parameter is loaded into the device when the feature
is enabled on that device.


Configurations
--------------

A configuration defines a set of features that are to be used in a trace
session where the configuration is selected. For any trace session only one
configuration may be selected.

The features defined may be on any type of device that is registered
to support system configuration. A configuration may select features to be
enabled on a class of devices - i.e. any ETMv4, or specific devices, e.g. a
specific CTI on the system.

As with the feature, a descriptor is used to define the configuration.
This will define the features that must be enabled as part of the configuration
as well as any preset values that can be used to override default parameter
values.


Preset Values
~~~~~~~~~~~~~

Preset values are easily selectable sets of parameter values for the features
that the configuration uses. The number of values in a single preset set, equals
the sum of parameter values in the features used by the configuration.

e.g. a configuration consists of 3 features, one has 2 parameters, one has
a single parameter, and another has no parameters. A single preset set will
therefore have 3 values.

Presets are optionally defined by the configuration, up to 15 can be defined.
If no preset is selected, then the parameter values defined in the feature
are used as normal.


Operation
~~~~~~~~~

The following steps take place in the operation of a configuration.

1) In this example, the configuration is ``autofdo``, which has an
   associated feature ``strobing`` that works on ETMv4 CoreSight Devices.

2) The configuration is enabled. For example ``perf`` may select the
   configuration as part of its command line::

    perf record -e cs_etm/autofdo/ myapp

   which will enable the ``autofdo`` configuration.

3) perf starts tracing on the system. As each ETMv4 that perf uses for
   trace is enabled,  the configuration manager will check if the ETMv4
   has a feature that relates to the currently active configuration.
   In this case ``strobing`` is enabled & programmed into the ETMv4.

4) When the ETMv4 is disabled, any registers marked as needing to be
   saved will be read back.

5) At the end of the perf session, the configuration will be disabled.


Viewing Configurations and Features
===================================

The set of configurations and features that are currently loaded into the
system can be viewed using the configfs API.

Mount configfs as normal and the ``cs-syscfg`` subsystem will appear::

    $ ls /config
    cs-syscfg  stp-policy

This has two sub-directories, with the load and unload attribute files::

    $ cd cs-syscfg/
    $ ls
    configurations features load  unload

The system has the configuration ``autofdo`` built in. It may be examined as
follows::

    $ cd configurations/
    $ ls
    autofdo
    $ cd autofdo/
    $ ls
    description  feature_refs  preset1  preset3  preset5  preset7  preset9
    enable       preset        preset2  preset4  preset6  preset8
    $ cat description
    Setup ETMs with strobing for autofdo
    $ cat feature_refs
    strobing

Each preset declared has a ``preset<n>`` subdirectory declared. The values for
the preset can be examined::

    $ cat preset1/values
    strobing.window = 0x1388 strobing.period = 0x2
    $ cat preset2/values
    strobing.window = 0x1388 strobing.period = 0x4

The ``enable`` and ``preset`` files allow the control of a configuration when
using CoreSight with sysfs.

The features referenced by the configuration can be examined in the features
directory::

    $ cd ../../features/strobing/
    $ ls
    description  matches  nr_params  params
    $ cat description
    Generate periodic trace capture windows.
    parameter 'window': a number of CPU cycles (W)
    parameter 'period': trace enabled for W cycles every period x W cycles
    $ cat matches
    SRC_ETMV4
    $ cat nr_params
    2

Move to the params directory to examine and adjust parameters::

    cd params
    $ ls
    period  window
    $ cd period
    $ ls
    value
    $ cat value
    0x2710
    # echo 15000 > value
    # cat value
    0x3a98

Parameters adjusted in this way are reflected in all device instances that have
loaded the feature.


Using Configurations in perf
============================

The configurations loaded into the CoreSight configuration management are
also declared in the perf ``cs_etm`` event infrastructure so that they can
be selected when running trace under perf::

    $ ls /sys/devices/cs_etm
    cpu0  cpu2  events  nr_addr_filters		power  subsystem  uevent
    cpu1  cpu3  format  perf_event_mux_interval_ms	sinks  type

The key directory here is ``events`` - a generic perf directory which allows
selection on the perf command line. As with the sinks entries, this provides
a hash of the configuration name.

The entry in the ``events`` directory uses perfs built in syntax generator
to substitute the syntax for the name when evaluating the command::

    $ ls events/
    autofdo
    $ cat events/autofdo
    configid=0xa7c3dddd

The ``autofdo`` configuration may be selected on the perf command line::

    $ perf record -e cs_etm/autofdo/u --per-thread <application>

A preset to override the current parameter values can also be selected::

    $ perf record -e cs_etm/autofdo,preset=1/u --per-thread <application>

When configurations are selected in this way, then the trace sink used is
automatically selected.

Using Configurations in sysfs
=============================

Coresight can be controlled using sysfs. When this is in use then a configuration
can be made active for the devices that are used in the sysfs session.

In a configuration there are ``enable`` and ``preset`` files.

To enable a configuration for use with sysfs::

    $ cd configurations/autofdo
    $ echo 1 > enable

This will then use any default parameter values in the features - which can be
adjusted as described above.

To use a ``preset<n>`` set of parameter values::

    $ echo 3 > preset

This will select preset3 for the configuration.
The valid values for preset are 0 - to deselect presets, and any value of
<n> where a ``preset<n>`` sub-directory is present.

Note that the active sysfs configuration is a global parameter, therefore
only a single configuration can be active for sysfs at any one time.
Attempting to enable a second configuration will result in an error.
Additionally, attempting to disable the configuration while in use will
also result in an error.

The use of the active configuration by sysfs is independent of the configuration
used in perf.


Creating and Loading Custom Configurations
==========================================

Custom configurations and / or features can be dynamically loaded into the
system by using a loadable module, or by loading a configuration table
through in configfs.

Loaded configurations can use previously loaded features. The system will
ensure that it is not possible to unload a feature that is currently in
use, by enforcing the unload order as the strict reverse of the load order.


Using a Loadable Module
-----------------------

A new configuration or set of features can be created using the internal
structures used in the drivers, by writing a loadable module that defines
the configuration, and loading this into the kernel at runtime.

Creating a custom configuration in this way requires the user to compile the
module for the specific kernel in use, which limits portability.

Module Example
~~~~~~~~~~~~~~

An example of a custom configuration module is found in ``./samples/coresight``.

This creates a new configuration that uses the existing built in
strobing feature, but provides a different set of presets.

When the module is loaded, then the configuration appears in the configfs
file system and is selectable in the same way as the built in configuration
described above.

The file ``coresight-cfg-sample.c`` contains the configuration and module
initialisation code needed to create the loadable module.

This will be built alongside the kernel modules if selected in KConfig.
(select ``CONFIG_SAMPLE_CORESIGHT_SYSCFG``).


Using a Configuration Table File
--------------------------------

Configurations and features can also be dynamically loaded at runtime
using a compact binary table format described below.

Upon load, this table is validated, expanded into the internal structures
needed for load into the CoreSight subsystem, and loaded into the relevant
CoreSight devices.

There are additional attributes in the cs-syscfg directory - ``load_table``
and ``unload_last_table`` that can be used to load and unload configuration
tables.

As described above, in order to respect configuration dependencies, unload
order is scrictly enforced to be the reverse of load order.

Load and unload cannot be done if trace is in progress using a configuration.

To load, 'cat' the table file into the load attribute::

    $ ls /config/cs-syscfg
    configurations features  load_table  show_last_load  unload_last_table
    $ cat example1.cscfg > /config/cs-syscfg/load_table
    $ ls /config/cs-syscfg/configurations/
    autofdo  autofdo3

``unload_last_table`` can be used to unload the last loaded configuration,
but only if this was loaded as a configuration table.

To unload, write a non-zero value to ``unload_last_table``. This will unload
the last loaded table - unless another configuration or feature has been
loaded as a loadable module after the last table load::

    $ echo 1 > /config/cs-syscfg/unload_last_table
    ls /config/cs-syscfg/configurations/
    autofdo

The type of the last loaded configuration can be determined by reading the
``show_last_load`` attribute::

    $ cat /config/cs-syscfg/show_last_load
    load name: autofdo3
    load type: Runtime Dynamic table load
    (configurations:  autofdo3 )
    (features:  None )

This shows the key elements of the loaded configuration - the type of load,
load name, and any configurations and features loaded by the table.

Unload will fail if the last loaded item was not a dynamic loaded table.
When using ``show_last_load`` a non table load will show::

    cat /config/cs-syscfg/show_last_load
    load name: [Not Set]
    load type: Loadable module


Generation tools and table examples
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``./tools/coresight`` directory contains example programs to generate and
read and print binary configuration table files.

Building the tools creates the ``coresight-cfg-file-gen`` program that will
generate a configuration binary ``example1.cscfg`` that can be loaded into the
system using configfs. The configuration declared in the source file
``coresight-cfg-example1.c`` is named ``autofdo3`` - the name that will be used
once loaded.

The source files ``coresight-cfg-bufw.h`` and ``coresight-cfg-bufw.c`` provide a
standard function to convert a configuration declared in 'C' into the correct
binary buffer format. These files can be re-used to create new custom
configurations. Alternatively, additional examples can be added to the
``coresight-cfg-file-gen`` program::

    $ ./coresight-cfg-file-gen
    Coresight Configuration file Generator

    Generating example1 example
    Generating example2 example

The program ``coresight-cfg-file-read`` can read back and print a configuration
binary. This is built using the file reader from the driver code
(``coresight-config-file.c``), which is copied over into ``./tools/coresight`` at
build time.::

    ./coresight-cfg-file-read example1.cscfg
    CoreSight Configuration file reader
    ============================================

    Configuration 1
    Name:- autofdo3
    Description:-
    Setup ETMs with strobing for autofdo
    Supplied presets allow experimentation with mark-space ratio for various loads

    Uses 1 features:-
    Feature-1: strobing

    Provides 4 sets of preset values, 2 presets per set
    set[0]: 0x7d0, 0x64,
    set[1]: 0x7d0, 0x3e8,
    set[2]: 0x7d0, 0x1388,
    set[3]: 0x7d0, 0x2710,

    ============================================
    File contains no features


CoreSight Configuration Table Format
------------------------------------

The file format is defined in the source file ``coresight-config-table.h``

The source reader and generator examples use/produce a table in this format,
as a binary file.

This arrangement is reproduced below:-

Overall Table structure
~~~~~~~~~~~~~~~~~~~~~~~

::

   [cscfg_table_header]  // Mandatory
   [CONFIG_ELEM]*        // Optional - multiple, defined by cscfg_table_header.nr_configs
   [FEATURE_ELEM]*       // Optional - multiple, defined by cscfg_table_header.nr_features

Table is invalid if both [CONFIG_ELEM] and [FEATURE_ELEM] are omitted.

A table that contains only [FEATURE_ELEM] may be loaded, and the features used
by subsequently loaded files with [CONFIG_ELEM] elements.

Element Name Strings
~~~~~~~~~~~~~~~~~~~~

Configuration name strings are required to consist of alphanumeric characters and '_' only. Other special characters are not permitted.

For example ``my_config_2`` is a valid name, while ``this-bad-config#5`` will not work.

This is in order to comply with the requirements of the perf command line.

It is recommended that Feature and Parameter names use the same convention to allow for future enhancements to the command line syntax.

CONFIG_ELEM element
~~~~~~~~~~~~~~~~~~~

::

   [cscfg_table_elem_header]               // header length value to end of feature strings.
   [cscfg_table_elem_str]                  // name of the configuration.
                                           // (see element string name requirements)
   [cscfg_table_elem_str]                  // description of configuration.
   [u16 value](nr_presets)                 // number of defined sets presets values.
   [u32 value](nr_total_params)            // total parameters defined by all used features.
   [u16 value](nr_feat_refs)               // number of features referenced by the configuration
   [u64 values] * (nr_presets * nr_total_params)     // the preset values.
   [cscfg_table_elem_str] * (nr_feat_refs) // names of features used in the configurations.

FEATURE_ELEM element
~~~~~~~~~~~~~~~~~~~~

::

   [cscfg_table_elem_header]               // header length is total bytes to end of param structures.
   [cscfg_table_elem_str]                  // feature name.
   [cscfg_table_elem_str]                  // feature description.
   [u32 value](match_flags)                // flags to associate the feature with a device.
   [u16 value](nr_regs)                    // number of registers.
   [u16 value](nr_params)                  // number of parameters.
   [cscfg_regval_desc struct] * (nr_regs)  // register definitions
   [PARAM_ELEM] * (nr_params)              // parameters definitions

PARAM_ELEM element
~~~~~~~~~~~~~~~~~~

::

   [cscfg_table_elem_str]        // parameter name.
   [u64 value](param_value)      // initial value.

Additional definitions
~~~~~~~~~~~~~~~~~~~~~~

The following structures are defined in ``coresight-config-file.h``

 * ``struct cscfg_table_header`` : This structure contains an initial magic number, the total
   length of the table, and the number of configurations and features in the table.
 * ``struct cscfg_table_elem_header``: This defines the total length and type of a CONFIG_ELEM
   or a FEATURE_ELEM.
 * ``struct cscfg_table_elem_str``: This defines a string and its length.

The magic number in cscfg_table_header is defined as two bitfields::

   [31:8] Fixed magic number to identify table type.
   [7:0]  Current table format version.

The following defines determine the maximum overall table size and maximum individual
string size

 * ``CSCFG_TABLE_MAXSIZE``      : maximum overall table size.
 * ``CSCFG_TABLE_STR_MAXSIZE``  : maximum individual string size.

Load Dependencies
~~~~~~~~~~~~~~~~~

Files may be unloaded only in the strict reverse order of loading. This is enforced by the
configuration system.

This is to ensure that any load dependencies are maintained.

A configuration table that contains a CONFIG_ELEM that references named features "feat_A" and "feat_B" will load only if either:-

a) "feat_A" and/or "feat_B" has been loaded previously, or are present as built-in / module loaded features.
b) "feat_A" and/or "feat_B" are declared as FEAT_ELEM in the same table as the CONFIG_ELEM.

Tables that contain features or configurations with the same names as those already loaded will fail to load.
