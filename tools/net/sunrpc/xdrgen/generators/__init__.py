# SPDX-License-Identifier: GPL-2.0

"""Define a base code generator class"""

import sys
from jinja2 import Environment, FileSystemLoader

from xdr_ast import _XdrAst, public_apis, pass_by_reference
from xdr_parse import get_xdr_annotate

def create_jinja2_environment(language: str, xdr_type: str) -> Environment:
    """Open a set of templates based on output language"""
    match language:
        case "C":
            environment = Environment(
                loader=FileSystemLoader(sys.path[0] + "/templates/C/" + xdr_type + "/"),
                trim_blocks=True,
                lstrip_blocks=True,
            )
            environment.globals["annotate"] = get_xdr_annotate()
            environment.globals["public_apis"] = public_apis
            environment.globals["pass_by_reference"] = pass_by_reference
            return environment
        case _:
            raise NotImplementedError("Language not supported")


class SourceGenerator:
    """Base class to generate source code for XDR types"""

    def __init__(self, language: str, peer: str):
        """Initialize an instance of this class"""
        raise NotImplementedError("No language support defined")

    def emit_definition(self, node: _XdrAst) -> None:
        """Emit one definition for this XDR type"""
        raise NotImplementedError("Definition generation not supported")

    def emit_declaration(self, node: _XdrAst) -> None:
        """Emit one function declaration for this XDR type"""
        raise NotImplementedError("Declaration generation not supported")

    def emit_decoder(self, node: _XdrAst) -> None:
        """Emit one decoder function for this XDR type"""
        raise NotImplementedError("Decoder generation not supported")

    def emit_encoder(self, node: _XdrAst) -> None:
        """Emit one encoder function for this XDR type"""
        raise NotImplementedError("Encoder generation not supported")
