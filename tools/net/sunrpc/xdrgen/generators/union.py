#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code to handle XDR unions"""

import sys
from jinja2 import Environment, FileSystemLoader, Template

from xdr_ast import _XdrBasic, _XdrUnion, _XdrVoid
from xdr_ast import _XdrDeclaration, _XdrCaseSpec
from xdr_ast import public_apis, pass_by_reference

from xdr_parse import get_xdr_annotate


def get_jinja_template(
    environment: Environment, template_type: str, template_name: str
) -> Template:
    """Retrieve a Jinja2 template for emitting source code"""
    return environment.get_template(template_type + "/" + template_name + ".j2")


def emit_union_switch_spec_definition(
    environment: Environment, node: _XdrDeclaration
) -> None:
    """Emit C declaration for an XDR union's discriminant"""
    assert isinstance(node, _XdrBasic)
    template = get_jinja_template(environment, "declaration", "switch_spec")
    print(
        template.render(
            name=node.name,
            type=node.spec.type_name,
            ctype=node.spec.type_decorator,
        )
    )


def emit_union_case_spec_definition(
    environment: Environment, node: _XdrDeclaration
) -> None:
    """Emit C declaration for an XDR union's case arm"""
    if isinstance(node.arm, _XdrVoid):
        return
    assert isinstance(node.arm, _XdrBasic)
    template = get_jinja_template(environment, "declaration", "case_spec")
    print(
        template.render(
            name=node.arm.name,
            type=node.arm.spec.type_name,
            ctype=node.arm.spec.type_decorator,
        )
    )


def emit_union_declaration(environment: Environment, node: _XdrUnion) -> None:
    """Emit one C union definition"""
    template = get_jinja_template(environment, "declaration", "open")
    print(template.render(name=node.name))

    emit_union_switch_spec_definition(environment, node.discriminant)

    for case in node.cases:
        emit_union_case_spec_definition(environment, case)

    if node.default is not None:
        emit_union_case_spec_definition(environment, node.default)

    template = get_jinja_template(environment, "declaration", "close")
    print(template.render(name=node.name))


def emit_union_switch_spec_decoder(
    environment: Environment, node: _XdrDeclaration
) -> None:
    """Emit C decoder for an XDR union's discriminant"""
    assert isinstance(node, _XdrBasic)
    template = get_jinja_template(environment, "decoder", "switch_spec")
    print(template.render(name=node.name, type=node.spec.type_name))


def emit_union_case_spec_decoder(environment: Environment, node: _XdrCaseSpec) -> None:
    """Emit C decoder functions for an XDR union's case arm"""

    if isinstance(node.arm, _XdrVoid):
        return

    template = get_jinja_template(environment, "decoder", "case_spec")
    for case in node.values:
        print(template.render(case=case))

    assert isinstance(node.arm, _XdrBasic)
    template = get_jinja_template(environment, "decoder", node.arm.template)
    print(
        template.render(
            name=node.arm.name,
            type=node.arm.spec.type_name,
            ctype=node.arm.spec.type_decorator,
        )
    )

    template = get_jinja_template(environment, "decoder", "break")
    print(template.render())


def emit_union_default_spec_decoder(environment: Environment, node: _XdrUnion) -> None:
    """Emit C decoder function for an XDR union's default arm"""
    default_case = node.default

    # Avoid a gcc warning about a default case with boolean discriminant
    if default_case is None and node.discriminant.spec.type_name == "bool":
        return

    template = get_jinja_template(environment, "decoder", "default_spec")
    print(template.render())

    if default_case is None or isinstance(default_case.arm, _XdrVoid):
        template = get_jinja_template(environment, "decoder", "break")
        print(template.render())
        return

    assert isinstance(default_case.arm, _XdrBasic)
    template = get_jinja_template(environment, "decoder", default_case.arm.template)
    print(
        template.render(
            name=default_case.arm.name,
            type=default_case.arm.spec.type_name,
            ctype=default_case.arm.spec.type_decorator,
        )
    )


def emit_union_decoder(environment: Environment, node: _XdrUnion) -> None:
    """Emit one C union decoder"""
    template = get_jinja_template(environment, "decoder", "open")
    print(template.render(name=node.name))

    emit_union_switch_spec_decoder(environment, node.discriminant)

    for case in node.cases:
        emit_union_case_spec_decoder(environment, case)

    emit_union_default_spec_decoder(environment, node)

    template = get_jinja_template(environment, "decoder", "close")
    print(template.render())


def emit_union_switch_spec_encoder(
    environment: Environment, node: _XdrDeclaration
) -> None:
    """Emit C encoder for an XDR union's discriminant"""
    assert isinstance(node, _XdrBasic)
    template = get_jinja_template(environment, "encoder", "switch_spec")
    print(template.render(name=node.name, type=node.spec.type_name))


def emit_union_case_spec_encoder(environment: Environment, node: _XdrCaseSpec) -> None:
    """Emit C encoder functions for an XDR union's case arm"""

    if isinstance(node.arm, _XdrVoid):
        return

    template = get_jinja_template(environment, "encoder", "case_spec")
    for case in node.values:
        print(template.render(case=case))

    assert isinstance(node.arm, _XdrBasic)
    template = get_jinja_template(environment, "encoder", node.arm.template)
    print(
        template.render(
            name=node.arm.name,
            type=node.arm.spec.type_name,
        )
    )

    template = get_jinja_template(environment, "encoder", "break")
    print(template.render())


def emit_union_default_spec_encoder(environment: Environment, node: _XdrUnion) -> None:
    """Emit C encoder function for an XDR union's default arm"""
    default_case = node.default

    # Avoid a gcc warning about a default case with boolean discriminant
    if default_case is None and node.discriminant.spec.type_name == "bool":
        return

    template = get_jinja_template(environment, "encoder", "default_spec")
    print(template.render())

    if default_case is None or isinstance(default_case.arm, _XdrVoid):
        template = get_jinja_template(environment, "encoder", "break")
        print(template.render())
        return

    assert isinstance(default_case.arm, _XdrBasic)
    template = get_jinja_template(environment, "encoder", default_case.arm.template)
    print(
        template.render(
            name=default_case.arm.name,
            type=default_case.arm.spec.type_name,
        )
    )


def emit_union_encoder(environment, node: _XdrUnion) -> None:
    """Emit one C union encoder"""
    template = get_jinja_template(environment, "encoder", "open")
    print(template.render(name=node.name))

    emit_union_switch_spec_encoder(environment, node.discriminant)

    for case in node.cases:
        emit_union_case_spec_encoder(environment, case)

    emit_union_default_spec_encoder(environment, node)

    template = get_jinja_template(environment, "encoder", "close")
    print(template.render())


class SourceGenerator:
    """Generate source code for XDR unions"""

    def __init__(self, language: str) -> None:
        """Set the output language"""
        match language:
            case "C":
                self.environment = Environment(
                    loader=FileSystemLoader(sys.path[0] + "/templates/C/union/"),
                    trim_blocks=True,
                    lstrip_blocks=True,
                )
                self.environment.globals["annotate"] = get_xdr_annotate()
                self.environment.globals["public_apis"] = public_apis
                self.environment.globals["pass_by_reference"] = pass_by_reference
            case _:
                raise NotImplementedError("Language not supported")

    def emit_declaration(self, node: _XdrUnion) -> None:
        """Emit one declaration for an XDR union"""
        emit_union_declaration(self.environment, node)

    def emit_decoder(self, node: _XdrUnion) -> None:
        """Emit one decoder function for an XDR union"""
        emit_union_decoder(self.environment, node)

    def emit_encoder(self, node: _XdrUnion) -> None:
        """Emit one encoder function for an XDR union"""
        emit_union_encoder(self.environment, node)
