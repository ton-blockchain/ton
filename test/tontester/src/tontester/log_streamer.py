import asyncio
import io
import logging
import re
import sys
from dataclasses import dataclass
from typing import final

_IS_TERMINAL_INTERACTIVE = sys.stderr.isatty()


@dataclass
class _LogEntry:
    level: int
    thread_id: int
    timestamp: str
    filename: str
    line_number: int
    label: str | None
    message: bytearray

    def format(self):
        COLORS = {
            0: "\x1b[1;31m",
            1: "\x1b[1;31m",
            2: "\x1b[1;33m",
            3: "\x1b[1;36m",
        }
        message = self.message.decode(errors="replace")
        label = f"[{self.label}]" if self.label is not None else ""

        if message.endswith("\x1b[0m\n"):
            slice_len = 5
        elif message.endswith("\n"):
            slice_len = 1
        else:
            slice_len = 0

        line = f"[{self.level}][t {self.thread_id}][{self.filename}:{self.line_number}]{label} {message[:-slice_len]}"

        if _IS_TERMINAL_INTERACTIVE and self.level in COLORS:
            line = f"{COLORS[self.level]}{line}\x1b[0m"
        return line


@final
class LogStreamer:
    def __init__(self, file: io.BufferedWriter, name: str, stream: asyncio.StreamReader):
        self._file = file
        self._name = name
        self._stream = stream
        self._task = asyncio.create_task(self._stream_log())
        self._logger = logging.getLogger(self._name)
        self._current_entry: _LogEntry | None = None

    async def aclose(self):
        await self._task

    async def _stream_log(self):
        leftover = bytearray()

        while not self._stream.at_eof():
            chunk = await self._stream.read(131072)
            _ = self._file.write(chunk)

            if leftover and not chunk:
                self._process_line(bytes(leftover))
                break

            i = 0
            while True:
                j = chunk.find(b"\n", i)
                if j < 0:
                    break
                if leftover:
                    line = leftover + chunk[i : j + 1]
                    self._process_line(bytes(line))
                    leftover.clear()
                else:
                    self._process_line(chunk[i : j + 1])
                i = j + 1

            leftover += chunk[i:]

        self._flush_entry()

    _log_pattern = re.compile(
        rb"""
            (?:
                \x1b\[\d+;\d+m
            )?
            \[
                \ *(\d+)
            \]
            \[
                t\ +(\d+)
            \]
            \[
                (\d{4}-\d{2}-\d{2}\ \d{2}:\d{2}:\d{2}\.\d{9})
            \]
            \[
                ([^\]]+):(\d+)
            \]
            (?:
                \[
                    ([^\]]+)
                \]
            )?
            \t
            (.*)
        """,
        re.VERBOSE | re.ASCII | re.DOTALL,
    )

    def _process_line(self, line: bytes):
        if self._current_entry is None:
            match = self._log_pattern.match(line)
            if match is None:
                self._log_malformed(line)
                return

            can_be_multiline = line.startswith(b"\x1b")
            if len(match.groups()) == 7:
                level_str, thread_id_str, timestamp, filename, line_number_str, label, message = (
                    match.groups()
                )
                label = label.decode()
            else:
                level_str, thread_id_str, timestamp, filename, line_number_str, message = (
                    match.groups()
                )
                label = None

            try:
                level = int(level_str)
                thread_id = int(thread_id_str)
                line_number = int(line_number_str)

                self._current_entry = _LogEntry(
                    level=level,
                    thread_id=thread_id,
                    timestamp=timestamp.decode(),
                    filename=filename.decode(),
                    line_number=line_number,
                    label=label,
                    message=bytearray(message),
                )
            except Exception:
                self._log_malformed(line)
                return
        else:
            can_be_multiline = True
            self._current_entry.message.extend(line)

        if not can_be_multiline or self._current_entry.message.endswith(b"\x1b[0m\n"):
            self._flush_entry()

    def _flush_entry(self):
        if self._current_entry is not None:
            self._logger.info(self._current_entry.format())
            self._current_entry = None

    def _log_malformed(self, data: bytes):
        if data.endswith(b"\n"):
            data = data[:-1]
        self._logger.info(data.decode(errors="replace"))
