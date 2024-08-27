#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code for an RPC program's procedures"""

from jinja2 import Environment

from generators import SourceGenerator, create_jinja2_environment
from xdr_ast import _RpcProgram, _RpcVersion, excluded_apis


def emit_version_declarations(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit declarations for each RPC version's procedures"""
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
    """Emit server argument decoders for each RPC version's procedures"""
    arguments = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            arguments.add(procedure.argument.type_name)

    template = environment.get_template("decoder/argument.j2")
    for argument in arguments:
        print(template.render(program=program, argument=argument))


def emit_version_result_decoders(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit client result decoders for each RPC version's procedures"""
    results = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            results.add(procedure.result.type_name)

    template = environment.get_template("decoder/result.j2")
    for result in results:
        print(template.render(program=program, result=result))


def emit_version_argument_encoders(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit client argument encoders for each RPC version's procedures"""
    arguments = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            arguments.add(procedure.argument.type_name)

    template = environment.get_template("encoder/argument.j2")
    for argument in arguments:
        print(template.render(program=program, argument=argument))


def emit_version_result_encoders(
    environment: Environment, program: str, version: _RpcVersion
) -> None:
    """Emit server result encoders for each RPC version's procedures"""
    results = set()
    for procedure in version.procedures:
        if procedure.name not in excluded_apis:
            results.add(procedure.result.type_name)

    template = environment.get_template("encoder/result.j2")
    for result in results:
        print(template.render(program=program, result=result))


class XdrProgramGenerator(SourceGenerator):
    """Generate source code for an RPC program's procedures"""

    def __init__(self, language: str, peer: str):
        """Initialize an instance of this class"""
        self.environment = create_jinja2_environment(language, "program")
        self.peer = peer

    def emit_declaration(self, node: _RpcProgram) -> None:
        """Emit a declaration pair for each of an RPC programs's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")

        for version in node.versions:
            emit_version_declarations(self.environment, program, version)

    def emit_decoder(self, node: _RpcProgram) -> None:
        """Emit all decoder functions for an RPC program's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")
        match self.peer:
            case "server":
                for version in node.versions:
                    emit_version_argument_decoders(
                        self.environment, program, version,
                    )
            case "client":
                for version in node.versions:
                    emit_version_result_decoders(
                        self.environment, program, version,
                    )

    def emit_encoder(self, node: _RpcProgram) -> None:
        """Emit all encoder functions for an RPC program's procedures"""
        raw_name = node.name
        program = raw_name.lower().removesuffix("_program").removesuffix("_prog")
        match self.peer:
            case "server":
                for version in node.versions:
                    emit_version_result_encoders(
                        self.environment, program, version,
                    )
            case "client":
                for version in node.versions:
                    emit_version_argument_encoders(
                        self.environment, program, version,
                    )
