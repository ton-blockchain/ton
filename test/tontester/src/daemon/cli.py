import argparse
import asyncio
import logging
import os
import sys
from pathlib import Path
from typing import cast

from .daemon import DashboardDaemon
from .ipc import IPCClient

logger = logging.getLogger(__name__)


def get_default_paths() -> tuple[Path, Path, Path]:
    integration_dir = Path(__file__).parent.parent.parent.parent / "integration"
    dashboard_dir = integration_dir / ".dashboard"
    config_path = dashboard_dir / "config.json"
    socket_path = dashboard_dir / "daemon.sock"
    frontend_dir = Path(__file__).parent / "frontend"
    return config_path, socket_path, frontend_dir


async def start(no_daemonize: bool = False) -> None:
    config_path, socket_path, frontend_dir = get_default_paths()

    if socket_path.exists():
        client = IPCClient(socket_path)
        _ = await client.ping()
        logger.error(f"Daemon is already running at {socket_path}")
        sys.exit(1)

    if not config_path.exists():
        logger.error(f"Config file not found at {config_path}")
        sys.exit(1)

    if not frontend_dir.exists():
        logger.error(f"Frontend directory not found at {frontend_dir}")
        sys.exit(1)

    if not no_daemonize and not os.environ.get("INVOCATION_ID"):
        env: list[str] = []
        for key, value in os.environ.items():
            env.extend(["-E", f"{key}={value}"])

        args = [
            "systemd-run",
            "--user",
            "--unit=tontester-daemon.service",
            "--slice=tontester-daemon.slice",
            "-p",
            "Type=simple",
            "-p",
            "Delegate=yes",
            "-p",
            "RemainAfterExit=no",
            *env,
            sys.executable,
            "-m",
            "daemon",
            "start",
            "--no-daemonize",
        ]

        os.execvp("systemd-run", args)

    daemon = DashboardDaemon(config_path, socket_path, frontend_dir)

    logger.info("Starting dashboard daemon...")
    logger.info(f"Config: {config_path}")
    logger.info(f"Socket: {socket_path}")

    try:
        await daemon.run()
    except KeyboardInterrupt:
        logger.info("Shutting down...")


async def stop() -> None:
    _, socket_path, _ = get_default_paths()

    if not socket_path.exists():
        logger.error("Daemon is not running")
        sys.exit(1)

    logger.info("Stopping daemon...")


async def status() -> None:
    config_path, socket_path, _ = get_default_paths()

    if not socket_path.exists():
        logger.error("Daemon is not running")
        sys.exit(1)

    try:
        client = IPCClient(socket_path)
        _ = await client.ping()
        logger.info("Daemon is running")
        logger.info(f"Config: {config_path}")
        logger.info(f"Socket: {socket_path}")
    except Exception as e:
        logger.error(f"Error connecting to daemon: {e}")
        sys.exit(1)


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    )

    parser = argparse.ArgumentParser(
        prog="daemon",
        description="Dashboard daemon control utility",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    start_parser = subparsers.add_parser("start", help="Start the daemon")
    _ = start_parser.add_argument(
        "--no-daemonize",
        action="store_true",
        help="Run in foreground without daemonizing",
    )

    _ = subparsers.add_parser("stop", help="Stop the daemon")
    _ = subparsers.add_parser("status", help="Check daemon status")

    args = parser.parse_args()
    command = cast(str, args.command)

    if command == "start":
        asyncio.run(start(no_daemonize=cast(bool, args.no_daemonize)))
    elif command == "stop":
        asyncio.run(stop())
    elif command == "status":
        asyncio.run(status())


if __name__ == "__main__":
    main()
