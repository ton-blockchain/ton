from multiprocessing import Process
from pathlib import Path
from typing import final, cast

from .parser import ParserLogs
from .visualizer import DashApp


def target(parser: ParserLogs, debug: bool, host: str, port: int):
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
                "parser": ParserLogs(self._logs_path),
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
    _ = parser.add_argument(
        "--logs", nargs="+", required=True, help="Paths to log files or directory"
    )
    _ = parser.add_argument(
        "--host", default="127.0.0.1", help="Host to bind to (default: 127.0.0.1)"
    )
    _ = parser.add_argument(
        "--port", type=int, default=8050, help="Port to bind to (default: 8050)"
    )

    args = parser.parse_args()
    logs = cast(list[str], args.logs)
    host = cast(str, args.host)
    port = cast(int, args.port)

    log_paths = [Path(log) for log in logs]
    if len(log_paths) == 1 and log_paths[0].is_dir():
        log_paths = [p for p in log_paths[0].iterdir()]
    app = DashApp(ParserLogs(log_paths))

    app.run(debug=True, host=host, port=port)


if __name__ == "__main__":
    _main()
