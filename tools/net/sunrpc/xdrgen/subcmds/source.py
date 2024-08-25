#!/usr/bin/env python3
# ex: set filetype=python:

"""Translate an XDR specification into executable C code that
can be compiled for the Linux kernel."""

import logging

from argparse import Namespace
from lark import logger
from lark.exceptions import UnexpectedInput

from generators import boilerplate, enum, program
from generators import typedef, struct, union
from xdr_ast import transform_parse_tree, _XdrAst, _XdrEnum, _XdrTypedef
from xdr_ast import _XdrStruct, _XdrUnion, _RpcProgram
from xdr_parse import xdr_parser, set_xdr_annotate

logger.setLevel(logging.INFO)


def emit_source_decoder(node: _XdrAst, language: str) -> None:
    """Emit one XDR decoder function for a source file"""
    if isinstance(node, _XdrEnum):
        sg = enum.SourceGenerator(language)
    elif isinstance(node, _XdrTypedef):
        sg = typedef.SourceGenerator(language)
    elif isinstance(node, _XdrStruct):
        sg = struct.SourceGenerator(language)
    elif isinstance(node, _XdrUnion):
        sg = union.SourceGenerator(language)
    elif isinstance(node, _RpcProgram):
        sg = program.SourceGenerator(language)
    else:
        return
    sg.emit_decoder(node)


def emit_source_encoder(node: _XdrAst, language: str) -> None:
    """Emit one XDR encoder function for a source file"""
    if isinstance(node, _XdrEnum):
        sg = enum.SourceGenerator(language)
    elif isinstance(node, _XdrTypedef):
        sg = typedef.SourceGenerator(language)
    elif isinstance(node, _XdrStruct):
        sg = struct.SourceGenerator(language)
    elif isinstance(node, _XdrUnion):
        sg = union.SourceGenerator(language)
    elif isinstance(node, _RpcProgram):
        sg = program.SourceGenerator(language)
    else:
        return
    sg.emit_encoder(node)


def generate_source(filename: str, root: _XdrAst, language: str) -> None:
    """Generate and emit a C source file"""

    sg = boilerplate.SourceGenerator(language)
    sg.emit_source_top(filename, root)

    for definition in root.definitions:
        emit_source_decoder(definition.value, language)

    for definition in root.definitions:
        emit_source_encoder(definition.value, language)


def handle_parse_error(e: UnexpectedInput) -> bool:
    """Simple parse error reporting, no recovery attempted"""
    print(e)
    return True


def subcmd(args: Namespace) -> int:
    """Generate encoder and decoder functions"""

    set_xdr_annotate(args.annotate)
    parser = xdr_parser()
    with open(args.filename, encoding="utf-8") as f:
        parse_tree = parser.parse(f.read(), on_error=handle_parse_error)
        ast = transform_parse_tree(parse_tree)
        generate_source(args.filename, ast, args.language)

    return 0
