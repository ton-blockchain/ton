"""Source code builder with indentation tracking."""

from collections.abc import Generator
from contextlib import contextmanager


class SourceBuilder:
    """Builds source code text with managed indentation.

    Usage:
        sb = SourceBuilder()
        sb.line("class Foo:")
        with sb.block():
            sb.line("x: int = 0")
            sb.blank()
            sb.line("def method(self):")
            with sb.block():
                sb.line("return self.x")
        print(sb.build())
    """

    _lines: list[str]
    _indent: int
    _indent_str: str

    def __init__(self, indent_str: str = "    ") -> None:
        self._lines = []
        self._indent = 0
        self._indent_str = indent_str

    def line(self, text: str = "") -> None:
        """Add a line at the current indentation level."""
        if text:
            self._lines.append(self._indent_str * self._indent + text)
        else:
            self._lines.append("")

    def blank(self) -> None:
        """Add a blank line."""
        self._lines.append("")

    @contextmanager
    def block(self) -> Generator[None]:
        """Context manager that indents the enclosed lines."""
        self._indent += 1
        try:
            yield
        finally:
            self._indent -= 1

    def build(self) -> str:
        """Return the accumulated source code as a string."""
        return "\n".join(self._lines) + "\n"
