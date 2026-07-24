from collections.abc import Mapping
from pathlib import Path

from tlb.generator.py import PyManifest, generate_python
from tlb.generator.sema import AnalyzedModule, analyze_text
from tlb.generator.sema.types import Module, WellKnownType
from tlb.generator.simplify_config import SimplifyConfig

import tl


def generate_tlb_python(
    schema_path: Path,
    out_path: Path,
    *,
    py_module: str,
    simplify: SimplifyConfig | None = None,
    imports: Mapping[str, AnalyzedModule] | None = None,
    foreign_manifests: Mapping[Module, PyManifest] | None = None,
    aug_source_path: Path | None = None,
) -> tuple[AnalyzedModule, PyManifest]:
    """Generate Python code from a TL-B schema file.

    `py_module` is the dotted import path the produced file lives at.
    `imports` provides analyzed modules to satisfy `//@import` directives.
    `foreign_manifests` provides Python-name maps for those modules so the
    generated code can reference them via `from … import …`.
    `aug_source_path` is an optional file whose content is spliced at the
    bottom of the generated module — used to attach hand-written
    augmentation classes without creating a circular import.
    """
    text = schema_path.read_text()
    analyzed = analyze_text(text, current_module=Module(schema_path.stem), imports=imports)
    aug_source = aug_source_path.read_text() if aug_source_path is not None else None
    code, manifest = generate_python(
        analyzed.types,
        current_module=analyzed.module,
        py_module=py_module,
        simplify=simplify,
        foreign_manifests=foreign_manifests,
        aug_source=aug_source,
    )
    _ = out_path.write_text(code)
    print(f"  {schema_path.name} -> {out_path.name}")
    return analyzed, manifest


if __name__ == "__main__":
    repo_root = Path(__file__).resolve().parents[2]
    schemas_root = repo_root / "tl/generate/scheme"

    schemas = [
        schemas_root / "lite_api.tl",
        schemas_root / "ton_api.tl",
        schemas_root / "tonlib_api.tl",
    ]
    out_directory = repo_root / "test/tontester/src/tonapi"

    for schema in schemas:
        tl.generate(schema, out_directory)

    tl.generate(
        repo_root / "test/tontester/tests/tl/test_schema.tl",
        repo_root / "test/tontester/tests/tl/generated",
    )

    # TL-B codegen
    print("Generating TL-B Python:")
    tlb_schemas = repo_root / "test/tontester/tests/tlb/schemas"
    tlb_out = repo_root / "test/tontester/tests/tlb/generated"
    tlb_out.mkdir(parents=True, exist_ok=True)
    simplify_all = SimplifyConfig.all()
    inline_only = SimplifyConfig(inline_records=True)
    test_schemas: list[tuple[str, SimplifyConfig | None]] = [
        ("basic", None),
        ("cellref", None),
        ("collisions", None),
        ("cond_tuple", None),
        ("generics", None),
        ("inline_records", inline_only),
        ("nat_params", None),
        ("nat_types", None),
        ("output_params", None),
        ("simplify", simplify_all),
        ("special_cells", None),
        ("validation", None),
    ]
    for name, config in test_schemas:
        _ = generate_tlb_python(
            tlb_schemas / f"{name}.tlb",
            tlb_out / f"{name}.py",
            py_module=f"generated.{name}",
            simplify=config,
        )

    # Cross-module test pair: imports_user.tlb imports types from imports_base.tlb.
    base_analyzed, base_manifest = generate_tlb_python(
        tlb_schemas / "imports_base.tlb",
        tlb_out / "imports_base.py",
        py_module="generated.imports_base",
    )
    _ = generate_tlb_python(
        tlb_schemas / "imports_user.tlb",
        tlb_out / "imports_user.py",
        py_module="generated.imports_user",
        imports={"imports_base.tlb": base_analyzed},
        foreign_manifests={base_analyzed.module: base_manifest},
    )

    # Generate hashmap helper (with bit/unary simplifications only)
    hashmap_simplify = SimplifyConfig(
        simplify=frozenset({WellKnownType.BIT, WellKnownType.UNARY, WellKnownType.UNIT})
    )
    hashmap_tlb = repo_root / "test/tontester/src/tlb/hashmap_auto.tlb"
    hashmap_out = repo_root / "test/tontester/src/tlb/hashmap_auto.py"
    _ = generate_tlb_python(
        hashmap_tlb, hashmap_out, py_module="tlb.hashmap_auto", simplify=hashmap_simplify
    )

    # Generate block.tlb (test copy with inline_only for existing e2e tests)
    block_tlb = repo_root / "crypto/block/block.tlb"
    block_out = tlb_out / "block.py"
    _ = generate_tlb_python(block_tlb, block_out, py_module="generated.block", simplify=inline_only)

    # Generate block.tlb (full simplifications for the block module).
    # Only typedefs with hand-written augmentations are entered here; the
    # rest of HashmapAugE typedefs in block.tlb fall through to generic
    # (unsimplified) HashmapAugE handling.
    block_module = Module("block")
    block_aug_classes: dict[tuple[Module, str], str] = {
        (block_module, "OutMsgQueue"): "OutMsgQueueAug",
        (block_module, "DispatchQueue"): "DispatchQueueAug",
        (block_module, "ShardAccounts"): "DepthBalanceAug",
        (block_module, "OldMcBlocksInfo"): "KeyMaxLtAug",
    }
    block_simplify = SimplifyConfig(
        simplify=simplify_all.simplify,
        inline_records=simplify_all.inline_records,
        aug_classes=block_aug_classes,
    )
    block_module_out = repo_root / "test/tontester/src/block/generated.py"
    block_aug_source = repo_root / "test/tontester/src/block/_aug_source.py"
    _ = generate_tlb_python(
        block_tlb,
        block_module_out,
        py_module="block.generated",
        simplify=block_simplify,
        aug_source_path=block_aug_source,
    )
