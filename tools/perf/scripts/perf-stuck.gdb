# SPDX-License-Identifier: GPL-2.0
#
# gdb commands for a stuck perf, used by perf-stuck.sh -g and usable directly:
#
#   gdb -p $(pgrep -x perf) -batch -x perf-stuck.gdb -ex bt
#
# PROTOTYPE: part of the perf-stuck.sh stopgap, wants to become a first
# class 'perf stuck' command printing these DIE chains without gdb.
#
# The commands are for the DWARF type chasers in util/dwarf-aux.c:
#
#   perf-die-chain <function> <die variable> [iterations]
#   perf-die-chain-all [iterations]
#   perf-dso
#
# For each iteration of the chasing loop they print the DIE address, the
# CU, its file offset, tag and name: a cycle shows as the same (addr, cu)
# pairs repeating, and a CU changing between iterations means the chase
# hops between a debug file and its dwz common file.

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
      # Pointer type DIEs have no DW_AT_name, so dwarf_diename() can
      # return NULL: printf %s of it would error out and abort this
      # batch script, handle it.
      set $perf_die_chain_name = (char *) dwarf_diename($arg1)
      printf "chain[%d] die=%p addr=%p cu=%p off=0x%lx tag=%d name=", $perf_die_chain_i, $arg1, $arg1->addr, $arg1->cu, ((Dwarf_Off) dwarf_dieoffset($arg1)), ((int) dwarf_tag($arg1))
      if $perf_die_chain_name == 0
	printf "(null)\n"
      else
	printf "%s\n", $perf_die_chain_name
      end
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
    # REFCNT_CHECKING, implied by an ASan/LSan build, wraps struct map and
    # struct dso in a proxy keeping the real object in ->orig, the dso being
    # map->orig->dso->orig there; struct symbol is not wrapped, so sym->name
    # needs no ->orig.  There is no way to ask gdb which layout it is looking
    # at, so walk each candidate until one evaluates.  Requiring gdb's Python
    # here adds nothing: $_any_caller_is() is one of its functions already.
    python
import gdb

dso = None
for expr in ("dloc->ms->map->dso->name",
             "dloc->ms->map->orig->dso->orig->name"):
    try:
        gdb.parse_and_eval(expr)
        dso = expr
        break
    except gdb.error:
        pass

if dso is None:
    print("dso=(no such member in struct map)")
else:
    gdb.execute('printf "dso=%s ip=0x%lx sym=%s\\n", ' + dso +
                ', dloc->ip, dloc->ms->sym ? dloc->ms->sym->name : "(no symbol)"')
    end
  else
    printf "not in find_data_type()\n"
  end
end

document perf-dso
Print the dso, ip and symbol of the data location being resolved.
end
