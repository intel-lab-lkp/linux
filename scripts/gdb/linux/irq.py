# SPDX-License-Identifier: GPL-2.0
#
# linux/scripts/gdb/linux/interrupts.py
#
# List IRQs using irq_to_desc() backed by maple tree

import gdb

class LxIrqs(gdb.Command):
    """List active IRQs via irq_to_desc()."""

    def __init__(self):
        super(LxIrqs, self).__init__("lx-irqs", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            max_irqs = int(gdb.parse_and_eval("nr_irqs"))
        except gdb.error:
            max_irqs = 4096  # Fallback value

        print("{:<20} {:<6} {:<20} {}".format("Address", "IRQ", "Handler", "Name"))
        print("-" * 50)

        for irq in range(max_irqs):
            try:
                desc = gdb.parse_and_eval(f"irq_to_desc({irq})")
                if desc == 0:
                    continue

                ptr = desc
                desc = desc.dereference()
                action = desc["action"]
                if int(action) == 0:
                    continue

                name = action["name"]
                handler = action["handler"]

                name_str = name.string() if name else "<no name>"
                print("{:<20} {:<6} {:<20} {}".format(str(ptr), irq, str(handler), name_str))

            except gdb.error:
                continue


LxIrqs()

