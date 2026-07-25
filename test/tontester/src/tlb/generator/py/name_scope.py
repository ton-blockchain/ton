"""Name scope for Python code generation.

Tracks registered names, avoids collisions with Python keywords, builtins,
and previously registered names. Maps sema objects (ResolvedType,
ResolvedConstructor, TypeParamDef, NatParamDef, ResolvedField) to unique Python identifiers.

Three binding classes control which objects can be bound to which kind of name:
- BindableForName: primary names (types, constructors, type-level params)
- BindableForField: field/param names with local-safe variants
- BindableForGeneric: type variable names (secondary binding on TypeLevelParam)
"""

import keyword

from ..identity_key import IdentityKey
from ..sema.types import (
    NatParamDef,
    ResolvedConstructor,
    ResolvedField,
    ResolvedType,
    TypeLevelParam,
    TypeParamDef,
)

type BindableForName = ResolvedType | ResolvedConstructor | TypeLevelParam
type BindableForField = ResolvedField | TypeParamDef | NatParamDef
type BindableForSetter = ResolvedField
type BindableForGeneric = TypeLevelParam

_PYTHON_KEYWORDS: frozenset[str] = frozenset(keyword.kwlist) | frozenset(keyword.softkwlist)

_FIELD_RESERVED: frozenset[str] = _PYTHON_KEYWORDS | frozenset(
    {
        "HashmapDict",  # referenced in custom __init__ body for HashmapDict.of(...)
        "self",  # would clash with the implicit method receiver — Python errors
        "serialize_to",
        "serialize",
        "load_from",
        "get_output",
        "check_type",
    }
)

_RESERVED: frozenset[str] = _PYTHON_KEYWORDS | frozenset(
    {
        "Anon",  # reserved so anonymous types start at Anon_1
        "Augmentation",
        "BitsTypeConstructor",
        "BoolTypeInfo",
        "BoundedUintTypeConstructor",
        "Builder",
        "Cell",
        "CellRefType",
        "Exception",
        "False",
        "FalseTypeInfo",
        "GenericTLBType",
        "HashmapDict",
        "InstantiableTypeInfo",
        "IntTypeConstructor",
        "Mapping",
        "MaybeTypeConstructor",
        "None",
        "Ref",
        "SelfTypeInfoTag",
        "Slice",
        "TLBRecord",
        "TLBType",
        "TlbModelError",
        "True",
        "TrueTypeInfo",
        "TupleTypeConstructor",
        "TypeError",
        "TypeInfo",
        "UintTypeConstructor",
        "UnaryTypeInfo",
        "UnitAug",
        "UnitTypeInfo",
        "ValueError",
        "VarIntTypeConstructor",
        "VarUIntTypeConstructor",
        "_can_be_used_as_generic_arg",
        "bitarray",
        "bool",
        "cell",
        "cls",
        "cs",
        "dict",
        "int",
        "isinstance",
        "len",
        "list",
        "object",
        "print",
        "range",
        "set",
        "str",
        "super",
        "tuple",
        "type",
    }
)

type Bindable = BindableForName | BindableForField


class NameScope:
    """Manages name bindings for a scope in generated code.

    Each scope tracks which names are taken and provides collision-free
    name generation. Names are bound to sema objects so codegen can look
    up the generated Python name for any sema construct.
    """

    _used: set[str]
    _bindings: dict[IdentityKey[Bindable], str]
    _local_bindings: dict[IdentityKey[Bindable], str]
    _generic_bindings: dict[IdentityKey[BindableForGeneric], str]
    _setter_bindings: dict[IdentityKey[BindableForSetter], str]
    _type_info_bindings: dict[IdentityKey[ResolvedType], str]
    _parent: NameScope | None

    def __init__(self, parent: NameScope | None = None) -> None:
        self._used = set()
        self._bindings = {}
        self._local_bindings = {}
        self._generic_bindings = {}
        self._setter_bindings = {}
        self._type_info_bindings = {}
        self._parent = parent

    def _is_taken(self, name: str) -> bool:
        if name in _RESERVED:
            return True
        if name in self._used:
            return True
        if self._parent is not None:
            return self._parent._is_taken(name)
        return False

    def _make_unique(self, preferred: str) -> str:
        """Return preferred name or a suffixed variant if taken."""
        candidate = preferred
        suffix = 1
        while self._is_taken(candidate):
            candidate = f"{preferred}_{suffix}"
            suffix += 1
        return candidate

    def bind(self, obj: BindableForName, preferred: str) -> str:
        """Register a primary name for a sema object. Returns the actual name used."""
        name = self._make_unique(preferred)
        self._used.add(name)
        self._bindings[IdentityKey(obj)] = name
        return name

    def bind_generic(self, obj: BindableForGeneric, preferred: str) -> str:
        """Register a type variable name for a TypeLevelParam. Returns the actual name used."""
        name = self._make_unique(preferred)
        self._used.add(name)
        self._generic_bindings[IdentityKey(obj)] = name
        return name

    def bind_setter(self, obj: BindableForSetter, preferred: str) -> str:
        """Register a setter method name for an inlined property. Returns the actual name used."""
        name = self._make_unique(preferred)
        self._used.add(name)
        self._setter_bindings[IdentityKey(obj)] = name
        return name

    def bind_type_info(self, t: ResolvedType, preferred: str) -> str:
        """Bind the auto-derived TypeInfo class name for a multi-cons type.

        Tracked separately from primary names so a user-defined type named
        `XType` can take the bare `XType` symbol while the TypeInfo of `X`
        gets a suffix.
        """
        name = self._make_unique(preferred)
        self._used.add(name)
        self._type_info_bindings[IdentityKey(t)] = name
        return name

    def lookup_type_info(self, t: ResolvedType) -> str:
        """Get the TypeInfo class name for `t`. Must have been bound."""
        key = IdentityKey(t)
        if key in self._type_info_bindings:
            return self._type_info_bindings[key]
        if self._parent is not None:
            return self._parent.lookup_type_info(t)
        raise KeyError(f"no TypeInfo binding for {t!r}")

    def is_type_info_bound(self, t: ResolvedType) -> bool:
        """Check whether `t` already has a TypeInfo binding in this or any parent scope."""
        key = IdentityKey(t)
        if key in self._type_info_bindings:
            return True
        if self._parent is not None:
            return self._parent.is_type_info_bound(t)
        return False

    def lookup_setter(self, obj: BindableForSetter) -> str:
        """Get the setter method name for a ResolvedField."""
        key = IdentityKey(obj)
        if key in self._setter_bindings:
            return self._setter_bindings[key]
        if self._parent is not None:
            return self._parent.lookup_setter(obj)
        raise KeyError(f"no setter binding for {obj!r}")

    def bind_field(self, obj: BindableForField, preferred: str) -> str:
        """Register a field name and a corresponding local variable name.

        The field name (returned, used for self.X) avoids only Python keywords
        and sibling collisions. The local variable name (retrieved via
        lookup_local) additionally avoids _RESERVED names like 'cs' and 'cls'
        that are used as method parameters in generated code.
        """
        field_name = preferred
        suffix = 1
        while field_name in _FIELD_RESERVED or field_name in self._used:
            field_name = f"{preferred}_{suffix}"
            suffix += 1
        self._used.add(field_name)
        self._bindings[IdentityKey(obj)] = field_name

        local_name = field_name
        key = IdentityKey(obj)
        suffix = 1
        while self._is_taken(local_name) or local_name in self._local_bindings.values():
            local_name = f"{preferred}_{suffix}"
            suffix += 1
        self._local_bindings[key] = local_name
        return field_name

    def lookup_local(self, obj: BindableForField | BindableForName) -> str:
        """Get the local variable name for a sema object.

        For field-bound objects, returns the local-safe name (avoiding cs/cls etc.).
        For other objects, falls back to the regular binding.
        """
        key = IdentityKey(obj)
        if key in self._local_bindings:
            return self._local_bindings[key]
        if key in self._bindings:
            return self._bindings[key]
        if self._parent is not None:
            return self._parent.lookup_local(obj)
        raise KeyError(f"no local binding for {obj!r}")

    def reserve(self, name: str) -> str:
        """Reserve a name without binding to an object."""
        actual = self._make_unique(name)
        self._used.add(actual)
        return actual

    def lookup(self, obj: Bindable) -> str:
        """Get the Python name for a sema object. Must have been bound."""
        key = IdentityKey(obj)
        if key in self._bindings:
            return self._bindings[key]
        if self._parent is not None:
            return self._parent.lookup(obj)
        raise KeyError(f"no binding for {obj!r}")

    def is_bound(self, obj: Bindable) -> bool:
        """Check whether `obj` already has a binding in this scope or any parent."""
        key = IdentityKey(obj)
        if key in self._bindings:
            return True
        if self._parent is not None:
            return self._parent.is_bound(obj)
        return False

    def lookup_generic(self, obj: BindableForGeneric) -> str:
        """Get the type variable name for a TypeLevelParam."""
        key = IdentityKey(obj)
        if key in self._generic_bindings:
            return self._generic_bindings[key]
        if self._parent is not None:
            return self._parent.lookup_generic(obj)
        raise KeyError(f"no generic binding for {obj!r}")

    def child(self) -> NameScope:
        """Create a child scope that inherits this scope's names."""
        return NameScope(parent=self)
