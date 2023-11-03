#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# -*- coding: utf-8; mode: python -*-

"""
    netlink-spec
    ~~~~~~~~~~~~~~~~~~~

    Implementation of the ``netlink-spec`` ReST-directive.

    :copyright:  Copyright (C) 2023  Breno Leitao <leitao@debian.org>
    :license:    GPL Version 2, June 1991 see linux/COPYING for details.

    The ``netlink-spec`` reST-directive performs extensive parsing
    specific to the Linux kernel's standard netlink specs, in an
    effort to avoid needing to heavily mark up the original YAML file.

    This code is split in three big parts:
        1) RST formatters: Use to convert a string to a RST output
        2) Parser helpers: Helper functions to parse the YAML data
        3) NetlinkSpec Directive: The actual directive class
"""

from typing import Any, Dict, List
import os.path
from docutils.parsers.rst import Directive
from docutils import statemachine
import yaml

__version__ = "1.0"
SPACE_PER_LEVEL = 4

# RST Formatters
def rst_definition(key: str, value: Any, level: int = 0) -> str:
    """Format a single rst definition"""
    return headroom(level) + key + "\n" + headroom(level + 1) + str(value)


def rst_paragraph(paragraph: str, level: int = 0) -> str:
    """Return a formatted paragraph"""
    return headroom(level) + paragraph


def headroom(level: int) -> str:
    """Return space to format"""
    return " " * (level * SPACE_PER_LEVEL)


def rst_bullet(item: str, level: int = 0) -> str:
    """Return a formatted a bullet"""
    return headroom(level) + f" - {item}"

def rst_subsubtitle(title: str) -> str:
    """Add a sub-sub-title to the document"""
    return f"{title}\n" + "~" * len(title)


def rst_fields(key: str, value: str, level: int = 0) -> str:
    """Return a RST formatted field"""
    return headroom(level) + f":{key}: {value}"


def rst_subtitle(title: str, level: int = 0) -> str:
    """Add a subtitle to the document"""
    return headroom(level) + f"\n{title}\n" + "-" * len(title)


def rst_list_inline(list_: List[str], level: int = 0) -> str:
    """Format a list using inlines"""
    return headroom(level) + "[" + ", ".join(inline(i) for i in list_) + "]"


def bold(text: str) -> str:
    """Format bold text"""
    return f"**{text}**"


def inline(text: str) -> str:
    """Format inline text"""
    return f"``{text}``"


def sanitize(text: str) -> str:
    """Remove newlines and multiple spaces"""
    # This is useful for some fields that are spread in multiple lines
    return str(text).replace("\n", "").strip()


# Parser helpers
# ==============
def parse_mcast_group(mcast_group: List[Dict[str, Any]]) -> str:
    """Parse 'multicast' group list and return a formatted string"""
    lines = []
    for group in mcast_group:
        lines.append(rst_paragraph(group["name"], 1))

    return "\n".join(lines)


def parse_do(do_dict: Dict[str, Any], level: int = 0) -> str:
    """Parse 'do' section and return a formatted string"""
    lines = []
    for key in do_dict.keys():
        lines.append(rst_bullet(bold(key), level + 1))
        lines.append(parse_do_attributes(do_dict[key], level + 1) + "\n")

    return "\n".join(lines)


def parse_do_attributes(attrs: Dict[str, Any], level: int = 0) -> str:
    """Parse 'attributes' section"""
    if "attributes" not in attrs:
        return ""
    lines = [rst_fields("attributes", rst_list_inline(attrs["attributes"]), level + 1)]

    return "\n".join(lines)


def parse_operations(operations: List[Dict[str, Any]]) -> str:
    """Parse operations block"""
    preprocessed = ["name", "doc", "title", "do", "dump"]
    lines = []

    for operation in operations:
        lines.append(rst_subsubtitle(operation["name"]))
        lines.append(rst_paragraph(operation["doc"]) + "\n")
        if "do" in operation:
            lines.append(rst_paragraph(bold("do"), 1))
            lines.append(parse_do(operation["do"], 1))
        if "dump" in operation:
            lines.append(rst_paragraph(bold("dump"), 1))
            lines.append(parse_do(operation["dump"], 1))

        for key in operation.keys():
            if key in preprocessed:
                # Skip the special fields
                continue
            lines.append(rst_fields(key, operation[key], 1))

        # New line after fields
        lines.append("\n")

    return "\n".join(lines)


def parse_entries(entries: List[Dict[str, Any]], level: int) -> str:
    """Parse a list of entries"""
    lines = []
    for entry in entries:
        if isinstance(entry, dict):
            # entries could be a list or a dictionary
            lines.append(
                rst_fields(entry.get("name"), sanitize(entry.get("doc")), level)
            )
        elif isinstance(entry, list):
            lines.append(rst_list_inline(entry, level))
        else:
            lines.append(rst_bullet(inline(sanitize(entry)), level))

    lines.append("\n")
    return "\n".join(lines)


def parse_definitions(defs: Dict[str, Any]) -> str:
    """Parse definitions section"""
    preprocessed = ["name", "entries", "members"]
    ignored = ["render-max"] # This is not printed
    lines = []

    for definition in defs:
        lines.append(rst_subsubtitle(definition["name"]))
        for k in definition.keys():
            if k in preprocessed + ignored:
                continue
            lines.append(rst_fields(k, sanitize(definition[k]), 1))

        # Field list needs to finish with a new line
        lines.append("\n")
        if "entries" in definition:
            lines.append(rst_paragraph(bold("Entries"), 1))
            lines.append(parse_entries(definition["entries"], 2))
        if "members" in definition:
            lines.append(rst_paragraph(bold("members"), 1))
            lines.append(parse_entries(definition["members"], 2))

    return "\n".join(lines)


def parse_attributes_set(entries: List[Dict[str, Any]]) -> str:
    """Parse attribute from attribute-set"""
    preprocessed = ["name", "type"]
    ignored = ["checks"]
    lines = []

    for entry in entries:
        lines.append(rst_bullet(bold(entry["name"])))
        for attr in entry["attributes"]:
            type_ = attr.get("type")
            attr_line = bold(attr["name"])
            if type_:
                # Add the attribute type in the same line
                attr_line += f" ({inline(type_)})"

            lines.append(rst_bullet(attr_line, 2))

            for k in attr.keys():
                if k in preprocessed + ignored:
                    continue
                lines.append(rst_fields(k, sanitize(attr[k]), 3))
            lines.append("\n")

    return "\n".join(lines)


def parse_yaml(obj: Dict[str, Any]) -> str:
    """Format the whole yaml into a RST string"""
    lines = []

    # This is coming from the RST
    lines.append(rst_subtitle("Summary"))
    lines.append(rst_paragraph(obj["doc"], 1))

    # Operations
    lines.append(rst_subtitle("Operations"))
    lines.append(parse_operations(obj["operations"]["list"]))

    # Multicast groups
    if "mcast-groups" in obj:
        lines.append(rst_subtitle("Multicast groups"))
        lines.append(parse_mcast_group(obj["mcast-groups"]["list"]))

    # Definitions
    lines.append(rst_subtitle("Definitions"))
    lines.append(parse_definitions(obj["definitions"]))

    # Attributes set
    lines.append(rst_subtitle("Attribute sets"))
    lines.append(parse_attributes_set(obj["attribute-sets"]))

    return "\n".join(lines)


def parse_yaml_file(filename: str, debug: bool = False) -> str:
    """Transform the yaml specified by filename into a rst-formmated string"""
    with open(filename, "r") as file:
        yaml_data = yaml.safe_load(file)
        content = parse_yaml(yaml_data)

    if debug:
        # Save the rst for inspection
        print(content, file=open(f"/tmp/{filename.split('/')[-1]}.rst", "w"))

    return content


# Main Sphinx Extension class
def setup(app):
    """Sphinx-build register function for 'netlink-spec' directive"""
    app.add_directive("netlink-spec", NetlinkSpec)
    return dict(version=__version__, parallel_read_safe=True, parallel_write_safe=True)


class NetlinkSpec(Directive):
    """NetlinkSpec (``netlink-spec``) directive class"""
    has_content = True
    # Argument is the filename to process
    required_arguments = 1

    def run(self):
        srctree = os.path.abspath(os.environ["srctree"])
        yaml_file = os.path.join(
            srctree, "Documentation/netlink/specs", self.arguments[0]
        )
        self.state.document.settings.record_dependencies.add(yaml_file)

        try:
            content = parse_yaml_file(yaml_file)
        except FileNotFoundError as exception:
            raise self.severe(str(exception))

        self.state_machine.insert_input(statemachine.string2lines(content), yaml_file)

        return []
