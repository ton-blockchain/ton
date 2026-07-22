"""Top-level semantic analysis orchestrator.

Phases:
1. Register types (names, arities, param kinds)
2. Resolve constructors (expressions, fields)
3. Compute tags (CRC32 auto-tags)
4. Build match trees (bit dispatch + constraint disambiguation)
5. Classify inference capability
6. Compute deserialization plans
7. Classify types (enum, typedef)
"""

from collections.abc import Mapping

from ..ast_nodes import Constructor, Schema
from ..lexer import Lexer
from ..parser import Parser
from .deser import build_deser_plan, classify_inference
from .match import build_match_tree
from .resolve import (
    AnalyzedModule as AnalyzedModule,
)
from .resolve import (
    TypeRegistry,
    check_type_arities,
    insert_implicit_constraints,
    register_types,
    resolve_constructors,
)
from .tags import assign_tags
from .types import (
    AnonymousRecordType,
    CellRefType,
    Module,
    ResolvedType,
    ResolvedTypeExpr,
    SemaError,
    TupleType,
    TypeApply,
)
from .well_known import classify_well_known


def analyze(
    schema: Schema,
    *,
    current_module: Module,
    imports: Mapping[str, AnalyzedModule] | None = None,
) -> AnalyzedModule:
    """Run semantic analysis on a parsed schema.

    `current_module` identifies the module being analyzed; types defined here
    will carry it as their `origin_module`. `imports` maps `//@import` paths
    to already-analyzed modules.
    """
    available_imports = imports or {}
    registry = TypeRegistry(current_module)

    # Register local types first so they shadow same-named imports.
    register_types(schema, registry)

    # Resolve and load imports.
    for imp in schema.imports:
        target = available_imports.get(imp.path)
        if target is None:
            raise SemaError(f"unresolved import '{imp.path}': not found in imports dict")
        registry.add_imported(target)

    resolve_constructors(schema, registry)

    user_types = registry.all_user_types()
    check_type_arities(user_types)
    insert_implicit_constraints(user_types)

    ast_by_type: dict[str, list[Constructor]] = {}
    for c in schema.constructors:
        ast_by_type.setdefault(c.result_type, []).append(c)
    for rt in user_types:
        ast_constructors = ast_by_type.get(rt.name, [])
        if ast_constructors:
            assign_tags(rt, ast_constructors)

    for rt in user_types:
        rt.match_tree = build_match_tree(rt)

    for rt in user_types:
        rt.inference = classify_inference(rt)

    for rt in user_types:
        for rc in rt.constructors:
            _ = build_deser_plan(rc)

    for rt in user_types:
        _classify_type(rt)
        classify_well_known(rt)

    user_types = _toposort(user_types)

    return AnalyzedModule(module=current_module, registry=registry, types=user_types)


def analyze_text(
    text: str,
    *,
    current_module: Module,
    imports: Mapping[str, AnalyzedModule] | None = None,
) -> AnalyzedModule:
    """Convenience: lex, parse, and analyze a TL-B schema string."""
    tokens = Lexer(text).tokenize()
    schema = Parser(tokens).parse()
    return analyze(schema, current_module=current_module, imports=imports)


def _classify_type(rt: ResolvedType) -> None:
    """Set is_enum, is_typedef, is_special flags."""
    if rt.constructors:
        rt.is_enum = all(len(c.fields) == 0 for c in rt.constructors)
        rt.is_special = rt.constructors[0].is_special  # all agree (checked in sema_resolve)

    if len(rt.constructors) == 1:
        c = rt.constructors[0]
        if (
            len(c.fields) == 1
            and len(c.params) == 0
            and c.fields[0].name is None
            and c.tag_len == 0
        ):
            rt.is_typedef = True
            rt.typedef_target = c.fields[0].type_expr


def _toposort(types: list[ResolvedType]) -> list[ResolvedType]:
    """Topologically sort types so dependencies come before dependents."""
    type_set = set(id(t) for t in types)
    deps: dict[int, set[int]] = {id(t): set() for t in types}

    for t in types:
        for c in t.constructors:
            for f in c.fields:
                _collect_deps(f.type_expr, type_set, deps[id(t)])
        deps[id(t)].discard(id(t))

    # Kahn's algorithm
    dependents: dict[int, list[int]] = {tid: [] for tid in deps}
    in_degree = {tid: len(d) for tid, d in deps.items()}
    for tid, d in deps.items():
        for dep in d:
            if dep in dependents:
                dependents[dep].append(tid)

    queue = [tid for tid, deg in in_degree.items() if deg == 0]
    result_ids: list[int] = []
    while queue:
        tid = queue.pop(0)
        result_ids.append(tid)
        for dependent in dependents.get(tid, []):
            in_degree[dependent] -= 1
            if in_degree[dependent] == 0:
                queue.append(dependent)

    remaining = [tid for tid in deps if tid not in set(result_ids)]
    result_ids.extend(remaining)

    by_id = {id(t): t for t in types}
    return [by_id[tid] for tid in result_ids]


def _collect_deps(expr: ResolvedTypeExpr, type_set: set[int], deps: set[int]) -> None:
    """Collect type dependencies from a type expression."""
    if isinstance(expr, TypeApply):
        if id(expr.type) in type_set:
            deps.add(id(expr.type))
        for arg in expr.arguments:
            if isinstance(arg, TypeApply | CellRefType | TupleType | AnonymousRecordType):
                _collect_deps(arg, type_set, deps)
    elif isinstance(expr, CellRefType):
        _collect_deps(expr.inner, type_set, deps)
    elif isinstance(expr, TupleType):
        _collect_deps(expr.element, type_set, deps)
    elif isinstance(expr, AnonymousRecordType):
        if id(expr.type) in type_set:
            deps.add(id(expr.type))
        for c in expr.type.constructors:
            for f in c.fields:
                _collect_deps(f.type_expr, type_set, deps)
