"""Python code generator for TL-B schemas.

Generates Python dataclasses with serialize/deserialize methods from
the resolved sema IR. Uses the runtime support library in tlb.object.
"""

import ast
from collections.abc import Mapping

from ..identity_key import IdentityKey
from ..sema.types import Module, NatLiteral, ResolvedConstructor, ResolvedType
from ..simplify_config import SimplifyConfig
from .context import PyContext
from .manifest import PyManifest
from .source_builder import SourceBuilder
from .type_generator import TypeGenerator


class _AugSourceError(Exception):
    pass


def _splice_aug_source(aug_source: str, ctx: PyContext) -> str:
    """Validate the auxiliary source and return its spliceable body.

    Top-level statements are limited to: a module docstring, classes,
    functions, module-level assignments, and `from X import …` lines.
    Imports are validated against the codegen's normal import-emission
    machinery (so the import block at the top of the generated module
    covers them) and stripped from the splice output. All other top-level
    definitions must use names that don't collide with anything codegen
    already binds at module level.

    Each `from X import Y` classifies as one of:
      - **runtime import** — `X` is one of the codegen's known runtime
        modules and `Y` is in its name list. Recorded via `ctx.use(Y)`.
      - **self import** — `X` is the current generated module. Each `Y`
        must already be in this module's manifest.
      - **foreign import** — `X` matches a `py_module` of one of
        `ctx.foreign_manifests`. Each `Y` must be in that manifest;
        recorded via `ctx.register_foreign_import`.
    """
    tree = ast.parse(aug_source)

    own_module = ctx.manifest.py_module
    own_basename = own_module.rsplit(".", 1)[-1]
    own_manifest_names = (
        set(ctx.manifest.type_names.values())
        | set(ctx.manifest.constructor_names.values())
        | set(ctx.manifest.type_info_names.values())
    )
    own_self_import_names = own_manifest_names | set(ctx.manifest.field_names.values())
    foreign_by_module: dict[str, set[str]] = {
        m.py_module: (
            set(m.type_names.values())
            | set(m.constructor_names.values())
            | set(m.type_info_names.values())
            | set(m.field_names.values())
        )
        for m in ctx.foreign_manifests.values()
    }

    # Names that already occupy the generated module's top-level scope.
    # New top-level definitions in the aug source must not collide with any
    # of these. Field names are scoped to their owning class — not module
    # level — so they're omitted from this set.
    reserved: set[str] = set(own_manifest_names) | set(ctx.used_imports)
    for fmap in ctx.foreign_imports.values():
        reserved.update(fmap.values())

    introduced: set[str] = set()
    kept: list[ast.stmt] = []
    for i, node in enumerate(tree.body):
        if (
            i == 0
            and isinstance(node, ast.Expr)
            and isinstance(node.value, ast.Constant)
            and isinstance(node.value.value, str)
        ):
            continue  # module docstring
        if isinstance(node, ast.Import):
            raise _AugSourceError(
                (
                    f"line {node.lineno}: bare `import X` is not supported in aug "
                    "source; use `from X import …`"
                )
            )
        if isinstance(node, ast.ImportFrom):
            _validate_aug_import(
                node, ctx, own_module, own_basename, own_self_import_names, foreign_by_module
            )
            continue
        names = _names_introduced(node)
        if names is None:
            raise _AugSourceError(
                (
                    f"line {node.lineno}: only docstrings, imports, classes, "
                    f"functions, and module-level assignments are allowed at "
                    f"the top level; got {type(node).__name__}"
                )
            )
        for name in names:
            if name in reserved:
                raise _AugSourceError(
                    (
                        f"line {node.lineno}: name {name!r} collides with a "
                        "name already in the generated module"
                    )
                )
            if name in introduced:
                raise _AugSourceError(
                    (f"line {node.lineno}: duplicate top-level name {name!r} in aug source")
                )
            introduced.add(name)
        kept.append(node)

    return ast.unparse(ast.Module(body=kept, type_ignores=tree.type_ignores))


def _names_introduced(node: ast.stmt) -> list[str] | None:
    """Return the top-level names defined by `node`, or None if `node` is
    not an allowed top-level definition form."""
    if isinstance(node, ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef):
        return [node.name]
    if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
        return [node.target.id]
    if isinstance(node, ast.Assign):
        out: list[str] = []
        for target in node.targets:
            extracted = _names_in_assign_target(target)
            if extracted is None:
                return None
            out.extend(extracted)
        return out
    return None


def _names_in_assign_target(target: ast.expr) -> list[str] | None:
    if isinstance(target, ast.Name):
        return [target.id]
    if isinstance(target, ast.Tuple | ast.List):
        out: list[str] = []
        for elem in target.elts:
            extracted = _names_in_assign_target(elem)
            if extracted is None:
                return None
            out.extend(extracted)
        return out
    return None


def _validate_aug_import(
    node: ast.ImportFrom,
    ctx: PyContext,
    own_module: str,
    own_basename: str,
    own_names: set[str],
    foreign_by_module: dict[str, set[str]],
) -> None:
    mod = node.module
    aliased = [(a.name, a.asname or a.name) for a in node.names]

    is_self = (node.level > 0 and (mod is None or mod == own_basename)) or (
        node.level == 0 and mod == own_module
    )
    if is_self:
        for name, asname in aliased:
            if asname != name:
                raise _AugSourceError(
                    (
                        f"line {node.lineno}: self-import alias not supported "
                        f"({name!r} as {asname!r})"
                    )
                )
            if name not in own_names:
                raise _AugSourceError(
                    (
                        f"line {node.lineno}: {name!r} imported from self but "
                        "is not defined in this module"
                    )
                )
        return

    if mod is not None:
        runtime_names = ctx.runtime_import_names(mod)
        if runtime_names is not None:
            for name, asname in aliased:
                if asname != name:
                    raise _AugSourceError(
                        (
                            f"line {node.lineno}: runtime-import alias not "
                            f"supported ({name!r} as {asname!r})"
                        )
                    )
                if name not in runtime_names:
                    raise _AugSourceError(
                        (f"line {node.lineno}: {name!r} not in codegen's import list for {mod!r}")
                    )
                ctx.use(name)
            return

    if mod is not None and mod in foreign_by_module:
        foreign_names = foreign_by_module[mod]
        for name, asname in aliased:
            if name not in foreign_names:
                raise _AugSourceError(
                    (f"line {node.lineno}: {name!r} not exported by foreign module {mod!r}")
                )
            ctx.register_foreign_import(mod, name, asname)
        return

    raise _AugSourceError(
        (
            f"line {node.lineno}: unrecognized import module {mod!r}; must be "
            "the current module, a foreign-imported module, or a codegen "
            "runtime module"
        )
    )


def _anonymous_constructor_name(type_name: str, c: ResolvedConstructor) -> str:
    literals = [v for v in c.nat_param_values if isinstance(v, NatLiteral)]
    if len(c.parent_type.type_level_params) == 1 and len(literals) == 1:
        return f"{type_name}_{literals[0].value}"
    return f"{type_name}_cons"


def generate_python(
    types: list[ResolvedType],
    *,
    current_module: Module,
    py_module: str,
    simplify: SimplifyConfig | None = None,
    foreign_manifests: Mapping[Module, PyManifest] | None = None,
    aug_source: str | None = None,
) -> tuple[str, PyManifest]:
    """Generate Python source code for a list of resolved types.

    `current_module` is the sema Module the types belong to; it is what
    `t.origin_module` is compared against to decide whether `t` is local or
    foreign. `py_module` is the dotted Python import path the produced file
    will live at (e.g. "block.generated"). `foreign_manifests` maps each
    imported sema Module to its previously captured PyManifest. References
    to types/constructors from those modules are bound and imported on
    demand during codegen.

    `aug_source` is optional Python source code (typically read from a
    type-checked editor file) that gets pasted at the bottom of the
    generated module. Imports of names from this same module are stripped —
    the spliced code references generated types directly. This is how
    HashmapAug augmentation classes are wired without circular imports.
    """
    config = simplify or SimplifyConfig.none()
    ctx = PyContext(
        current_module=current_module,
        py_module=py_module,
        simplify=config,
        foreign_manifests=foreign_manifests,
    )

    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        type_name = t.name or "Anon"
        if t.has_unnamed_sole_constructor:
            # Unnamed sole constructor: use the type name directly, don't bind type
            cons = t.constructors[0]
            _ = ctx.bind_constructor(cons, type_name)
        else:
            _ = ctx.bind_type(t, type_name)
            for c in t.constructors:
                _ = ctx.bind_constructor(c, c.name or _anonymous_constructor_name(type_name, c))

    # Bind TypeInfo class names for multi-constructor types AFTER all type
    # and constructor names are bound. This way user-defined names take their
    # bare identifiers and the auto-derived `{X}Type` gets a suffix on collision.
    for t in types:
        if t.is_builtin or not t.constructors or t.has_sole_constructor:
            continue
        _ = ctx.bind_type_info(t)

    # Pre-bind all field names so cross-type inference chain lookups work.
    type_generators: list[TypeGenerator] = []
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        tg = TypeGenerator(ctx, t)
        type_generators.append(tg)

    # Capture field bindings into the manifest now that all constructors are
    # registered (their child scopes hold the field bindings).
    for t in types:
        if t.is_builtin or not t.constructors:
            continue
        for c in t.constructors:
            cons_scope = ctx.get_constructor(c).scope
            for f in c.fields:
                if cons_scope.is_bound(f):
                    ctx.manifest.field_names[IdentityKey(f)] = cons_scope.lookup(f)

    body = SourceBuilder()
    for tg in type_generators:
        tg.generate(body)

    # Validate and strip the auxiliary source up front — splicing may
    # register new runtime/foreign imports through ctx, which need to be
    # in the import block emitted next.
    spliced_aug = _splice_aug_source(aug_source, ctx) if aug_source is not None else None

    sb = SourceBuilder()
    ctx.emit_imports(sb)
    sb.blank()
    sb.line(body.build().rstrip())
    if spliced_aug is not None:
        sb.blank()
        sb.blank()
        sb.line("# === inlined augmentation source ===")
        sb.blank()
        sb.line(spliced_aug)
    sb.blank()
    return sb.build(), ctx.manifest
