#!/usr/bin/env python3
# ex: set filetype=python:

"""Translate an XDR specification into executable C code that
can be compiled for the Linux kernel."""

import logging

from argparse import Namespace
from lark import logger
from lark.exceptions import UnexpectedInput

from xdr_parse import xdr_parser

logger.setLevel(logging.DEBUG)


def handle_parse_error(e: UnexpectedInput) -> bool:
    """Simple parse error reporting, no recovery attempted"""
    print(e)
    return True


def subcmd(args: Namespace) -> int:
    """Lexical and syntax check of an XDR specification"""

    parser = xdr_parser()
    with open(args.filename, encoding="utf-8") as f:
        parser.parse(f.read(), on_error=handle_parse_error)

    return 0
