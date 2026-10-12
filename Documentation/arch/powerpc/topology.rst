.. SPDX-License-Identifier: GPL-2.0
.. _topology:


llc_override
==============

Override the scheduler-visible LLC domain on PowerPC.

Having LLC(last level cache) at MC/PKG helps to experiment with new
scheduler feature called cache aware scheduling which expects LLC to have
at least more than 1 core. Scheduler uses notion of LLC for wakeup and
load balancing decisions.

Accepted values are:

  0    Disable the override and use the platform default.
  smt  LLC will be at SMT domain.
  mc   LLC will be at MC domain, including at SMT domains.
  pkg  LLC will be at PKG domain, including at MC and SMT domains,

Writing a new value rebuilds the scheduler domains. One can check the
domain flags and look for SD_SHARE_LLC to confirm if it took effect.

Note that this doesn't change HW cache topology. It is only a debug knob
to experiment with scheduler related functionalities. So set it to sensible
values. Also note that there is no performance guarantees across the values.
