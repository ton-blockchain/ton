from pathlib import Path


def generate(schema: Path, out_directory: Path):
    from .generator.generators import generate_tlobjects
    from .generator.parsers import parse_tl

    objects = parse_tl(schema)
    filename = schema.name.split(".")[0]
    generate_tlobjects(objects, filename, out_directory)
