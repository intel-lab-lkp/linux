#!/usr/bin/env python3
# ex: set filetype=python:

"""Translate an XDR specification into executable code that
can be compiled for the Linux kernel."""

import logging

from argparse import Namespace
from lark import logger
from lark.exceptions import UnexpectedInput

from generators.boilerplate import XdrBoilerplateGenerator
from generators.constant import XdrConstantGenerator
from generators.enum import XdrEnumGenerator
from generators.pointer import XdrPointerGenerator
from generators.program import XdrProgramGenerator
from generators.typedef import XdrTypedefGenerator
from generators.struct import XdrStructGenerator
from generators.union import XdrUnionGenerator

from xdr_ast import transform_parse_tree, _RpcProgram, Specification
from xdr_ast import _XdrConstant, _XdrEnum, _XdrPointer
from xdr_ast import _XdrTypedef, _XdrStruct, _XdrUnion
from xdr_parse import xdr_parser, set_xdr_annotate

logger.setLevel(logging.INFO)


def emit_header_declarations(root: Specification, language: str) -> None:
    """Emit header declarations"""
    for definition in root.definitions:
        if isinstance(definition.value, _RpcProgram):
            gen = XdrProgramGenerator(language, "server")
            gen.emit_declaration(definition.value)


def emit_header_definitions(root: Specification, language: str) -> None:
    """Emit header definitions"""
    for definition in root.definitions:
        if isinstance(definition.value, _XdrConstant):
            gen = XdrConstantGenerator(language, "server")
        elif isinstance(definition.value, _XdrEnum):
            gen = XdrEnumGenerator(language, "server")
        elif isinstance(definition.value, _XdrPointer):
            gen = XdrPointerGenerator(language, "server")
        elif isinstance(definition.value, _XdrTypedef):
            gen = XdrTypedefGenerator(language, "server")
        elif isinstance(definition.value, _XdrStruct):
            gen = XdrStructGenerator(language, "server")
        elif isinstance(definition.value, _XdrUnion):
            gen = XdrUnionGenerator(language, "server")
        else:
            continue
        gen.emit_definition(definition.value)


def handle_parse_error(e: UnexpectedInput) -> bool:
    """Simple parse error reporting, no recovery attempted"""
    print(e)
    return True


def subcmd(args: Namespace) -> int:
    """Generate definitions and declarations"""

    if not args.definitions and not args.declarations:
        print("One or both of --definitions and --declarations is needed.")
        return 0

    set_xdr_annotate(args.annotate)
    parser = xdr_parser()
    with open(args.filename, encoding="utf-8") as f:
        parse_tree = parser.parse(f.read(), on_error=handle_parse_error)
        ast = transform_parse_tree(parse_tree)

        gen = XdrBoilerplateGenerator(args.language)
        gen.emit_header_top(args.filename, ast)

        if args.definitions:
            emit_header_definitions(ast, args.language)
        if args.declarations:
            emit_header_declarations(ast, args.language)

        generator = XdrBoilerplateGenerator(args.language)
        generator.emit_header_bottom(ast)

    return 0
