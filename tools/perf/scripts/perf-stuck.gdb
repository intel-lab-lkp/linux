# SPDX-License-Identifier: GPL-2.0
#
# gdb commands for a perf that is stuck, used by perf-stuck.sh -g and usable
# directly:
#
#   gdb -p $(pgrep -x perf) -batch -x perf-stuck.gdb -ex bt
#
# PROTOTYPE: part of the perf-stuck.sh stopgap, see the note at the start of
# that script: this wants to move into a first class 'perf stuck' command,
# which would print these DIE chains by itself, without gdb.
#
# The settings are the ones that keep a batch attach from stopping to ask
# questions (debuginfod, pagination) and that make the output readable.
#
# The commands are for the DWARF type chasers in util/dwarf-aux.c, the
# functions a data type profiling 'perf report -s type' spins in when a
# debug info file has a type chain that got into a cycle:
#
#   perf-die-chain <function> <die variable> [iterations]
#   perf-die-chain-all [iterations]
#   perf-dso
#
# For each iteration of the chasing loop they print the DIE address, the
# CU it came from, its offset in the debug file, its tag and its name: a
# cycle shows up as the same handful of (addr, cu) pairs repeating, and a
# CU that changes from one iteration to the next means the chase is
# hopping between a debug file and its dwz common file.

set pagination off
set confirm off
set debuginfod enabled off
set print pretty on
set height 0
set width 0

define perf-die-chain
  if $argc < 2
    printf "usage: perf-die-chain <function> <die variable> [iterations]\n"
  else
    frame function $arg0
    if $argc == 3
      set $perf_die_chain_n = $arg2
    else
      set $perf_die_chain_n = 10
    end
    set $perf_die_chain_head = $pc
    set $perf_die_chain_i = 0
    while $perf_die_chain_i < $perf_die_chain_n
      printf "chain[%d] die=%p addr=%p cu=%p off=0x%lx tag=%d name=%s\n", $perf_die_chain_i, $arg1, $arg1->addr, $arg1->cu, ((Dwarf_Off) dwarf_dieoffset($arg1)), ((int) dwarf_tag($arg1)), ((char *) dwarf_diename($arg1))
      until *$perf_die_chain_head
      set $perf_die_chain_i = $perf_die_chain_i + 1
    end
  end
end

document perf-die-chain
Print the DIE chain being walked by a DWARF type chasing loop.
usage: perf-die-chain <function> <die variable> [iterations]
  perf-die-chain die_get_pointer_type type_die
  perf-die-chain __die_get_real_type vr_die
  perf-die-chain die_get_real_type vr_die
end

define perf-die-chain-all
  if $argc == 0
    set $perf_die_chain_n = 10
  else
    set $perf_die_chain_n = $arg0
  end
  if $_any_caller_is("die_get_pointer_type", 20)
    printf "stuck in die_get_pointer_type():\n"
    perf-die-chain die_get_pointer_type type_die $perf_die_chain_n
  else
    if $_any_caller_is("__die_get_real_type", 20)
      printf "stuck in __die_get_real_type():\n"
      perf-die-chain __die_get_real_type vr_die $perf_die_chain_n
    else
      if $_any_caller_is("die_get_real_type", 20)
        printf "stuck in die_get_real_type():\n"
        perf-die-chain die_get_real_type vr_die $perf_die_chain_n
      else
        printf "not in a DWARF type chaser, try: bt\n"
      end
    end
  end
end

document perf-die-chain-all
Find which DWARF type chaser the process is in and print the DIE chain.
usage: perf-die-chain-all [iterations]
end

define perf-dso
  if $_any_caller_is("find_data_type", 20)
    frame function find_data_type
    printf "dso=%s ip=0x%lx sym=%s\n", dloc->ms->map->dso->name, dloc->ip, dloc->ms->sym->name
  else
    printf "not in find_data_type()\n"
  end
end

document perf-dso
Print the dso, ip and symbol of the data location being resolved.
end
