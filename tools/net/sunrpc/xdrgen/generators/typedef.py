#!/usr/bin/env python3
# ex: set filetype=python:

"""Generate code to handle XDR typedefs"""

from jinja2 import Environment, Template

from generators import SourceGenerator, create_jinja2_environment

from xdr_ast import _XdrBasic, _XdrVariableLengthString
from xdr_ast import _XdrFixedLengthOpaque, _XdrVariableLengthOpaque
from xdr_ast import _XdrFixedLengthArray, _XdrVariableLengthArray
from xdr_ast import _XdrOptionalData, _XdrVoid, _XdrBuiltInType
from xdr_ast import _XdrDeclaration, _XdrTypedef


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


def emit_type_definition(environment: Environment, node: _XdrDeclaration) -> None:
    """Emit a definition for one XDR typedef"""
    if isinstance(node, _XdrBasic):
        template = get_jinja_template(environment, "definition", node.template)
        print(
            template.render(
                name=node.name,
                type=get_kernel_c_type(node.spec),
                ctype=node.spec.type_decorator,
            )
        )
    elif isinstance(node, _XdrVariableLengthString):
        template = get_jinja_template(environment, "definition", node.template)
        print(template.render(name=node.name))
    elif isinstance(node, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "definition", node.template)
        print(template.render(name=node.name, size=node.size))
    elif isinstance(node, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "definition", node.template)
        print(template.render(name=node.name))
    elif isinstance(node, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "definition", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                size=node.size,
            )
        )
    elif isinstance(node, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "definition", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                ctype=node.spec.type_decorator,
            )
        )
    elif isinstance(node, _XdrOptionalData):
        raise NotImplementedError("<optional_data> typedef not yet implemented")
    elif isinstance(node, _XdrVoid):
        raise NotImplementedError("<void> typedef not yet implemented")
    else:
        raise NotImplementedError("typedef: type not recognized")


def emit_typedef_decoder(environment: Environment, node: _XdrDeclaration) -> None:
    """Emit a decoder function for one typedef"""
    if isinstance(node, _XdrBasic):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
            )
        )
    elif isinstance(node, _XdrVariableLengthString):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                size=node.size,
            )
        )
    elif isinstance(node, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                size=node.size,
                ctype=node.spec.type_decorator,
            )
        )
    elif isinstance(node, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "decoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrOptionalData):
        raise NotImplementedError("<optional_data> typedef not yet implemented")
    elif isinstance(node, _XdrVoid):
        raise NotImplementedError("<void> typedef not yet implemented")
    else:
        raise NotImplementedError("typedef: type not recognized")


def emit_typedef_encoder(environment: Environment, node: _XdrDeclaration) -> None:
    """Emit one encoder function for one typedef"""
    if isinstance(node, _XdrBasic):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
            )
        )
    elif isinstance(node, _XdrVariableLengthString):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrFixedLengthOpaque):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                size=node.size,
            )
        )
    elif isinstance(node, _XdrVariableLengthOpaque):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrFixedLengthArray):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                size=node.size,
            )
        )
    elif isinstance(node, _XdrVariableLengthArray):
        template = get_jinja_template(environment, "encoder", node.template)
        print(
            template.render(
                name=node.name,
                type=node.spec.type_name,
                maxsize=node.maxsize,
            )
        )
    elif isinstance(node, _XdrOptionalData):
        raise NotImplementedError("<optional_data> typedef not yet implemented")
    elif isinstance(node, _XdrVoid):
        raise NotImplementedError("<void> typedef not yet implemented")
    else:
        raise NotImplementedError("typedef: type not recognized")


class XdrTypedefGenerator(SourceGenerator):
    """Generate source code for XDR typedefs"""

    def __init__(self, language: str, peer: str):
        """Initialize an instance of this class"""
        self.environment = create_jinja2_environment(language, "typedef")
        self.peer = peer

    def emit_definition(self, node: _XdrTypedef) -> None:
        """Emit one definition for an XDR typedef"""
        emit_type_definition(self.environment, node.declaration)

    def emit_decoder(self, node: _XdrTypedef) -> None:
        """Emit one decoder function for an XDR typedef"""
        emit_typedef_decoder(self.environment, node.declaration)

    def emit_encoder(self, node: _XdrTypedef) -> None:
        """Emit one encoder function for an XDR typedef"""
        emit_typedef_encoder(self.environment, node.declaration)
