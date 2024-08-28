#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code to handle XDR struct types"""

from jinja2 import Environment, Template

from generators import SourceGenerator, create_jinja2_environment

from xdr_ast import _XdrBasic, _XdrVariableLengthString
from xdr_ast import _XdrFixedLengthOpaque, _XdrVariableLengthOpaque
from xdr_ast import _XdrFixedLengthArray, _XdrVariableLengthArray
from xdr_ast import _XdrOptionalData, _XdrBuiltInType, _XdrStruct
from xdr_ast import _XdrDeclaration


def get_kernel_c_type(type_spec: str) -> str:
    """Return C type to be used for an XDR built-in type"""
    xdr_type_to_c_type = {
        "unsigned_hyper": "u64",
        "hyper": "s64",
        "unsigned_long": "u32",
        "long": "s32",
        "unsigned_int": "u32",
        "int": "s32",
        "bool": "bool",
    }
    if isinstance(type_spec, _XdrBuiltInType):
        return xdr_type_to_c_type[type_spec.type_name]
    return type_spec.type_name


def get_jinja_template(
    environment: Environment, template_type: str, template_name: str
) -> Template:
    """Retrieve a Jinja2 template for emitting source code"""
    return environment.get_template(template_type + "/" + template_name + ".j2")


def emit_struct_member_definition(
    environment: Environment, field: _XdrDeclaration
) -> None:
    """Emit a definition for one field in an XDR struct"""
    if isinstance(field, _XdrBasic):
        template = get_jinja_template(environment, "definition", field.template)
        print(
            template.render(
                name=field.name,
                type=get_kernel_c_type(field.spec),
                ctype=field.spec.type_decorator,
            )
        )
    elif isinstance(field, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "definition", field.template)
        print(
            template.render(
                name=field.name,
                size=field.size,
            )
        )
    elif isinstance(field, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "definition", field.template)
        print(template.render(name=field.name))
    elif isinstance(field, _XdrVariableLengthString):
        template = get_jinja_template(environment, "definition", field.template)
        print(template.render(name=field.name))
    elif isinstance(field, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "definition", field.template)
        print(
            template.render(
                name=field.name,
                type=get_kernel_c_type(field.spec),
                size=field.size,
            )
        )
    elif isinstance(field, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "definition", field.template)
        print(
            template.render(
                name=field.name,
                type=get_kernel_c_type(field.spec),
                ctype=field.spec.type_decorator,
            )
        )
    elif isinstance(field, _XdrOptionalData):
        template = get_jinja_template(environment, "definition", field.template)
        print(
            template.render(
                name=field.name,
                type=get_kernel_c_type(field.spec),
                ctype=field.spec.type_decorator,
            )
        )


def emit_struct_definition(environment: Environment, node: _XdrStruct) -> None:
    """Emit one definition for an XDR struct type"""
    template = get_jinja_template(environment, "definition", "open")
    print(template.render(name=node.name))

    for field in node.fields:
        emit_struct_member_definition(environment, field)

    template = get_jinja_template(environment, "definition", "close")
    print(template.render(name=node.name))


def emit_struct_member_decoder(
    environment: Environment, field: _XdrDeclaration
) -> None:
    """Emit a decoder for one field in an XDR struct"""
    if isinstance(field, _XdrBasic):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                ctype=field.spec.type_decorator,
            )
        )
    elif isinstance(field, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                size=field.size,
            )
        )
    elif isinstance(field, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                maxsize=field.maxsize,
            )
        )
    elif isinstance(field, _XdrVariableLengthString):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                maxsize=field.maxsize,
            )
        )
    elif isinstance(field, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                size=field.size,
                ctype=field.spec.type_decorator,
            )
        )
    elif isinstance(field, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                maxsize=field.maxsize,
                ctype=field.spec.type_decorator,
            )
        )
    elif isinstance(field, _XdrOptionalData):
        template = get_jinja_template(environment, "decoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                ctype=field.spec.type_decorator,
            )
        )


def emit_struct_decoder(environment: Environment, node: _XdrStruct) -> None:
    """Emit one decoder function for a struct type"""
    template = get_jinja_template(environment, "decoder", "open")
    print(template.render(name=node.name))

    for field in node.fields:
        emit_struct_member_decoder(environment, field)

    template = get_jinja_template(environment, "decoder", "close")
    print(template.render())


def emit_struct_member_encoder(
    environment: Environment, field: _XdrDeclaration
) -> None:
    """Emit an encoder for one field in an XDR struct"""
    if isinstance(field, _XdrBasic):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
            )
        )
    elif isinstance(field, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                size=field.size,
            )
        )
    elif isinstance(field, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                maxsize=field.maxsize,
            )
        )
    elif isinstance(field, _XdrVariableLengthString):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                maxsize=field.maxsize,
            )
        )
    elif isinstance(field, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                size=field.size,
            )
        )
    elif isinstance(field, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                maxsize=field.maxsize,
            )
        )
    elif isinstance(field, _XdrOptionalData):
        template = get_jinja_template(environment, "encoder", field.template)
        print(
            template.render(
                name=field.name,
                type=field.spec.type_name,
                ctype=field.spec.type_decorator,
            )
        )


def emit_struct_encoder(environment: Environment, node: _XdrStruct) -> None:
    """Emit one encoder function for a struct type"""
    template = get_jinja_template(environment, "encoder", "open")
    print(template.render(name=node.name))

    for field in node.fields:
        emit_struct_member_encoder(environment, field)

    template = get_jinja_template(environment, "encoder", "close")
    print(template.render())


class XdrStructGenerator(SourceGenerator):
    """Generate source code for XDR structs"""

    def __init__(self, language: str, peer: str):
        """Initialize an instance of this class"""
        self.environment = create_jinja2_environment(language, "struct")
        self.peer = peer

    def emit_definition(self, node: _XdrStruct) -> None:
        """Emit one definition for an XDR struct type"""
        emit_struct_definition(self.environment, node)

    def emit_decoder(self, node: _XdrStruct) -> None:
        """Emit one decoder function for an XDR struct type"""
        emit_struct_decoder(self.environment, node)

    def emit_encoder(self, node: _XdrStruct) -> None:
        """Emit one encoder function for an XDR struct type"""
        emit_struct_encoder(self.environment, node)
