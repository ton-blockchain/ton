#!/usr/bin/env python3
"""Generate the dashboards under metrics/grafana/dashboards.

  python3 -m metrics.grafana.dashgen.build build [--board NAME] [--stdout | --out DIR]
  python3 -m metrics.grafana.dashgen.build check [--board NAME]
  python3 -m metrics.grafana.dashgen.build validate [--board NAME]

check regenerates each board and compares it, key-order-insensitively, with the
file on disk.
"""

import argparse
import difflib
import importlib
import json
import sys
from pathlib import Path

from .lib.core import validate_dashboard

HERE = Path(__file__).resolve().parent
DASHBOARDS = HERE.parent / "dashboards"


def boards():
    """Every module beside this one is a board; each exports build()."""
    return sorted(p.stem.replace("_", "-") for p in HERE.glob("[!_]*.py") if p.stem != "build")


def generate(name):
    module = importlib.import_module(f"{__package__}.{name.replace('-', '_')}")
    return module.build()


def render(dash):
    """The on-disk form: 2-space indent, escaped non-ASCII, no trailing newline."""
    return json.dumps(dash, ensure_ascii=True, indent=2)


def canonical(node):
    """Key-order-insensitive form used for comparison."""
    return json.dumps(node, sort_keys=True, ensure_ascii=True, indent=2)


def label(panel):
    return f"panel {panel.get('id')} {panel.get('title')!r}"


def sections(dash):
    """Comparable pieces of a dashboard: every panel by identity, plus everything else."""
    out = {}

    def add(panels):
        for panel in panels:
            nested = panel.get("panels", [])
            out[label(panel)] = dict(panel, panels=[]) if nested else panel
            add(nested)

    rest = dict(dash)
    add(rest.pop("panels", []))
    out["dashboard body (panels excluded)"] = rest
    return out


def report(name, want, got):
    lost = sections(want)
    made = sections(got)
    for key in sorted(set(lost) | set(made)):
        a, b = lost.get(key, {}), made.get(key, {})
        if canonical(a) == canonical(b):
            continue
        diff = difflib.unified_diff(
            canonical(a).splitlines(), canonical(b).splitlines(),
            fromfile=f"{name}.json :: {key}", tofile=f"generated :: {key}", lineterm="")
        print("\n".join(diff))


def check(names):
    failed = []
    for name in names:
        want = json.loads((DASHBOARDS / f"{name}.json").read_text(encoding="utf-8"))
        validate_dashboard(want)
        got = generate(name)
        if canonical(want) == canonical(got):
            print(f"{name}: match")
            continue
        failed.append(name)
        print(f"{name}: MISMATCH")
        report(name, want, got)
    return 1 if failed else 0


def validate(names):
    for name in names:
        dashboard = generate(name)  # dashboard() validates before dropping internal scope markers
        validate_dashboard(dashboard)
        print(f"{name}: valid")
    return 0


def build(names, out, to_stdout):
    for name in names:
        text = render(generate(name))
        if to_stdout:
            sys.stdout.write(text)
            continue
        path = (out or DASHBOARDS) / f"{name}.json"
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "check", "validate"))
    parser.add_argument("--board", action="append", choices=boards(),
                        help="default: every migrated board")
    parser.add_argument("--out", type=Path, help="build: write into this directory instead")
    parser.add_argument("--stdout", action="store_true", help="build: print instead of writing")
    args = parser.parse_args(argv)
    names = args.board or boards()
    if args.command != "build" and (args.out is not None or args.stdout):
        parser.error("--out and --stdout are only valid with build")
    if args.stdout and len(names) != 1:
        parser.error("--stdout requires exactly one --board")
    if args.command == "check":
        return check(names)
    if args.command == "validate":
        return validate(names)
    return build(names, args.out, args.stdout)


if __name__ == "__main__":
    sys.exit(main())
