#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code to handle XDR enum types"""

import sys
from jinja2 import Environment, FileSystemLoader

from xdr_ast import public_apis, _XdrEnum
from xdr_parse import get_xdr_annotate


class SourceGenerator:
    """Generate source code for XDR enum types"""

    def __init__(self, language: str) -> None:
        """Set the output language"""
        match language:
            case "C":
                self.environment = Environment(
                    loader=FileSystemLoader(sys.path[0] + "/templates/C/enum/"),
                    trim_blocks=True,
                    lstrip_blocks=True,
                )
                self.environment.globals["annotate"] = get_xdr_annotate()
                self.environment.globals["public_apis"] = public_apis
            case _:
                raise NotImplementedError("Language not supported")

    def emit_declaration(self, node: _XdrEnum) -> None:
        """Emit one declaration for an enum type"""

        template = self.environment.get_template("declaration/open.j2")
        print(template.render(name=node.name))

        template = self.environment.get_template("declaration/enumerator.j2")
        for enumerator in node.enumerators:
            print(
                template.render(
                    name=enumerator.name,
                    value=enumerator.value,
                )
            )

        template = self.environment.get_template("declaration/close.j2")
        print(template.render(public_apis=public_apis, name=node.name))

    def emit_decoder(self, node: _XdrEnum) -> None:
        """Emit one decoder function for an enum type"""

        template = self.environment.get_template("decoder/enum.j2")
        print(template.render(name=node.name))

    def emit_encoder(self, node: _XdrEnum) -> None:
        """Emit one encoder function for an enum type"""

        template = self.environment.get_template("encoder/enum.j2")
        print(template.render(name=node.name))
