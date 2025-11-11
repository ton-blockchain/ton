import collections
import re
from pathlib import Path

from .parsedtlobject import ParsedTLObject
from .tlarg import TLArg

CORE_TYPES = {
    0xBC799737,  # boolFalse#bc799737 = Bool;
    0x997275B5,  # boolTrue#997275b5 = Bool;
    0x3FEDD339,  # true#3fedd339 = True;
    0xC4B9F9BB,  # error#c4b9f9bb code:int text:string = Error;
    0x56730BCC,  # null#56730bcc = Null;
}


def _from_line(line: str, is_function: bool) -> ParsedTLObject:
    match = re.match(
        r"^([\w.]+)"  # 'name'
        + r"(?:#([0-9a-fA-F]+))?"  # '#optionalcode'
        +
        # r'(?:\s{?\w+:(?:\([^)]*\)|[\w\d<>#.?!()]+)}?)*'
        r".*?"
        + r"\s=\s"  # ' = '
        + r"([\w\d<>#.?]+);$",  # '<result.type>;'
        line,
    )
    if match is None:
        raise ValueError("Cannot parse TLObject {}".format(line))

    args_match: list[tuple[str, str, str]] = re.findall(
        r"({)?"
        + r"(\w+)"
        + r":"
        +
        # r'([\w\d<>#.?!]+)',
        r"("
        +
        # r'(?:\([^)]*\)|[\w\d<>#.?!]+)'
        r"(?:[\w\d<>#.?!]+(?:\([^)]*\))?|\([^)]*\))"
        + r")",
        line,
    )

    for i, (brace, name, arg_type) in enumerate(args_match):
        if "vector" in arg_type:
            arg_type = arg_type.replace("(", "").replace(")", ">").replace("vector ", "Vector<")
        if "(" in arg_type and ")" in arg_type:
            arg_type = arg_type.replace("(", "").replace(")", "")
        args_match[i] = (brace, name, arg_type)

    name = match.group(1)

    return ParsedTLObject(
        fullname=name,
        object_id=match.group(2),
        result=match.group(3),
        is_function=is_function,
        args=[TLArg(name, arg_type, brace != "") for brace, name, arg_type in args_match],
    )


def parse_tl(file_path: Path, ignored_ids: set[int] = CORE_TYPES) -> list[ParsedTLObject]:
    """
    This method yields TLObjects from a given .tl file.

    Note that the file is parsed completely before the function yields
    because references to other objects may appear later in the file.
    """
    obj_all: list[ParsedTLObject] = []
    obj_by_name = {}
    obj_by_type: dict[str, list[ParsedTLObject]] = collections.defaultdict(list)
    with file_path.open() as file:
        incomplete = ""
        is_function = False
        for line in file:
            comment_index = line.find("//")
            if comment_index != -1:
                line = line[:comment_index]

            line = line.strip()
            if not line:
                continue

            match = re.match(r"---(\w+)---", line)
            if match:
                following_types = match.group(1)
                is_function = following_types == "functions"
                continue

            if ";" not in line:
                incomplete += line + " "
                continue
            elif incomplete:
                line = incomplete + line
                incomplete = ""

            try:
                result = _from_line(line, is_function)

                if result.id in ignored_ids:
                    continue

                obj_all.append(result)
                if not result.is_function:
                    obj_by_name[result.fullname] = result
                    obj_by_type[result.result].append(result)
            except ValueError as e:
                if all(
                    [
                        x not in str(e)
                        for x in (
                            "int ? = Int;",
                            "long ? = Long",
                            "double ? = Double;",
                            "string ? = String;",
                            "object ? = Object;",
                            "function ? = Function;",
                            "bytes data:string = Bytes;",
                            "true = True;",
                            "boolTrue = Bool;",
                            "boolFalse = Bool;",
                            "vector#1cb5c415 {t:Type} # [ t ] = Vector t;",
                            "vector {t:Type} # [ t ] = Vector t;",
                            "int128 4*[ int ] = Int128;",
                            "int256 8*[ int ] = Int256;",
                        )
                    ]
                ):
                    raise

    # Once all objects have been parsed, replace the
    # string type from the arguments with references
    # for obj in obj_all:
    #     for arg in obj.args:
    #         arg.cls = obj_by_type.get(arg.type) or (
    #             [obj_by_name[arg.type]] if arg.type in obj_by_name else []
    #         )

    return obj_all


def find_layer(file_path: Path):
    """Finds the layer used on the specified scheme.tl file."""
    layer_regex = re.compile(r"^//\s*LAYER\s*(\d+)$")
    with file_path.open("r") as file:
        for line in file:
            match = layer_regex.match(line)
            if match:
                return int(match.group(1))
