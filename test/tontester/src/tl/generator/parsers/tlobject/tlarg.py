import re

from ....generator.utils import get_class_name


class TLArg:
    def __init__(self, name: str, arg_type: str, generic_definition: bool):
        """
        Initializes a new .tl argument
        :param name: The name of the .tl argument
        :param arg_type: The type of the .tl argument
        :param generic_definition: Is the argument a generic definition?
                                   (i.e. {X:Type})
        """
        MANGLING = {
            "list": "list_",
            "self": "self_",
            "from": "from_",
            "bytes": "bytes_",
        }

        self.name: str = MANGLING.get(name, name)

        # Default values
        self.is_vector: bool = False
        self.flag: str | None = None  # name of the flag to check if self is present
        self.skip_constructor_id: bool = False
        self.flag_index: int = -1  # bit index of the flag to check if self is present
        # self.cls: list[ParsedTLObject] | None = None

        # The type can be an indicator that other arguments will be flags
        self.flag_indicator: bool
        self.type: str
        self.is_generic: bool
        if arg_type == "#":
            self.flag_indicator = True
            self.type = "int"
            self.is_generic = False
        else:
            self.flag_indicator = False
            self.is_generic = arg_type.startswith("!")
            # Strip the exclamation mark always to have only the name
            self.type = arg_type.lstrip("!")

            # The type may be a flag (FLAGS.IDX?REAL_TYPE)
            # FLAGS can be any name, but it should have appeared previously.
            flag_match = re.match(r"(\w+).(\d+)\?([\w<>.]+)", self.type)
            if flag_match:
                self.flag = flag_match.group(1)
                self.flag_index = int(flag_match.group(2))
                # Update the type to match the exact type, not the "flagged" one
                self.type = flag_match.group(3)

            # Then check if the type is a Vector<REAL_TYPE>
            vector_match = re.match(r"[Vv]ector<([\w\d.]+)>", self.type)
            if vector_match:
                self.is_vector = True

                # If the type's first letter is not uppercase, then
                # it is a constructor and we use (read/write) its ID
                self.use_vector_id: bool = self.type[0] == "V"

                # Update the type to match the one inside the vector
                self.type = vector_match.group(1)

            # See use_vector_id. An example of such case is ipPort in
            # help.configSpecial
            if self.type.split(".")[-1][0].islower():
                self.skip_constructor_id = True
        # if self.type in ('Int128' or 'Int256'):
        #     self.type = 'bytes'

        self.generic_definition: bool = generic_definition

    def get_type_class_name(self):
        return get_class_name(self.type)

    def type_hint(self):
        cls = self.type
        # if '.' in cls:
        #     cls = snake_to_camel_case('_'.join(cls.split('.')))
        result = {
            "int": "int",
            "long": "int",
            "int64": "int",
            "int32": "int",
            "int53": "int",
            "int128": "bytes",
            "int256": "bytes",
            "double": "float",
            "string": "str",
            "bytes": "bytes",
            "Bool": "bool",
            "true": "Literal[True]",
            "secureString": "str",
            "secureBytes": "bytes",
        }.get(cls)

        is_primitive = result is not None
        if not is_primitive:
            result = self.get_type_class_name()
            if not self.skip_constructor_id:
                result = f"Type{result}"

        if self.is_vector:
            result = f"list[{result}]"
        elif not is_primitive:
            result = f"{result} | None"

        return result

    def get_init_arg(self):
        if self.is_vector:
            return f"{self.name}: '{self.type_hint()} | None' = None"
        else:
            return f"{self.name}: '{self.type_hint()}' = {self.default_value()}"

    def get_init_assignment(self) -> str:
        if self.is_vector:
            return f"self.{self.name}: {self.type_hint()} = {self.name} if {self.name} is not None else []"
        else:
            return f"self.{self.name}: {self.type_hint()} = {self.name}"

    def default_value(self):
        if self.is_vector:
            return "[]"
        elif self.type in ("int", "long", "int64", "int32", "int53"):
            return "0"
        elif self.type == "bytes":
            return "b''"
        elif self.type == "int128":
            return "b'\\x00' * 16"
        elif self.type == "int256":
            return "b'\\x00' * 32"
        elif self.type == "string":
            return "''"
        elif self.type == "double":
            return "0.0"
        elif self.type == "Bool":
            return "False"
        elif self.type == "true":
            return "True"
        elif self.type == "secureString":
            return "''"
        elif self.type == "secureBytes":
            return "b''"
        else:
            return "None"

    def default_value_for_from_dict(self):
        if self.is_vector:
            return "[]"
        elif self.type == "bytes":
            return "''"
        elif self.type == "int128":
            return "'AAAAAAAAAAAAAAAAAAAAAA=='"
        elif self.type == "int256":
            return "'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='"
        elif self.type == "secureBytes":
            return "''"
        else:
            return self.default_value()
