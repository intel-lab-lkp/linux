.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver pmic50x0
======================

Supported chips:

  * JEDEC PMIC50X0 (JESD301) compliant DDR5 PMICs

    JEDEC standard download:
	https://www.jedec.org/standards-documents/docs/jesd301-1a03

    Prefix: 'pmic50x0'

    Addresses scanned: ~

Author:
	Almog Ben Shaul <almogbs@amazon.com>


Description
-----------

This driver implements support for hardware monitoring of JEDEC PMIC50X0
compliant DDR5 Power Management ICs. These devices are I2C-based power
management controllers designed specifically for DDR5 memory modules.

The driver provides monitoring for:

  * Voltage measurements across 4 switch nodes (A, B, C, D)
  * Current measurements for each switch node
  * Power consumption per switch node and total power
  * PMIC die temperature
  * Comprehensive error status reporting

The PMIC50X0 specification defines a standard interface for DDR5 power
management, including telemetry and error reporting capabilities.


Usage Notes
-----------

Error monitoring is performed via a delayed work queue that polls error
registers at a configurable interval (default 1000ms). The polling interval
can be adjusted via the module parameter ``error_polling_ms``.


Hardware monitoring sysfs entries
---------------------------------

======================= ========================================================
temp1_input		PMIC die temperature in millidegrees Celsius

in0_input		Switch Node A output voltage in millivolts
in1_input		Switch Node B output voltage in millivolts
in2_input		Switch Node C output voltage in millivolts
in3_input		Switch Node D output voltage in millivolts

curr1_input		Switch Node A output current in milliamperes
curr2_input		Switch Node B output current in milliamperes
curr3_input		Switch Node C output current in milliamperes
curr4_input		Switch Node D output current in milliamperes

power1_input		Switch Node A power consumption in microwatts
power2_input		Switch Node B power consumption in microwatts
power3_input		Switch Node C power consumption in microwatts
power4_input		Switch Node D power consumption in microwatts
power5_input		Total power consumption (sum of all nodes) in microwatts
======================= ========================================================


Error Status Counters
---------------------

The driver maintains counters for various error conditions. Each counter
increments when the corresponding error condition is detected during polling.
All error attributes are read-only and return the number of times the error
has been detected since driver load or counter reset.

====================================== =========================================
err_global_log_vin_bulk_over_vol       VIN_Bulk input over-voltage error count
err_global_log_crit_temp               Critical temperature error count
err_global_log_buck_ov_or_uv           Buck converter over/under-voltage count
err_vin_bulk_input_over_vol_stat       VIN_Bulk over-voltage status count
err_vin_mgmt_input_over_vol_stat       VIN_Mgmt over-voltage status count
err_vin_bulk_input_pow_good_stat       VIN_Bulk power good status count
err_vin_mgmt_to_vin_bulk_stat          VIN_Mgmt to VIN_Bulk switchover count
err_swa_out_pow_good_stat              Switch Node A power good status count
err_swb_out_pow_good_stat              Switch Node B power good status count
err_swc_out_pow_good_stat              Switch Node C power good status count
err_swd_out_pow_good_stat              Switch Node D power good status count
err_swa_out_over_vol_stat              Switch Node A over-voltage count
err_swb_out_over_vol_stat              Switch Node B over-voltage count
err_swc_out_over_vol_stat              Switch Node C over-voltage count
err_swd_out_over_vol_stat              Switch Node D over-voltage count
err_swa_out_under_vol_lockout_stat     Switch Node A under-voltage lockout count
err_swb_out_under_vol_lockout_stat     Switch Node B under-voltage lockout count
err_swc_out_under_vol_lockout_stat     Switch Node C under-voltage lockout count
err_swd_out_under_vol_lockout_stat     Switch Node D under-voltage lockout count
err_swa_high_out_curr_consump_stat     Switch Node A high current warning count
err_swb_high_out_curr_consump_stat     Switch Node B high current warning count
err_swc_high_out_curr_consump_stat     Switch Node C high current warning count
err_swd_high_out_curr_consump_stat     Switch Node D high current warning count
err_swa_out_curr_limiter_warn_stat     Switch Node A current limiter count
err_swb_out_curr_limiter_warn_stat     Switch Node B current limiter count
err_swc_out_curr_limiter_warn_stat     Switch Node C current limiter count
err_swd_out_curr_limiter_warn_stat     Switch Node D current limiter count
err_crit_temp_shutdown_stat            Critical temperature shutdown count
err_pmic_high_temp_warn_stat           High temperature warning count
err_vout_1v_out_power_good_stat        VOUT_1.0V LDO power good status count
err_vout_1_8v_out_power_good_stat      VOUT_1.8V LDO power good status count
err_vbias_power_good_stat              VBias power good status count
====================================== =========================================
