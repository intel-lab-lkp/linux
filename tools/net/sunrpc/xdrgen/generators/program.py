#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code for an RPC program's procedures"""

import sys
from jinja2 import Environment, FileSystemLoader

from xdr_ast import excluded_apis, _RpcVersion


def emit_version_declarations(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit C declarations for each RPC version's procedures"""
    print("")
    template = environment.get_template("declaration/argument.j2")
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            print(
                template.render(
                    program=program,
                    argument=procedure.argument.type_name,
                )
            )

    print("")
    template = environment.get_template("declaration/result.j2")
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            print(
                template.render(
                    program=program,
                    result=procedure.result.type_name,
                )
            )


def emit_version_argument_decoders(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit C decoders for each RPC version's procedures"""
    arguments = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            arguments.add(procedure.argument.type_name)

    template = environment.get_template("decoder/argument.j2")
    for argument in arguments:
        print(template.render(program=program, argument=argument))


def emit_version_result_encoders(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit C encoders for each RPC version's procedures"""
    results = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            results.add(procedure.result.type_name)

    template = environment.get_template("encoder/result.j2")
    for result in results:
        print(template.render(program=program, result=result))


class SourceGenerator:
    """Generate source code for an RPC program's procedures"""

    def __init__(self, language) -> None:
        """Set the output language"""
        match language:
            case "C":
                self.environment = Environment(
                    loader=FileSystemLoader(sys.path[0] + "/templates/C/program/"),
                    trim_blocks=True,
                    lstrip_blocks=True,
                )
            case _:
                raise NotImplementedError("Language not supported")

    def emit_declaration(self, node) -> None:
        """Emit a declarations pair for each of an RPC programs's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")

        for version in node.versions:
            emit_version_declarations(self.environment, program, version)

    def emit_decoder(self, node) -> None:
        """Emit all decoder functions for an RPC program's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")

        for version in node.versions:
            emit_version_argument_decoders(self.environment, program, version)

    def emit_encoder(self, node) -> None:
        """Emit all encoder functions for an RPC program's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")

        for version in node.versions:
            emit_version_result_encoders(self.environment, program, version)
