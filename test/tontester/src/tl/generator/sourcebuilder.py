import typing
from typing import override


class SourceBuilder:
    """This class should be used to build .py source files"""

    def __init__(self, out_stream: typing.TextIO, indent_size: int = 4):
        self.current_indent: int = 0
        self.on_new_line: bool = False
        self.indent_size: int = indent_size
        self.out_stream: typing.TextIO = out_stream

        # Was a new line added automatically before? If so, avoid it
        self.auto_added_line: bool = False

    def indent(self):
        """Indents the current source code line
        by the current indentation level
        """
        self.write(" " * (self.current_indent * self.indent_size))

    def write(self, string: str, *args: str | tuple[str], **kwargs: dict[str, str]):
        """Writes a string into the source code,
        applying indentation if required
        """
        if self.on_new_line:
            self.on_new_line = False  # We're not on a new line anymore
            # If the string was not empty, indent; Else probably a new line
            if string.strip():
                self.indent()

        if args or kwargs:
            _ = self.out_stream.write(string.format(*args, **kwargs))
        else:
            _ = self.out_stream.write(string)

    def writeln(self, string: str = "", *args: str | tuple[str], **kwargs: dict[str, str]) -> None:
        """Writes a string into the source code _and_ appends a new line,
        applying indentation if required
        """
        self.write(string + "\n", *args, **kwargs)
        self.on_new_line = True

        # If we're writing a block, increment indent for the next time
        if string and string[-1] == ":":
            self.current_indent += 1

        # Clear state after the user adds a new line
        self.auto_added_line = False

    def end_block(self):
        """Ends an indentation block, leaving an empty line afterwards"""
        self.current_indent -= 1

        # If we did not add a new line automatically yet, now it's the time!
        if not self.auto_added_line:
            self.writeln()
            self.auto_added_line = True

    @override
    def __str__(self):
        _ = self.out_stream.seek(0)
        return self.out_stream.read()

    def __enter__(self):
        return self

    def __exit__(self, exc_type: str, exc_val: str, exc_tb: str):
        pass
