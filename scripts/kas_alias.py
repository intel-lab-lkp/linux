#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2023 Red Hat, Inc. Alessandro Carminati <alessandro.carminati@gmail.com>
#
# kas_alias: Adds alias to duplicate symbols for the kallsyms output.

import os
import re
import argparse
import subprocess
from enum import Enum
from collections import namedtuple

# Regex representing symbols that needs no alias
regex_filter = [
        "^__compound_literal\\.[0-9]+$",
        "^__[wm]*key\\.[0-9]+$",
        "^_*TRACE_SYSTEM.*$",
        "^__already_done\\.[0-9]+$",
        "^__msg\\.[0-9]+$",
        "^__func__\\.[0-9]+$",
        "^CSWTCH\\.[0-9]+$",
        "^_rs\\.[0-9]+$",
        "^___tp_str\\.[0-9]+$",
        "^__flags\\.[0-9]+$",
        "^___done\\.[0-9]+$",
        "^__print_once\\.[0-9]+$",
        "^___once_key\\.[0-9]+$",
        "^__pfx_.*$",
        "^__cfi_.*$",
        "^\\.LC[0-9]+$",
        "^\\.L[0-9]+.[0-9]+$",
        "^__UNIQUE_ID_.*$",
        "^symbols\\.[0-9]+$",
        "^_note_[0-9]+$"
        ]

class DebugLevel(Enum):
    PRODUCTION = 0
    INFO = 1
    DEBUG_BASIC = 2
    DEBUG_MODULES = 3
    DEBUG_ALL = 4

class SeparatorType:
    def __call__(self, separator):
        if len(separator) != 1:
            raise argparse.ArgumentTypeError("Separator must be a single character")
        return separator

class Addr2LineError(Exception):
    pass

debug = DebugLevel.PRODUCTION

Line = namedtuple('Line', ['address', 'type', 'name'])

def parse_nm_lines(lines, name_occurrences=None):
    """
    Parses a given nm output and returns the symbol list along with a hash of
    symbol occurrences.
    Args:
      lines: List of tuples representing one nm line.
      name_occurrences: Hash having the name as key, used to count names'
                        occurrences.
    Returns:
      Creates a new line list proper for the nm output it parsed and, updates
      the occurrences hash.
  """
    if debug >= DebugLevel.DEBUG_BASIC.value:
       print("parse_nm_lines: parse start")

    if name_occurrences is None:
        name_occurrences = {}

    symbol_list = []

    for line in lines:
        fields = line.strip().split()

        if len(fields) >= 3:
            address, type, name = fields[0], fields[1], ' '.join(fields[2:])
            symbol_list.append(Line(address, type, name))
            name_occurrences[name] = name_occurrences.get(name, 0) + 1

    return symbol_list, name_occurrences

def start_addr2line_process(binary_file, addr2line_file):
    """
    Initializes an addr2line server process operating on the given ELF object.
    Args:
      binary_file: String representing the binary file name object of addr2line
                   queries.
      addr2line_file: String representing the addr2line executable name.
    Returns:
      Returns addr2line process descriptor.
    """
    if debug >= DebugLevel.DEBUG_BASIC.value:
       print(f"start_addr2line_process: Starting addr2line process on {binary_file}")

    try:
        addr2line_process = subprocess.Popen([addr2line_file, '-fe',
                                             binary_file],
                                             stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE,
                                             text=True)
        return addr2line_process
    except Exception as e:
         raise SystemExit(f"Fatal: Can't start addr2line resolver: {e}")


def addr2line_fetch_address(addr2line_process, address):
    """
    Queries a specific address using the active addr2line process.
    Args:
      addr2line_process: Descriptor of the addr2line process that is wanted to
                         handle the query.
      address: The address of the symbol that needs to be resolved.
    Returns:
      Returns a string representing the file and line number where the symbol
      at the specified address has been defined. The address is normalized
      before being returned.
  """
    if debug >= DebugLevel.DEBUG_ALL.value:
       print(f"addr2line_fetch_address: Resolving {address}")

    try:
        addr2line_process.stdin.write(address + '\n')
        addr2line_process.stdin.flush()
        addr2line_process.stdout.readline().strip()
        output = addr2line_process.stdout.readline().strip()

        return os.path.normpath(output)
    except Exception as e:
        raise SystemExit(
                         "Fatal: Error communicating with"
                         f" the addr2line resolver: {e}."
                        )

def process_line(line, process_data_sym, init_section_info):
    """
    Determines whether a duplicate item requires an alias or not.
    Args:
      line: nm line object that needs to be checked.
      process_data_sym: Flag indicating that the script requires to produce alias
                        also for data symbols.
    Returns:
      Returns true if the line needs to be processed, false otherwise.
    """
    if debug >= DebugLevel.DEBUG_ALL.value:
       print(f"process_line: Processing {line.address} {line.type} {line.name}")

    # The module contains symbols that were discarded after being loaded. Typically,
    # these symbols belong to the initialization function. These symbols have their
    # address in the init section addresses, so this check prevents these symbols
    # from being assigned aliases.
    if init_section_info != None:
        if (int(line.address, 16) >= init_section_info["address"] and
          int(line.address, 16) <= init_section_info["address"] + init_section_info["size"]):
            if debug >= DebugLevel.DEBUG_ALL.value:
                print(f"process_line: Skip {line.name} since its address is .init.text")
            return False

    if process_data_sym:
        return not (any(re.match(regex, line.name) for regex in regex_filter))
    else:
        return (line.type in {"T", "t"}) and (
                not (any(re.match(regex, line.name) for regex in regex_filter)))

def fetch_file_lines(filename):
    """
    Reads a text file and retrieves its content.
    Args:
      filename: String representing the name of the file that needs to be read.
    Returns:
      Returns a string list representing the lines read in the file.
    """
    if debug >= DebugLevel.DEBUG_BASIC.value:
       print(f"fetch_file_lines: Fetch {filename}")

    try:
        with open(filename, 'r') as file:
            lines = [line.strip() for line in file.readlines()]
        return lines
    except FileNotFoundError:
        raise SystemExit(f"Fatal: File not found: {filename}")

def do_nm(filename, nm_executable):
    """
    Runs the nm command on a specified file.
    Args:
      filename: String representing the name of the file on which nm should
      run against.
      nm_executable: String representing the nm executable filename.
    Returns:
      Returns a strings list representing the nm output.
    """
    # Later, during processing, objcopy cannot modify files in place when
    # adding new alias symbols. It requires a source file and a destination
    # file.
    # After this operation, there is an object file ".o" with the aliases and
    # a ".k{0,1}o.orig" file, which is the old intended object and serves as the
    # source for objcopy.
    # In a fresh build, the state is just fine.
    # However, in a second build without clean, an issue arises.
    # The ".k{0,1}o" file already contain the alias, and reprocessing it, do
    # corrupt the final result. To address this, do_nm must check if the file
    # ".k{0,1}o.orig" already exists.
    # If it does, that's the target for nm and must be renamed in ".k{0,1}o"
    # to restore the intended state. If not, it's a fresh build, and nm can
    # proceed with the ".k{0,1}o" file.
    backup_file = filename + '.orig'
    if os.path.exists(backup_file):
        print(f"do_nm: {filename} is not clean, restore {backup_file} to {filename}")
        os.rename(backup_file, filename)

    if debug >= DebugLevel.DEBUG_BASIC.value:
       print(f"do_nm: executing {nm_executable} -n {filename}")

    try:
        nm_output = subprocess.check_output([nm_executable, '-n', filename],
                      universal_newlines=True, stderr=subprocess.STDOUT).splitlines()
        return nm_output
    except subprocess.CalledProcessError as e:
        raise SystemExit(f"Fatal: Error executing nm: {e}")

def make_objcpy_arg(line, decoration, elf_section_names):
    """
    Produces an objcopy argument statement for a single alias to be added in a
    module.
    Args:
      line: nm line object target for this iteration.
      decoration: String representing the decoration (normalized addr2line
                  output) to be added at the symbol name to have the alias.
      elf_section_names: List of the section names that can be used by objcopy
                         to add a symbol to the ELF symbol table.
    Returns:
      Returns a string that directly maps the argument string objcopy should
      use to add the alias.
    """
    try:
        section = (
            elf_section_names[".text"] if line.type.upper() == "T" else (
                elf_section_names[".data"] if line.type.upper() == "D" else (
                    elf_section_names[".rodata"] if line.type.upper() == "R" else ".bss"
                )
            )
        )
        flag = "global" if line.type.isupper() else "local"

        if debug >= DebugLevel.DEBUG_MODULES.value:
           print("make_objcpy_arg: "
                 f"{line.name + decoration}={section}:0x{line.address},{flag}")


        return (
                "--add-symbol "
                f"{line.name + decoration}={section}:0x{line.address},{flag} "
               )
    except Exception:
        print(
              f"make_objcpy_arg warning: Skip alias for {line.name}"
              f" type {line.type} because no corresponding section found.")
        return ""

def execute_objcopy(objcopy_executable, objcopy_args, object_file):
    """
    Uses objcopy to add aliases to a given module object file.
    Since objcopy can't operate in place, the original object file is renamed
    before operating on it. At function end, a new object file having the old
    object's name is carrying the aliases for the duplicate symbols.
    Args:
      objcopy_executable: String representing the object copy executable file.
      objcopy_args: Arguments (aliases to add to the object file) to be used
                    in the objcopy execution command line.
      object_file: Target object file (module object file) against which objcopy is executed.
    Returns:
      Nothing is returned, but as a side effect of this function execution,
      the module's object file contains the aliases for duplicated symbols.
    """
    # Rename the original object file by adding a suffix
    backup_file = object_file + '.orig'
    if debug >= DebugLevel.DEBUG_MODULES.value:
       print("execute_objcopy: "
             f"rename {object_file} to {backup_file}")
    os.rename(object_file, backup_file)

    full_command = (
                    f"{objcopy_executable} "
                    f"{objcopy_args} {backup_file} {object_file}"
                   )
    if debug >= DebugLevel.DEBUG_MODULES.value:
       print(f"execute_objcopy: executing {full_command}")

    try:
        subprocess.run(full_command, shell=True, check=True)
    except subprocess.CalledProcessError as e:
        os.rename(backup_file, object_file)
        raise SystemExit(f"Fatal: Error executing objcopy: {e}")

def generate_decoration(line, config, addr2line_process):
    """
    Generates symbol decoration to be used to make the alias name, by
    querying addr2line.
    Args:
      line: nm line object that needs an alias.
      config: Object containing command line configuration.
      addr2line_process: Descriptor of the addr2line process that serves
                         the binary object where the symbol belongs.
    Returns:
      Returns a string representing the decoration for the given symbol,
      or empty string if this can not be done. E.g., addr2line can't find
      the point where the symbol is defined.
    """
    output = addr2line_fetch_address(addr2line_process, line.address)
    decoration = config.separator + "".join(
        "_" if not c.isalnum() else c for c in output.replace(config.linux_base_dir, "")
    )
    # The addr2line can emit the special string "?:??" when addr2line can not find the
    # specified address in the DWARF section that after normalization it becomes "____".
    # In such cases, emitting an alias wouldn't make sense, so it is skipped.
    if decoration != config.separator + "____":
       return decoration
    return ""

def get_objdump_text(objdump_executable, file_to_operate):
    """
    objdump output is needed for a couple of functions revolving around
    modules. This function kust query objdump to emit sections info and
    return its output.
    Args:
      objdump_executable: String representing the objdump executable.
      file_to_operate: file whose section names are wanted.
    Returns:
      Returns objdump output.
    """
    try:
        output = subprocess.check_output(
                   [objdump_executable, '-h', file_to_operate],
                   universal_newlines=True)

    except Exception as e:
        raise SystemExit(
                         "Fatal: Can't find section names"
                         f" for {file_to_operate}. Error: {e}"
                        )
    return output

def get_init_text_info(objdump_lines):
    """
    Recovers info on the .init.text section.
    Args:
      objdump_lines: output from objdump -h command.
    Returns:
      Returns a map containing the size and address of the .init.text section
      None if it is not there.
    """
    section_info = None
    section_name_pattern = re.compile(r'^\s*\d+')

    for line in objdump_lines.strip().splitlines():
        if section_name_pattern.match(line):
            parts = line.split()
            if len(parts) >= 2:
                current_section_name = parts[1]
                if current_section_name == ".init.text":
                    size = int(parts[2], 16)
                    address = int(parts[3], 16)
                    section_info = {"size": size, "address": address}
                    break

    return section_info

def get_section_names(objdump_lines):
    """
    objcopy needs to refer to a section name to assign the symbol type.
    Unfortunately, not always all the section are present into a given
    object file exist, for example, ".rodata" can not exist, and a [Rr]
    symbol my refer to some other section e.g., ".rodata.str1".
    For this reason this function tries to recover the exact names to use
    in an objcopy statement.
    Args:
      objdump_lines: output from objdump -h command.
    Returns:
      Returns a map containing four string indexed with typical section
      names.
    """
    section_names = []
    lines = objdump_lines.strip().splitlines()
    section_name_pattern = re.compile(r'^\s*\d+')
    for line in lines:
        if section_name_pattern.match(line):
            parts = line.split()
            if len(parts) >= 2:
                section_name = parts[1]
                section_names.append(section_name)

    best_matches = [".text", ".rodata", ".data", ".bss"]
    result = {}

    for match in best_matches:
        for section_name in section_names:
            if re.match(match+".*", section_name):
                result[match] = section_name

    if debug >= DebugLevel.DEBUG_MODULES.value:
        for key, value in result.items():
            print(f"get_section_names: sections {key} = {value}")

    return result

def produce_output_modules(config, symbol_list, name_occurrences,
                           module_file_name, addr2line_process):
    """
    Computes the alias addition on a given module object file.
    Args:
      config: Object containing command line configuration.
      symbol_list: List of tuples representing nm lines for the given object
                   file.
      name_occurrences: Hash that stores symbol occurreces for the build.
      module_file_name: String representing the target moule object file.
      addr2line_process: Descriptor of the addr2line process that is wanted to
                         handle the query.
    Returns:
      Nothing is returned, but as a side effect of this function execution,
      the module's object file contains the aliases for duplicated symbols.
    """
    objcopy_args = "";
    args_cnt = 0
    objdump_data = get_objdump_text(config.objdump_file, module_file_name)
    elf_section_names = get_section_names(objdump_data)
    init_text_section_data = get_init_text_info(objdump_data)
    for obj in symbol_list:
        if (name_occurrences[obj.name] > 1) and process_line(obj, config.process_data_sym, init_text_section_data):
            decoration = generate_decoration(obj, config, addr2line_process)
            if decoration != "":
                objcopy_args = objcopy_args + make_objcpy_arg(obj, decoration, elf_section_names)
                args_cnt = args_cnt + 1
                if args_cnt > 50:
                   if debug >= DebugLevel.DEBUG_MODULES.value:
                      print("Produce_output_modules: Number of arguments high, split objcopy call into multiple statements.")
                   execute_objcopy(config.objcopy_file, objcopy_args, module_file_name)
                   args_cnt = 0
                   objcopy_args = ""

    execute_objcopy(config.objcopy_file, objcopy_args, module_file_name)

def produce_output_vmlinux(config, symbol_list, name_occurrences, addr2line_process):
    """
    Computes the alias addition for the core Linux on image.
    Args:
      config: Object containing command line configuration.
      symbol_list: List of tuples representing nm lines for the given object
                   file.
      name_occurrences: Hash that stores symbol occurreces for the build.
      addr2line_process: Descriptor of the addr2line process that is wanted to
                         handle the query.
    Returns:
      Nothing is returned, but as a side effect of this function execution,
      the core kernel image contains the aliases for duplicated symbols.
    """
    with open(config.output_file, 'w') as output_file:
        for obj in symbol_list:
            output_file.write(f"{obj.address} {obj.type} {obj.name}\n")
            if (name_occurrences[obj.name] > 1) and process_line(obj, config.process_data_sym, None):
                decoration = generate_decoration(obj, config, addr2line_process)
                if decoration != "":
                    output_file.write(f"{obj.address} {obj.type} {obj.name + decoration}\n")

if __name__ == "__main__":
    # Handles command-line arguments and generates a config object
    parser = argparse.ArgumentParser(description='Add alias to multiple occurring symbols name in kallsyms')
    parser.add_argument('-a', "--addr2line", dest="addr2line_file", required=True, help="Set the addr2line executable to be used.")
    parser.add_argument('-b', "--basedir", dest="linux_base_dir", required=True, help="Set base directory of the source kernel code.")
    parser.add_argument('-c', "--objcopy", dest="objcopy_file", required=True, help="Set the objcopy executable to be used.")
    parser.add_argument('-d', "--process_data", dest="process_data_sym", required=False, help="Requires the tool to process data symbols along with text symbols.", action='store_true')
    parser.add_argument('-e', "--nm", dest="nm_file", required=True, help="Set the nm executable to be used.")
    parser.add_argument('-m', "--modules_list", dest="module_list", required=True, help="Set the file containing the list of the modules object files.")
    parser.add_argument('-n', "--nmdata", dest="nm_data_file", required=True, help="Set vmlinux nm output file to use for core image.")
    parser.add_argument('-o', "--outfile", dest="output_file", required=True, help="Set the vmlinux nm output file containing aliases.")
    parser.add_argument('-s', "--separator", dest="separator", required=False, help="Set separator, character that separates original name from the addr2line data in alias symbols.", default="@", type=SeparatorType())
    parser.add_argument('-u', "--objdump", dest="objdump_file", required=True, help="Set objdump  executable to be used.")
    parser.add_argument('-v', "--vmlinux", dest="vmlinux_file", required=True, help="Set the vmlinux core image file.")
    parser.add_argument('-z', "--debug", dest="debug", required=False, help="Set the debug level.", choices=[f"{level.value}" for level in DebugLevel], default="1" )
    config = parser.parse_args()
    debug = int(config.debug)

    try:
        if debug >= DebugLevel.INFO.value:
            print("kas_alias: Start processing")

        # Determine kernel source code base directory
        config.linux_base_dir = os.path.normpath(os.getcwd() + "/" + config.linux_base_dir) + "/"

        if debug >= DebugLevel.INFO.value:
            print("kas_alias: Process nm data from vmlinux")

        # Process nm data from vmlinux
        vmlinux_nm_lines = fetch_file_lines(config.nm_data_file)
        vmlinux_symbol_list, name_occurrences = parse_nm_lines(vmlinux_nm_lines)

        if debug >= DebugLevel.INFO.value:
            print("kas_alias: Process nm data for modules")

        # Process nm data for modules
        module_list = fetch_file_lines(config.module_list)
        module_symbol_list = {}
        for module in module_list:
            module_nm_lines = do_nm(module, config.nm_file)
            module_symbol_list[module], name_occurrences = parse_nm_lines(module_nm_lines, name_occurrences)

        if debug >= DebugLevel.INFO.value:
            print("kas_alias: Produce file for vmlinux")

        # Produce file for vmlinux
        addr2line_process = start_addr2line_process(config.vmlinux_file, config.addr2line_file)
        produce_output_vmlinux(config, vmlinux_symbol_list, name_occurrences, addr2line_process)
        addr2line_process.stdin.close()
        addr2line_process.stdout.close()
        addr2line_process.stderr.close()
        addr2line_process.wait()

        # link-vmlinux.sh calls this two times: Avoid running kas_alias twice for efficiency and prevent duplicate aliases
        # in module processing by checking the last letter of the nm data file
        if config.vmlinux_file and config.vmlinux_file[-1] == '2':
            if debug >= DebugLevel.INFO.value:
                print("kas_alias: Add aliases to module files")

            # Add aliases to module files
            for module in module_list:
                addr2line_process = start_addr2line_process(module, config.addr2line_file)
                produce_output_modules(config, module_symbol_list[module], name_occurrences, module, addr2line_process)
                addr2line_process.stdin.close()
                addr2line_process.stdout.close()
                addr2line_process.stderr.close()
                addr2line_process.wait()
        else:
            if debug >= DebugLevel.INFO.value:
                print("kas_alias: Skip module processing if pass is not the second")


    except Exception as e:
        raise SystemExit(f"Script terminated due to an error: {e}")
