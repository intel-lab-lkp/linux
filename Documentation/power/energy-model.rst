.. SPDX-License-Identifier: GPL-2.0

=======================
Energy Model of devices
=======================

1. Overview
-----------

The Energy Model (EM) framework serves as an interface between drivers knowing
the power consumed by devices at various performance levels, and the kernel
subsystems willing to use that information to make energy-aware decisions.

The source of the information about the power consumed by devices can vary greatly
from one platform to another. These power costs can be estimated using
devicetree data in some cases. In others, the firmware will know better.
Alternatively, userspace might be best positioned. And so on. In order to avoid
each and every client subsystem to re-implement support for each and every
possible source of information on its own, the EM framework intervenes as an
abstraction layer which standardizes the format of power cost tables in the
kernel, hence enabling to avoid redundant work.

The power values might be expressed in micro-Watts or in an 'abstract scale'.
Multiple subsystems might use the EM and it is up to the system integrator to
check that the requirements for the power value scale types are met. An example
can be found in the Energy-Aware Scheduler documentation
Documentation/scheduler/sched-energy.rst. For some subsystems like thermal or
powercap power values expressed in an 'abstract scale' might cause issues.
These subsystems are more interested in estimation of power used in the past,
thus the real micro-Watts might be needed. An example of these requirements can
be found in the Intelligent Power Allocation in
Documentation/driver-api/thermal/power_allocator.rst.
Kernel subsystems might implement automatic detection to check whether EM
registered devices have inconsistent scale (based on EM internal flag).
Important thing to keep in mind is that when the power values are expressed in
an 'abstract scale' deriving real energy in micro-Joules would not be possible.

The figure below depicts an example of drivers (Arm-specific here, but the
approach is applicable to any architecture) providing power costs to the EM
framework, and interested clients reading the data from it::

       +---------------+  +-----------------+  +---------------+
       | Thermal (IPA) |  | Scheduler (EAS) |  |     Other     |
       +---------------+  +-----------------+  +---------------+
               |                   | em_cpu_energy()   |
               |                   | em_cpu_get()      |
               +---------+         |         +---------+
                         |         |         |
                         v         v         v
                        +---------------------+
                        |    Energy Model     |
                        |     Framework       |
                        +---------------------+
                           ^       ^       ^
                           |       |       | em_dev_register_perf_domain()
                +----------+       |       +---------+
                |                  |                 |
        +---------------+  +---------------+  +--------------+
        |  cpufreq-dt   |  |   arm_scmi    |  |    Other     |
        +---------------+  +---------------+  +--------------+
                ^                  ^                 ^
                |                  |                 |
        +--------------+   +---------------+  +--------------+
        | Device Tree  |   |   Firmware    |  |      ?       |
        +--------------+   +---------------+  +--------------+

In case of CPU devices the EM framework manages power cost tables per
'performance domain' in the system. A performance domain is a group of CPUs
whose performance is scaled together. Performance domains generally have a
1-to-1 mapping with CPUFreq policies. All CPUs in a performance domain are
required to have the same micro-architecture. CPUs in different performance
domains can have different micro-architectures.


2. Design
-----------------

2.1 Basic EM
^^^^^^^^^^^^

The basic EM is built around constant power information for each performance
state, which is accessible in: 'dev->em_pd->default_table->state'. This model
can be derived based on power measurements of the device e.g. CPU while
running some benchmark. The benchmark might be integer heavy or floating point
computation with a data set fitting into the CPU cache or registers. Bare in
mind that this model might not cover all possible workloads running on CPUs.
Thus, please run a few different benchmarks and verify with some real
workloads your power model values. The power variation due to the workload
instruction mix and data set is not modeled. The static power, which can
change during runtime due to variation of SOC temperature, is not modeled
either.

2.2 Runtime modifiable EM
^^^^^^^^^^^^^^^^^^^^^^^^^

To better reflect power variation due to static power (leakage) the EM
supports runtime modifications of the power values. The mechanism relies on
RCU to free the modifiable EM perf_state table memory. Its user, the task
scheduler, also uses RCU to access this memory. The EM framework is
responsible for allocating the new memory for the modifiable EM perf_state
table. The old memory is freed automatically using RCU callback mechanism.
This design decision is made based on task scheduler using that data and
to prevent wrong usage of kernel modules if they would be responsible for the
memory management.

There are two structures with the performance state tables in the EM:
a) dev->em_pd->default_table
b) dev->em_pd->runtime_table
They both point to the same memory location via:
'em_perf_table::state' pointer, until the first modification of the values
This should save memory on platforms which would never modify the EM. When
the first modification is made the 'default_table' (a) contains the old
EM which was created during the setup. The modified EM is available in the
'runtime_table' (b).

Only EAS uses the 'runtime_table' and benefits from the updates to the
EM values. Other sub-systems (thermal, powercap) use the 'default_table' (a).

The kernel code which want to modify the EM values is protected from concurrent
access using a mutex. Therefore, the code must use sleeping context when
they want to modify the EM.

With the runtime modifiable EM we switch from a 'single and during the entire
runtime static EM' (system property) design to a 'single EM which can be
changed during runtime according e.g. to the workload' (system and workload
property) design.

It is possible also to modify the CPU performance values for each EM's
performance state. Thus, the full power and performance profile (which
is an exponential curve) can be changed according e.g. to the workload
or system property.


3. Core APIs
------------

3.1 Config options
^^^^^^^^^^^^^^^^^^

CONFIG_ENERGY_MODEL must be enabled to use the EM framework.


3.2 Registration of performance domains
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Registration of 'advanced' EM
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The 'advanced' EM gets its name due to the fact that the driver is allowed
to provide more precised power model. It's not limited to some implemented math
formula in the framework (like it is in 'simple' EM case). It can better reflect
the real power measurements performed for each performance state. Thus, this
registration method should be preferred in case considering EM static power
(leakage) is important.

Drivers are expected to register performance domains into the EM framework by
calling the following API::

  int em_dev_register_perf_domain(struct device *dev, unsigned int nr_states,
		struct em_data_callback *cb, cpumask_t *cpus, bool microwatts);

Drivers must provide a callback function returning <frequency, power> tuples
for each performance state. The callback function provided by the driver is free
to fetch data from any relevant location (DT, firmware, ...), and by any mean
deemed necessary. Only for CPU devices, drivers must specify the CPUs of the
performance domains using cpumask. For other devices than CPUs the last
argument must be set to NULL.
The last argument 'microwatts' is important to set with correct value. Kernel
subsystems which use EM might rely on this flag to check if all EM devices use
the same scale. If there are different scales, these subsystems might decide
to return warning/error, stop working or panic.
See Section 4. for an example of driver implementing this
callback, or Section 3.4 for further documentation on this API

Registration of EM using DT
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The  EM can also be registered using OPP framework and information in DT
"operating-points-v2". Each OPP entry in DT can be extended with a property
"opp-microwatt" containing micro-Watts power value. This OPP DT property
allows a platform to register EM power values which are reflecting total power
(static + dynamic). These power values might be coming directly from
experiments and measurements.

Registration of 'artificial' EM
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

There is an option to provide a custom callback for drivers missing detailed
knowledge about power value for each performance state. The callback
.get_cost() is optional and provides the 'cost' values used by the EAS.
This is useful for platforms that only provide information on relative
efficiency between CPU types, where one could use the information to
create an abstract power model. But even an abstract power model can
sometimes be hard to fit in, given the input power value size restrictions.
The .get_cost() allows to provide the 'cost' values which reflect the
efficiency of the CPUs. This would allow to provide EAS information which
has different relation than what would be forced by the EM internal
formulas calculating 'cost' values. To register an EM for such platform, the
driver must set the flag 'microwatts' to 0, provide .get_power() callback
and provide .get_cost() callback. The EM framework would handle such platform
properly during registration. A flag EM_PERF_DOMAIN_ARTIFICIAL is set for such
platform. Special care should be taken by other frameworks which are using EM
to test and treat this flag properly.

Registration of 'simple' EM
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The 'simple' EM is registered using the framework helper function
cpufreq_register_em_with_opp(). It implements a power model which is tight to
math formula::

	Power = C * V^2 * f

The EM which is registered using this method might not reflect correctly the
physics of a real device, e.g. when static power (leakage) is important.


3.3 Accessing performance domains
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

There are two API functions which provide the access to the energy model:
em_cpu_get() which takes CPU id as an argument and em_pd_get() with device
pointer as an argument. It depends on the subsystem which interface it is
going to use, but in case of CPU devices both functions return the same
performance domain.

Subsystems interested in the energy model of a CPU can retrieve it using the
em_cpu_get() API. The energy model tables are allocated once upon creation of
the performance domains, and kept in memory untouched.

The energy consumed by a performance domain can be estimated using the
em_cpu_energy() API. The estimation is performed assuming that the schedutil
CPUfreq governor is in use in case of CPU device. Currently this calculation is
not provided for other type of devices.

More details about the above APIs can be found in ``<linux/energy_model.h>``
or in Section 3.5


3.4 Runtime modifications
^^^^^^^^^^^^^^^^^^^^^^^^^

Drivers willing to modify the EM at runtime should use the following API::


  int em_dev_update_perf_domain(struct device *dev,
			struct em_data_callback *cb, void *priv);

Drivers must provide a callback .update_power() returning power value for each
performance state. The callback function provided by the driver is free
to fetch data from any relevant location (DT, firmware, ...) or sensor.
The .update_power() callback is called by the EM for each performance state to
provide new power value. In the Section 4.2 there is an example driver
which shows simple implementation of this mechanism. The callback can be
declared with EM_UPDATE_CB() macro. The caller of that callback also passes
a private void pointer back to the driver which tries to update EM.
It is useful and helps to maintain the consistent context for all performance
state calls for a given EM.


3.5 Description details of this API
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
.. kernel-doc:: include/linux/energy_model.h
   :internal:

.. kernel-doc:: kernel/power/energy_model.c
   :export:


4. Examples
-----------

4.1 Example driver with EM registration
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The CPUFreq framework supports dedicated callback for registering
the EM for a given CPU(s) 'policy' object: cpufreq_driver::register_em().
That callback has to be implemented properly for a given driver,
because the framework would call it at the right time during setup.
This section provides a simple example of a CPUFreq driver registering a
performance domain in the Energy Model framework using the (fake) 'foo'
protocol. The driver implements an est_power() function to be provided to the
EM framework::

  -> drivers/cpufreq/foo_cpufreq.c

  01	static int est_power(struct device *dev, unsigned long *mW,
  02			unsigned long *KHz)
  03	{
  04		long freq, power;
  05
  06		/* Use the 'foo' protocol to ceil the frequency */
  07		freq = foo_get_freq_ceil(dev, *KHz);
  08		if (freq < 0);
  09			return freq;
  10
  11		/* Estimate the power cost for the dev at the relevant freq. */
  12		power = foo_estimate_power(dev, freq);
  13		if (power < 0);
  14			return power;
  15
  16		/* Return the values to the EM framework */
  17		*mW = power;
  18		*KHz = freq;
  19
  20		return 0;
  21	}
  22
  23	static void foo_cpufreq_register_em(struct cpufreq_policy *policy)
  24	{
  25		struct em_data_callback em_cb = EM_DATA_CB(est_power);
  26		struct device *cpu_dev;
  27		int nr_opp;
  28
  29		cpu_dev = get_cpu_device(cpumask_first(policy->cpus));
  30
  31     	/* Find the number of OPPs for this policy */
  32     	nr_opp = foo_get_nr_opp(policy);
  33
  34     	/* And register the new performance domain */
  35     	em_dev_register_perf_domain(cpu_dev, nr_opp, &em_cb, policy->cpus,
  36					    true);
  37	}
  38
  39	static struct cpufreq_driver foo_cpufreq_driver = {
  40		.register_em = foo_cpufreq_register_em,
  41	};


4.2 Example driver with EM modification
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This section provides a simple example of a thermal driver modifying the EM.
The driver implements a mod_power_perf() function to be provided to the
EM framework. The driver is woken up periodically to check the temperature
and modify the EM data if needed::

  -> drivers/thermal/foo_thermal.c

  01	static int mod_power_perf(struct device *dev, unsigned long freq,
  02			unsigned long *power, unsigned long *perf, void *priv)
  03	{
  04		struct foo_context *ctx = priv;
  05
  06		*perf = foo_estimate_performance(dev, freq);
  07		*power = foo_estimate_power(dev, freq, ctx->temperature);
  08		if (*power >= EM_MAX_POWER);
  09			return -EINVAL;
  10
  11		return 0;
  12	}
  13
  14	/*
  15	 * Function called periodically to check the temperature and
  16	 * update the EM if needed
  17	 */
  18	static void foo_thermal_em_update(struct foo_context *ctx)
  19	{
  20		struct em_data_callback em_cb = EM_UPDATE_CB(mod_power);
  21		struct cpufreq_policy *policy = ctx->policy;
  22		struct device *cpu_dev;
  23
  24		cpu_dev = get_cpu_device(cpumask_first(policy->cpus));
  25
  26		ctx->temperature = foo_get_temp(cpu_dev, ctx);
  27		if (ctx->temperature < FOO_EM_UPDATE_TEMP_THRESHOLD)
  28			return;
  29
  30		/* Update EM for the CPUs' performance domain */
  31		ret = em_dev_update_perf_domain(cpu_dev, &em_cb, ctx);
  32		if (ret)
  33			pr_warn("foo_thermal: EM update failed\n");
  34	}
