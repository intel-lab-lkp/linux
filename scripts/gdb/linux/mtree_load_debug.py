import gdb

class MTreeLoad(gdb.Command):
    def __init__(self):
        super(MTreeLoad, self).__init__("mtree-load", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            args = gdb.string_to_argv(arg)
            if len(args) != 2:
                print("Usage: mtree-load <symbol> <key>")
                return

            sym_name = args[0]
            key = int(args[1])

            sym = gdb.parse_and_eval(sym_name)
            root_val = sym['ma_root']
            root_ptr_val = root_val.cast(gdb.lookup_type("void").pointer())
            root_ptr_int = int(root_ptr_val)

            print(f"[debug] Starting at root: {hex(root_ptr_int)}")

            if root_ptr_int == 0 or root_ptr_int == 0xffffffffffffffff:
                print("[error] Empty or invalid tree root.")
                return

            clean_ptr_val = root_ptr_val.cast(gdb.lookup_type("unsigned long"))
            clean_addr = int(clean_ptr_val) & ~0xf
            print(f"[debug] Untagged node ptr: {hex(clean_addr)}")

            node_ptr_val = gdb.Value(clean_addr).cast(
                gdb.lookup_type("void").pointer()).cast(
                gdb.lookup_type("struct maple_node").pointer())
            node = node_ptr_val.dereference()

            self.walk_node(node)

        except Exception as e:
            print(f"Initialization or lookup error: {e}")

    def get_slot_count(self, node):
        try:
            base = int(node.address.cast(gdb.lookup_type("unsigned long")))
            meta_end_offset = 264
            end_ptr = gdb.Value(base + meta_end_offset).cast(
                gdb.lookup_type('unsigned char').pointer())
            count = int(end_ptr.dereference())
            print(f"[debug] Extracted count from meta.end @ {hex(base + meta_end_offset)} = {count}")
            return count
        except Exception as e:
            print(f"[error] Could not determine slot count: {e}")
            raise

    def walk_node(self, node):
        count = self.get_slot_count(node)
        print(f"[debug] Brute-force scanning node with count = {count}")

        base = int(node.address.cast(gdb.lookup_type("unsigned long")))
        slot_offset = 8
        pointer_size = gdb.lookup_type("void").pointer().sizeof

        for i in range(count + 1):
            slot_addr = base + slot_offset + i * pointer_size
            try:
                val = gdb.Value(slot_addr).cast(gdb.lookup_type("unsigned long").pointer()).dereference()
                addr = int(val)
            except Exception as e:
                print(f"[error] Failed to read raw slot[{i}] at {hex(slot_addr)}: {e}")
                continue

            if addr == 0 or addr == 0xffffffffffffffff:
                print(f"[debug] Skipping null/invalid slot[{i}] = {hex(addr)}")
                continue

            clean_ptr = addr & ~0xf

            # Attempt to treat it as irq_desc
            try:
                irq_desc_ptr = gdb.Value(clean_ptr).cast(
                    gdb.lookup_type("struct irq_desc").pointer())
                _ = irq_desc_ptr.dereference()
                self.print_irq_desc(clean_ptr)
                continue
            except:
                pass

            # If not irq_desc, maybe it's another node
            try:
                subnode_ptr = gdb.Value(clean_ptr).cast(
                    gdb.lookup_type("void").pointer()).cast(
                    gdb.lookup_type("struct maple_node").pointer())
                subnode = subnode_ptr.dereference()
                print(f"[debug] Recursively walking subnode from slot[{i}] = {hex(clean_ptr)}")
                self.walk_node(subnode)
            except Exception as e:
                print(f"[debug] Slot[{i}] @ {hex(clean_ptr)} is not a known structure: {e}")

    def print_irq_desc(self, ptr):
        try:
            print(f"[debug] Attempting to print irq_desc at {hex(ptr)}")
            irq_desc_ptr = gdb.Value(ptr).cast(
                gdb.lookup_type("struct irq_desc").pointer())
            irq_desc = irq_desc_ptr.dereference()

            irq_number = int(irq_desc['irq_data']['irq'])

            try:
                chip = irq_desc['irq_data']['chip']
                chip_name = chip.dereference().type.name
            except Exception:
                chip_name = "<unavailable>"

            print("""--- IRQ Descriptor ---
  IRQ number: {}
  Chip type: {}
----------------------""".format(irq_number, chip_name))

        except Exception as e:
            print(f"[error] Could not print IRQ descriptor: {e}")

MTreeLoad()
