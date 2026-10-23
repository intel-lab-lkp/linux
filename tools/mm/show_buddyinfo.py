#!/usr/bin/env drgn
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2026 Aaron Tomlin <atomlin@atomlin.com>

from collections.abc import Iterator

from drgn import Object, Program, get_default_prog
from drgn.helpers.linux.nodemask import for_each_online_node

DESC = """
This is a drgn-based Python script that displays /proc/buddyinfo from a
vmcore. For further information on drgn, please refer to
https://github.com/osandov/drgn.
"""


def buddyinfo():
    """Display the buddyinfo."""

    def walk_zones_in_node(prog: Program) -> Iterator[tuple[Object, Object]]:
        for node_nr in for_each_online_node(prog):
            try:
                pgdat = prog["node_data"][node_nr]
            except (KeyError, Exception):
                pgdat = prog["contig_page_data"].address_of_()
            for zone in pgdat.node_zones:
                yield pgdat, zone

    def populated_zone(zone: Object) -> bool:
        """Determine if the specified zone is populated."""
        return zone.present_pages.value_() > 0

    prog = get_default_prog()
    for pgdat, zone in walk_zones_in_node(prog):
        if populated_zone(zone):
            print(
                f"Node {pgdat.node_id.value_()}, zone "
                f"{zone.name.string_().decode('utf-8'):8s} ",
                end="",
            )
            for order in range(len(zone.free_area)):
                print(f"{zone.free_area[order].nr_free.value_():6} ", end="")
            print()


if __name__ == "__main__":
    buddyinfo()
