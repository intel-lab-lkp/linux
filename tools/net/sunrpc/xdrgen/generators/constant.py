#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code to handle XDR constants"""

import sys
from jinja2 import Environment, FileSystemLoader

from xdr_ast import _XdrConstant

class SourceGenerator:
    """Generate source code for XDR constants"""

    def __init__(self, language: str) -> None:
        """Set the output language"""
        match language:
            case "C":
                self.environment = Environment(
                    loader=FileSystemLoader(sys.path[0] + "/templates/C/constants/"),
                    trim_blocks=True,
                    lstrip_blocks=True,
                )
            case _:
                raise NotImplementedError("Language not supported")

    def emit_declaration(self, node: _XdrConstant) -> None:
        """Emit one declaration for a constant"""
        template = self.environment.get_template("declaration.j2")
        print(template.render(name=node.name, value=node.value))

    def emit_decoder(self, node: _XdrConstant) -> None:
        """Emit one decoder for a constant"""

    def emit_encoder(self, node: _XdrConstant) -> None:
        """Emit one encoder for a constant"""
