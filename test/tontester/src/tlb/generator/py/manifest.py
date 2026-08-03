"""PyManifest — names chosen during Python codegen for one module.

Captured by `generate_python` and consumed by downstream module codegen so
imported types/constructors get rebound to the same Python identifiers (or
re-aliased via `from x import y as z` if the consumer's scope collides).
"""

from dataclasses import dataclass, field

from ..identity_key import IdentityKey
from ..sema.types import ResolvedConstructor, ResolvedField, ResolvedType


@dataclass
class PyManifest:
    """Per-module mapping of resolved sema objects to their bound Python names.

    `py_module` is the dotted import path of the file these names live in,
    e.g. "block.generated". Downstream codegen uses it to emit
    `from {py_module} import {name}` for any referenced entry.

    `field_names` covers fields of the module's constructors. Consumers reach
    into them when an inference chain crosses the module boundary (e.g.
    `m.first.value` where `first` is bound here).
    """

    py_module: str
    type_names: dict[IdentityKey[ResolvedType], str] = field(default_factory=dict)
    constructor_names: dict[IdentityKey[ResolvedConstructor], str] = field(default_factory=dict)
    field_names: dict[IdentityKey[ResolvedField], str] = field(default_factory=dict)
    # TypeInfo class names for multi-constructor types. Derived as `{X}Type`
    # by default but reserved through NameScope so user names can take
    # priority and the TypeInfo gets a suffix on collision.
    type_info_names: dict[IdentityKey[ResolvedType], str] = field(default_factory=dict)
