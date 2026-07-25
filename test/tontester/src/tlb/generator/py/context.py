"""Shared context for Python code generation.

Owns the import tracker and name scope.
Both codegen and strategy modules depend on this, avoiding circular imports.
"""

from collections.abc import Mapping
from typing import Protocol

from ..identity_key import IdentityKey
from ..sema.types import (
    Module,
    ResolvedConstructor,
    ResolvedField,
    ResolvedType,
    TypeParamDef,
)
from ..simplify_config import SimplifyConfig
from .manifest import PyManifest
from .name_scope import NameScope
from .source_builder import SourceBuilder


class ConstructorInfo(Protocol):
    """Protocol for constructor codegen data needed by cross-type lookups."""

    scope: NameScope
    type_params: list[TypeParamDef]

    def type_var_name(self, p: TypeParamDef) -> str: ...


class PyContext:
    """Shared state for a single file generation run."""

    current_module: Module
    manifest: PyManifest
    scope: NameScope
    simplify: SimplifyConfig
    used_imports: set[str]
    foreign_imports: dict[str, dict[str, str]]
    foreign_manifests: Mapping[Module, PyManifest]
    _next_tmp: int
    _type_scopes: dict[IdentityKey[ResolvedType], NameScope]
    _constructors: dict[IdentityKey[ResolvedConstructor], ConstructorInfo]

    def __init__(
        self,
        *,
        current_module: Module,
        py_module: str,
        simplify: SimplifyConfig | None = None,
        foreign_manifests: Mapping[Module, PyManifest] | None = None,
    ) -> None:
        self.current_module = current_module
        self.manifest = PyManifest(py_module=py_module)
        self.scope = NameScope()
        self.simplify = simplify or SimplifyConfig.none()
        self.used_imports = set()
        self.foreign_imports = {}
        self.foreign_manifests = foreign_manifests or {}
        self._next_tmp = 0
        self._type_scopes = {}
        self._constructors = {}

    def runtime_import_names(self, module: str) -> set[str] | None:
        """Return the runtime-library names available for `module`, or None
        if `module` is not one the codegen knows how to emit imports from."""
        if module not in self._IMPORTS:
            return None
        return set(self._IMPORTS[module])

    def register_foreign_import(self, py_module: str, source: str, bound: str) -> None:
        existing = self.foreign_imports.setdefault(py_module, {})
        if source in existing:
            assert existing[source] == bound, (
                f"inconsistent foreign binding for '{source}' in '{py_module}': "
                f"{existing[source]!r} vs {bound!r}"
            )
        else:
            existing[source] = bound

    def _is_foreign_type(self, t: ResolvedType) -> bool:
        return t.origin_module is not None and t.origin_module is not self.current_module

    def _is_foreign_constructor(self, c: ResolvedConstructor) -> bool:
        return self._is_foreign_type(c.parent_type)

    def lookup_type(self, t: ResolvedType) -> str:
        """Look up the Python name of `t`. Lazily binds and registers an import
        if `t` is from a foreign module."""
        if self._is_foreign_type(t):
            return self._discover_foreign_type(t)
        return self.scope.lookup(t)

    def lookup_constructor(self, c: ResolvedConstructor) -> str:
        """Look up the Python name of `c`. Lazily binds and registers an import
        if `c` belongs to a foreign type."""
        if self._is_foreign_constructor(c):
            return self._discover_foreign_constructor(c)
        return self.scope.lookup(c)

    def lookup_field(self, c: ResolvedConstructor, f: ResolvedField) -> str:
        """Look up the Python name of field `f` belonging to constructor `c`.

        Local fields live in the constructor's child scope; foreign fields are
        read from the foreign module's PyManifest.
        """
        if self._is_foreign_constructor(c):
            assert c.parent_type.origin_module is not None
            manifest = self.foreign_manifests.get(c.parent_type.origin_module)
            assert manifest is not None, (
                f"foreign constructor {c.name!r} of type {c.parent_type.name!r} "
                "has no PyManifest in foreign_manifests"
            )
            key = IdentityKey(f)
            assert key in manifest.field_names, (
                f"foreign field {f.name!r} of constructor {c.name!r} not present "
                f"in manifest for type {c.parent_type.name!r}"
            )
            return manifest.field_names[key]
        return self._constructors[IdentityKey(c)].scope.lookup(f)

    def _discover_foreign_type(self, t: ResolvedType) -> str:
        if self.scope.is_bound(t):
            return self.scope.lookup(t)
        assert t.origin_module is not None
        manifest = self.foreign_manifests.get(t.origin_module)
        assert manifest is not None, (
            f"foreign type {t.name!r} from module {t.origin_module.name!r} "
            "has no PyManifest in foreign_manifests"
        )
        key = IdentityKey(t)
        assert key in manifest.type_names, (
            f"foreign type {t.name!r} not present in manifest for module {t.origin_module.name!r}"
        )
        source = manifest.type_names[key]
        bound = self.scope.bind(t, source)
        self.register_foreign_import(manifest.py_module, source, bound)
        return bound

    def _discover_foreign_constructor(self, c: ResolvedConstructor) -> str:
        if self.scope.is_bound(c):
            return self.scope.lookup(c)
        assert c.parent_type.origin_module is not None
        manifest = self.foreign_manifests.get(c.parent_type.origin_module)
        assert manifest is not None, (
            f"foreign constructor {c.name!r} of type {c.parent_type.name!r} "
            "has no PyManifest in foreign_manifests"
        )
        key = IdentityKey(c)
        assert key in manifest.constructor_names, (
            f"foreign constructor {c.name!r} not present in manifest for "
            f"type {c.parent_type.name!r}"
        )
        source = manifest.constructor_names[key]
        bound = self.scope.bind(c, source)
        self.register_foreign_import(manifest.py_module, source, bound)
        return bound

    def set_type_scope(self, t: ResolvedType, scope: NameScope) -> None:
        self._type_scopes[IdentityKey(t)] = scope

    def register_constructor(self, c: ResolvedConstructor, gen: ConstructorInfo) -> None:
        self._constructors[IdentityKey(c)] = gen

    def get_type_scope(self, t: ResolvedType) -> NameScope:
        return self._type_scopes[IdentityKey(t)]

    def bind_type(self, t: ResolvedType, preferred: str) -> str:
        """Bind a local type's primary Python name and capture it in the manifest."""
        name = self.scope.bind(t, preferred)
        self.manifest.type_names[IdentityKey(t)] = name
        return name

    def bind_constructor(self, c: ResolvedConstructor, preferred: str) -> str:
        """Bind a local constructor's Python name and capture it in the manifest."""
        name = self.scope.bind(c, preferred)
        self.manifest.constructor_names[IdentityKey(c)] = name
        return name

    def bind_type_info(self, t: ResolvedType) -> str:
        """Bind the auto-derived TypeInfo class name for a multi-cons type.

        The preferred name is `{type_name}Type`; NameScope suffixes on
        collision so a user-defined `XType` keeps its bare name. Captured in
        the manifest.
        """
        preferred = f"{self.scope.lookup(t)}Type"
        name = self.scope.bind_type_info(t, preferred)
        self.manifest.type_info_names[IdentityKey(t)] = name
        return name

    def lookup_type_info(self, t: ResolvedType) -> str:
        """Return the Python name of the TypeInfo class for multi-constructor type `t`."""
        if self._is_foreign_type(t):
            return self._discover_foreign_type_info(t)
        return self.scope.lookup_type_info(t)

    def _discover_foreign_type_info(self, t: ResolvedType) -> str:
        if self.scope.is_type_info_bound(t):
            return self.scope.lookup_type_info(t)
        # Ensure the type itself is bound and imported first.
        _ = self._discover_foreign_type(t)
        assert t.origin_module is not None
        manifest = self.foreign_manifests[t.origin_module]
        key = IdentityKey(t)
        assert key in manifest.type_info_names, (
            f"foreign multi-cons type {t.name!r} has no TypeInfo entry "
            f"in manifest for module {t.origin_module.name!r}"
        )
        source = manifest.type_info_names[key]
        bound = self.scope.bind_type_info(t, source)
        self.register_foreign_import(manifest.py_module, source, bound)
        return bound

    def get_constructor(self, c: ResolvedConstructor) -> ConstructorInfo:
        return self._constructors[IdentityKey(c)]

    def use(self, *names: str) -> None:
        """Mark imports as needed."""
        for n in names:
            self.used_imports.add(n)

    def tmp(self, prefix: str = "_tmp") -> str:
        """Generate a unique temporary variable name."""
        name = f"{prefix}_{self._next_tmp}"
        self._next_tmp += 1
        return name

    _IMPORTS: dict[str, list[str]] = {
        "collections.abc": ["Mapping"],
        "dataclasses": ["dataclass"],
        "typing": ["Literal", "final", "override"],
        "bitarray": ["bitarray"],
        "pytoniq_core": ["Builder", "Cell", "Slice"],
        "tlb.object": [
            "AnyType",
            "BitsTypeConstructor",
            "BoolTypeInfo",
            "BoundedUintTypeConstructor",
            "CellRefType",
            "FalseTypeInfo",
            "GenericTLBType",
            "InstantiableTypeInfo",
            "IntTypeConstructor",
            "MaybeTypeConstructor",
            "Ref",
            "SelfTypeInfoTag",
            "TLBRecord",
            "TLBType",
            "TlbModelError",
            "TrueTypeInfo",
            "TupleTypeConstructor",
            "TypeInfo",
            "UintTypeConstructor",
            "UnaryTypeInfo",
            "UnitTypeInfo",
            "VarIntTypeConstructor",
            "VarUIntTypeConstructor",
        ],
        "tlb.hashmap": ["Augmentation", "HashmapDict", "UnitAug"],
    }

    def emit_imports(self, sb: SourceBuilder) -> None:
        for module, names in self._IMPORTS.items():
            used = [n for n in names if n in self.used_imports]
            if used:
                sb.line(f"from {module} import {', '.join(used)}")
        for py_module in sorted(self.foreign_imports):
            names_map = self.foreign_imports[py_module]
            parts = [
                src if src == bnd else f"{src} as {bnd}" for src, bnd in sorted(names_map.items())
            ]
            sb.line(f"from {py_module} import {', '.join(parts)}")
