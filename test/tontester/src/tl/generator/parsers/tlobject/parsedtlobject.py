import re
import zlib
from typing import override

from ...utils import get_class_name
from .tlarg import TLArg


class ParsedTLObject:
    def __init__(
        self,
        fullname: str,
        object_id: str | None,
        args: list[TLArg],
        result: str,
        is_function: bool,
    ):
        """
        Initializes a new TLObject, given its properties.

        :param fullname: The fullname of the TL object (namespace.name)
                         The namespace can be omitted.
        :param object_id: The hexadecimal string representing the object ID
        :param args: The arguments, if any, of the TL object
        :param result: The result type of the TL object
        :param is_function: Is the object a function or a type?
        """
        # The name can or not have a namespace
        self.fullname: str = fullname
        self.namespace: str
        self.name: str
        if "." in fullname:
            # self.namespace, self.name = fullname.split('.', maxsplit=1)
            self.namespace, self.name = fullname.split(".")[0], fullname
        else:
            self.namespace, self.name = "", fullname

        self.name = self.name.replace(".", "_")

        self.args: list[TLArg] = args
        self.result: str = result
        self.is_function: bool = is_function
        self.id: int
        if object_id is None:
            self.id = self.infer_id()
        else:
            self.id = int(object_id, base=16)

        # self.class_name: str = snake_to_camel_case(self.name, suffix='Request' if self.is_function else '')
        self.class_name: str = get_class_name(
            self.name, suffix="Request" if self.is_function else ""
        )

        self.real_args: list[TLArg] = list(
            a
            for a in self.sorted_args()
            if
            # (a.flag_indicator or a.generic_definition))
            not a.generic_definition
        )

    def sorted_args(self):
        """Returns the arguments properly sorted and ready to plug-in
        into a Python's method header (i.e., flags and those which
        can be inferred will go last so they can default =None)
        """
        return sorted(self.args, key=lambda x: bool(x.flag))

    @override
    def __repr__(self, ignore_id: bool = False):
        if ignore_id:
            hex_id = ""
        else:
            hex_id = "#{:08x}".format(self.id)

        if self.args:
            args = " " + " ".join([repr(arg) for arg in self.args])
        else:
            args = ""

        return "{}{}{} = {}".format(self.fullname, hex_id, args, self.result)

    def infer_id(self):
        representation = self.__repr__(ignore_id=True)
        representation = (
            representation.replace("<", " ")
            .replace(">", "")
            .replace("{", "")
            .replace("}", "")
            .replace("Vector", "vector")
        )

        # Remove optional empty values (special-cased to the true type)
        representation = re.sub(r" \w+:\w+\.\d+\?true", r"", representation)
        return zlib.crc32(representation.encode("ascii"))
