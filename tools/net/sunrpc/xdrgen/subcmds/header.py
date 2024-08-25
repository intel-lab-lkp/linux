#!/usr/bin/env python3
# ex: set filetype=python:

"""Translate an XDR specification into executable C code that
can be compiled for the Linux kernel."""

import logging

from argparse import Namespace
from lark import logger
from lark.exceptions import UnexpectedInput

from generators import boilerplate, constant, enum
from generators import program, typedef, struct, union
from xdr_ast import transform_parse_tree, _XdrAst, _RpcProgram, _XdrEnum
from xdr_ast import _XdrConstant, _XdrTypedef, _XdrStruct, _XdrUnion
from xdr_parse import xdr_parser, set_xdr_annotate

logger.setLevel(logging.INFO)


def generate_header(filename: str, root: _XdrAst, language: str) -> None:
    """Generate and emit a header file"""

    sg = boilerplate.SourceGenerator(language)
    sg.emit_header_top(filename, root)

    for definition in root.definitions:
        if isinstance(definition.value, _XdrConstant):
            sg = constant.SourceGenerator(language)
        elif isinstance(definition.value, _XdrEnum):
            sg = enum.SourceGenerator(language)
        elif isinstance(definition.value, _XdrTypedef):
            sg = typedef.SourceGenerator(language)
        elif isinstance(definition.value, _XdrStruct):
            sg = struct.SourceGenerator(language)
        elif isinstance(definition.value, _XdrUnion):
            sg = union.SourceGenerator(language)
        elif isinstance(definition.value, _RpcProgram):
            sg = program.SourceGenerator(language)
        else:
            continue
        sg.emit_declaration(definition.value)

    generator = boilerplate.SourceGenerator(language)
    generator.emit_header_bottom(root)


def handle_parse_error(e: UnexpectedInput) -> bool:
    """Simple parse error reporting, no recovery attempted"""
    print(e)
    return True


def subcmd(args: Namespace) -> int:
    """Generate definitions and declarations"""

    set_xdr_annotate(args.annotate)
    parser = xdr_parser()
    with open(args.filename, encoding="utf-8") as f:
        parse_tree = parser.parse(f.read(), on_error=handle_parse_error)
        ast = transform_parse_tree(parse_tree)
        generate_header(args.filename, ast, args.language)

    return 0
