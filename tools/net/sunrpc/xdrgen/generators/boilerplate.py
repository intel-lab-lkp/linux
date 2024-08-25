#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate boilerplate items"""

import sys
import os.path
import time
from jinja2 import Environment, FileSystemLoader

from xdr_ast import _XdrAst, _RpcProgram, get_header_name


def find_xdr_program_name(root: _XdrAst) -> str:
    """Retrieve the RPC program name from an abstract syntax tree"""
    raw_name = get_header_name()
    if raw_name != "none":
        return raw_name.lower()
    for definition in root.definitions:
        if isinstance(definition.value, _RpcProgram):
            raw_name = definition.value.name
            return raw_name.lower().removesuffix("_program").removesuffix("_prog")
    return "noprog"


class SourceGenerator:
    """Generate source code boilerplate"""

    def __init__(self, language: str) -> None:
        """Set the output language"""
        match language:
            case "C":
                self.environment = Environment(
                    loader=FileSystemLoader(sys.path[0] + "/templates/C/boilerplate/"),
                    trim_blocks=True,
                    lstrip_blocks=True,
                )
            case _:
                raise NotImplementedError("Language not supported")

    def emit_header_bottom(self, root: _XdrAst) -> None:
        """Emit the bottom header guard"""
        name = find_xdr_program_name(root)
        template = self.environment.get_template("header_bottom.j2")
        print(template.render(program=name))

    def emit_header_top(self, filename: str, root: _XdrAst) -> None:
        """Emit the top header guard"""
        name = find_xdr_program_name(root)
        template = self.environment.get_template("header_top.j2")
        print(
            template.render(
                program=name,
                mtime=time.ctime(os.path.getmtime(filename)),
            )
        )

    def emit_source_top(self, filename: str, root: _XdrAst) -> None:
        """Emit the top source boilerplate"""
        name = find_xdr_program_name(root)
        template = self.environment.get_template("source_top.j2")
        print(
            template.render(
                program=name,
                mtime=time.ctime(os.path.getmtime(filename)),
            )
        )
