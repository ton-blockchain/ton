from multiprocessing import Process
from pathlib import Path
from typing import cast, final

from .parser import GroupParser, ParserSessionStats
from .visualizer import DashApp


def target(parser: GroupParser, debug: bool, host: str, port: int):
    DashApp(parser).run(debug, host, port)


@final
class ConsensusExplorer:
    def __init__(self, logs_path: list[Path], host: str = "127.0.0.1", port: int = 8050):
        self._logs_path = logs_path
        self._host = host
        self._port = port
        self.__process: Process | None = None

    def run(self):
        self.__process = Process(
            target=target,
            kwargs={
                "parser": ParserSessionStats(self._logs_path, "(.*)"),
                "debug": False,
                "host": self._host,
                "port": self._port,
            },
        )
        self.__process.start()

    def kill(self):
        if self.__process is not None:
            self.__process.terminate()


def _main():
    import argparse

    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    _ = source.add_argument("--logs", nargs="+", help="Paths to log files or directory")
    _ = source.add_argument(
        "--stats-dir", help="Directory with gzipped session stats files (watched for changes)"
    )
    _ = parser.add_argument("--db", help="Path to SQLite database (default: <stats-dir>/index.db)")
    _ = parser.add_argument(
        "--hostname-regex",
        default=r"^(.*)$",
        help="Regex with capture group to extract hostname from filename",
    )
    _ = parser.add_argument(
        "--host", default="127.0.0.1", help="Host to bind to (default: 127.0.0.1)"
    )
    _ = parser.add_argument(
        "--port", type=int, default=8050, help="Port to bind to (default: 8050)"
    )

    args = parser.parse_args()

    host = cast(str, args.host)
    port = cast(int, args.port)

    stats_dir_str = cast(str | None, args.stats_dir)

    hostname_regex = cast(str, args.hostname_regex)

    if stats_dir_str:
        db_path_str = cast(str | None, args.db)

        from .cached_parser import CachedGroupParser
        from .file_index import FileIndex

        stats_dir = Path(stats_dir_str)
        db_path = Path(db_path_str) if db_path_str else stats_dir / "index.db"

        file_index = FileIndex(stats_dir, db_path)
        cached_parser = CachedGroupParser(file_index, hostname_regex)
        file_index.install_callback(cached_parser)

        with file_index:
            app = DashApp(cached_parser)
            app.run(debug=True, host=host, port=port)
    else:
        logs = cast(list[str], args.logs)
        log_paths = [Path(log) for log in logs]
        if len(log_paths) == 1 and log_paths[0].is_dir():
            log_paths = [p for p in log_paths[0].iterdir()]
        app = DashApp(ParserSessionStats(log_paths, hostname_regex))
        app.run(debug=True, host=host, port=port)


if __name__ == "__main__":
    _main()
