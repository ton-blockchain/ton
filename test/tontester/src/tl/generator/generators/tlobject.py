import io
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from ..parsers.tlobject import ParsedTLObject
from ..sourcebuilder import SourceBuilder
from ..utils import get_class_name

BASE_TYPES = {
    "string",
    "bytes",
    "int",
    "long",
    "int128",
    "int256",
    "int64",
    "int32",
    "int53",
    "double",
    "Bool",
    "true",
    "secureString",
    "secureBytes",
}

BASE64_ENCODED_TYPES = {
    "bytes",
    "int128",
    "int256",
    "secureBytes",
}


@dataclass
class ImportContext:
    literal_needed: bool = False
    base64_needed: bool = False


def _write_modules(
    out_file: Path,
    tlobjects: list[ParsedTLObject],
    type_constructors: dict[str, list[ParsedTLObject]],
):
    out_file.parent.mkdir(parents=True, exist_ok=True)
    source = io.StringIO()
    ctx = ImportContext()
    with SourceBuilder(source) as builder:
        type_names: set[str] = set()
        type_defs: list[str] = []

        for t in tlobjects:
            if t.name in BASE_TYPES:
                continue
            if not t.is_function:
                type_name = get_class_name(t.result)
                type_names.add(type_name)
                constructors = type_constructors[t.result]
                if not constructors:
                    pass
                elif len(constructors) == 1:
                    type_defs.append(
                        "type Type{} = {}".format(type_name, constructors[0].class_name)
                    )
                else:
                    type_defs.append(
                        f"type Type{type_name} = {' | '.join(c.class_name for c in constructors)}"
                    )

        # Generate the class for every TLObject
        for t in tlobjects:
            if t.name in BASE_TYPES:
                continue
            _write_source_code(t, t.is_function, builder, type_constructors, ctx)
            builder.current_indent = 0

        # Write the type definitions generated earlier.
        builder.writeln()
        for line in set(type_defs):
            builder.writeln(line)

    with out_file.open("w") as f, SourceBuilder(f) as builder:
        if ctx.literal_needed:
            builder.writeln("from typing import override, Self, Literal")
        else:
            builder.writeln("from typing import override, Self")
        if ctx.base64_needed:
            builder.writeln("import base64")
        builder.writeln("import tl")
        builder.writeln()
        builder.writeln(source.getvalue())


def _write_source_code(
    tlobject: ParsedTLObject,
    is_function: bool,
    builder: SourceBuilder,
    type_constructors: dict[str, list[ParsedTLObject]],
    ctx: ImportContext,
):
    """
    Writes the source code corresponding to the given TLObject
    by making use of the ``builder`` `SourceBuilder`.

    Additional information such as file path depth and
    the ``Type: [Constructors]`` must be given for proper
    importing and documentation strings.
    """
    _write_class_init(tlobject, is_function, builder, ctx)
    _write_to_dict(tlobject, builder, ctx)
    _write_from_dict(tlobject, type_constructors, builder)


def _write_class_init(
    tlobject: ParsedTLObject,
    is_function: bool,
    builder: SourceBuilder,
    ctx: ImportContext,
):
    builder.writeln()
    builder.writeln()
    builder.writeln(
        "class {}({}):", tlobject.class_name, "tl.TLRequest" if is_function else "tl.TLObject"
    )

    args = [a.get_init_arg() for a in tlobject.args]

    # Write the __init__ function if it has any argument
    if not tlobject.args:
        return

    builder.writeln(f"def __init__(self, {', '.join(args)}):")

    # Set the arguments
    for arg in tlobject.args:
        if arg.type == "true":
            ctx.literal_needed = True
        builder.writeln(arg.get_init_assignment())

    builder.end_block()


def _write_to_dict(tlobject: ParsedTLObject, builder: SourceBuilder, ctx: ImportContext):
    builder.writeln("@override")
    builder.writeln("def to_dict(self) -> dict[str, tl.JSONSerializable]:")
    builder.writeln("return {")
    builder.current_indent += 1

    builder.write("'@type': '{}'", tlobject.fullname)
    for arg in tlobject.args:
        builder.writeln(",")
        if arg.type in BASE_TYPES:
            builder.write(f"'{arg.name}': ")
            if arg.type in BASE64_ENCODED_TYPES:
                ctx.base64_needed = True
                if arg.is_vector:
                    builder.write(f"[base64.b64encode(x).decode() for x in self.{arg.name}]")
                else:
                    builder.write(f"base64.b64encode(self.{arg.name}).decode()")
            else:
                if arg.is_vector:
                    builder.write("self.{0}[:]", arg.name)
                else:
                    builder.write("self.{}", arg.name)
        else:
            if arg.is_vector:
                builder.write(f"'{arg.name}': ")
                builder.write(f"[x.to_dict() for x in self.{arg.name}]")
            else:
                builder.write(
                    f"**({{'{arg.name}': self.{arg.name}.to_dict()}} "
                    + f"if self.{arg.name} is not None else {{}})"
                )  # do not write object in json if it's None

    builder.writeln()
    builder.current_indent -= 1
    builder.writeln("}")

    builder.end_block()


def _write_from_dict(
    tlobject: ParsedTLObject,
    type_constructors: dict[str, list[ParsedTLObject]],
    builder: SourceBuilder,
):
    builder.writeln("@classmethod")
    builder.writeln("@override")
    builder.writeln("def from_dict(cls, d: tl.JSONSerializable) -> Self:")

    builder.writeln("if not isinstance(d, dict):")
    builder.writeln('raise tl.ModelError(f"Expected dict, got {type(d)}")')
    builder.current_indent -= 1

    builder.writeln(f'if d.get("@type", "{tlobject.fullname}") != "{tlobject.fullname}":')
    builder.writeln(
        f'raise tl.ModelError(f"Expected @type to be {tlobject.fullname}, got {{d.get("@type")}}")'
    )
    builder.current_indent -= 1

    for arg in tlobject.args:
        if arg.type in BASE_TYPES:
            deserializer = f"tl.deserialize_{arg.type}"
        elif arg.skip_constructor_id:
            name = arg.get_type_class_name()
            if not arg.is_vector:
                deserializer = f"tl.deserialize_object({name}.from_dict)"
            else:
                deserializer = f"{name}.from_dict"
        else:
            constructors = type_constructors.get(arg.type, [])
            builder.writeln(
                f"def deserialize_{arg.name}(value: tl.JSONSerializable) -> Type{arg.get_type_class_name()}:"
            )
            builder.writeln("if not isinstance(value, dict):")
            builder.writeln('raise tl.ModelError(f"Expected dict, got {type(value)}")')
            builder.current_indent -= 1
            for c in constructors:
                builder.writeln(
                    f'if value.get("@type") == "{c.fullname}": return {c.class_name}.from_dict(value)'
                )
            builder.writeln(
                f'raise tl.ModelError(f"Unknown constructor for {arg.type}: {{value.get("@type")}}")'
            )
            builder.current_indent -= 1
            if not arg.is_vector:
                deserializer = f"tl.deserialize_object(deserialize_{arg.name})"
            else:
                deserializer = f"deserialize_{arg.name}"

        if not arg.is_vector:
            builder.writeln(
                f"_{arg.name} = "
                + f"{deserializer}(d.get('{arg.name}', {arg.default_value_for_from_dict()}))"
            )
        else:
            builder.writeln(
                f"_{arg.name} = tl.deserialize_list(d.get('{arg.name}', []), {deserializer})"
            )

    builder.writeln(
        "return cls({})",
        ", ".join(f"{arg.name}=_{arg.name}" for arg in tlobject.args),
    )

    builder.end_block()


def sort_tlobjects(tlobjects: list[ParsedTLObject]):
    types: list[ParsedTLObject] = []
    type_constructors: dict[str, list[ParsedTLObject]] = defaultdict(list)
    for tlobject in tlobjects:
        types.append(tlobject)
        if not tlobject.is_function:
            type_constructors[tlobject.result].append(tlobject)
    return types, type_constructors


def generate_tlobjects(tlobjects: list[ParsedTLObject], fn: str, output_dir: Path):
    output_dir.mkdir(parents=True, exist_ok=True)
    py_typed_marker = output_dir / "py.typed"
    py_typed_marker.touch()

    types, type_constructors = sort_tlobjects(tlobjects)
    _write_modules(output_dir / (fn + ".py"), types, type_constructors)
