import asyncio
import sys
from pathlib import Path

from .daemon import DashboardDaemon
from .ipc import IPCClient


def get_default_paths() -> tuple[Path, Path, Path]:
    integration_dir = Path(__file__).parent.parent.parent.parent / "integration"
    dashboard_dir = integration_dir / ".dashboard"
    config_path = dashboard_dir / "config.yaml"
    socket_path = dashboard_dir / "daemon.sock"
    frontend_dir = Path(__file__).parent / "frontend"
    return config_path, socket_path, frontend_dir


async def start() -> None:
    config_path, socket_path, frontend_dir = get_default_paths()

    if socket_path.exists():
        try:
            client = IPCClient(socket_path)
            _ = await client.ping()
            print(f"Daemon already running at {socket_path}")
            return
        except Exception:
            socket_path.unlink()

    if not frontend_dir.exists():
        print(f"Error: Frontend directory not found at {frontend_dir}")
        sys.exit(1)

    daemon = DashboardDaemon(config_path, socket_path, frontend_dir)

    print("Starting dashboard daemon...")
    print(f"Config: {config_path}")
    print(f"Socket: {socket_path}")

    try:
        await daemon.start()
    except KeyboardInterrupt:
        print("\nShutting down...")


async def stop() -> None:
    _, socket_path, _ = get_default_paths()

    if not socket_path.exists():
        print("Daemon is not running")
        sys.exit(1)

    print("Stopping daemon...")


async def status() -> None:
    config_path, socket_path, _ = get_default_paths()

    if not socket_path.exists():
        print("Daemon is not running")
        sys.exit(1)

    try:
        client = IPCClient(socket_path)
        _ = await client.ping()
        print("Daemon is running")
        print(f"Config: {config_path}")
        print(f"Socket: {socket_path}")
    except Exception as e:
        print(f"Error connecting to daemon: {e}")
        sys.exit(1)


def main() -> None:
    if len(sys.argv) != 2:
        print("Usage: uv run daemon [start|stop|status]")
        sys.exit(1)

    command = sys.argv[1]

    if command == "start":
        asyncio.run(start())
    elif command == "stop":
        asyncio.run(stop())
    elif command == "status":
        asyncio.run(status())
    else:
        print(f"Unknown command: {command}")
        sys.exit(1)


if __name__ == "__main__":
    main()
