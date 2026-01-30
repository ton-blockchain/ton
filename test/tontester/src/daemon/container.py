"""Container lifecycle management using Podman."""

import asyncio
import json
import logging
import os
import shutil
import subprocess
from abc import ABC, abstractmethod
from pathlib import Path
import sys
from typing import final

from pydantic import BaseModel

logger = logging.getLogger(__name__)


class ContainerConfig(BaseModel):
    """Configuration for a container."""

    name: str
    image: str
    port_mappings: dict[int, int]  # host_port -> container_port
    volumes: dict[Path, str] = {}  # host_path -> container_path
    tmpfs_mounts: list[str] = []  # tmpfs mount points
    environment: dict[str, str] = {}
    command: list[str] | None = None
    run_as_user: bool = True  # Run container with host user's UID/GID


class ContainerInfo(BaseModel):
    """Information about a running container."""

    id: str
    name: str
    status: str
    ports: dict[int, int]


class ContainerController(ABC):
    """Abstract base class for container lifecycle management."""

    def __init__(self, instance_dir: Path):
        self.instance_dir = instance_dir
        self.instance_dir.mkdir(parents=True, exist_ok=True)
        self.container_file = instance_dir / "container.json"
        self._container_id: str | None = None
        self._load_state()

    def _load_state(self) -> None:
        """Load container ID from persistent state file."""
        if self.container_file.exists():
            try:
                data = json.loads(self.container_file.read_text())
                container_id = data.get("container_id")
                if container_id and self._container_exists(container_id):
                    self._container_id = container_id
                else:
                    # Container no longer exists, clean up state file
                    self.container_file.unlink()
                    self._container_id = None
            except Exception:
                self.container_file.unlink()
                self._container_id = None

    def _save_state(self) -> None:
        """Save container ID to persistent state file."""
        if self._container_id:
            self.container_file.write_text(json.dumps({"container_id": self._container_id}))
        elif self.container_file.exists():
            self.container_file.unlink()

    def _run_command(self, command: list[str], check: bool = True) -> str:
        """Run a podman command and return stdout."""
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=check,
        )
        return result.stdout.strip()

    def _container_exists(self, container_id: str) -> bool:
        """Check if a container exists."""
        try:
            self._run_command(["podman", "container", "exists", container_id])
            return True
        except subprocess.CalledProcessError:
            return False

    @property
    def is_running(self) -> bool:
        """Check if the container is currently running."""
        if not self._container_id:
            return False
        try:
            output = self._run_command(
                ["podman", "inspect", "-f", "{{.State.Running}}", self._container_id]
            )
            return output.lower() == "true"
        except subprocess.CalledProcessError:
            return False

    @abstractmethod
    def get_config(self) -> ContainerConfig:
        """Get the container configuration."""
        pass

    async def start(self) -> None:
        """Start the container."""
        if self._container_id and self.is_running:
            logger.info(f"Container {self.get_config().name} is already running")
            return

        if self._container_id and not self.is_running:
            # Container exists but is stopped, start it
            self._run_command(["podman", "start", self._container_id])
            logger.info(f"Started existing container {self.get_config().name}")
            return

        # Create and start new container
        config = self.get_config()
        command = ["podman", "run", "-d", "--name", config.name]

        # Run as host user for proper file permissions (using userns keep-id)
        if config.run_as_user:
            command.extend(["--user", f"{os.getuid()}:{os.getgid()}"])
            command.extend(["--userns", "keep-id"])

        # Add to tontester network for container-to-container communication
        command.extend(["--network", "tontester"])

        # Add port mappings
        for host_port, container_port in config.port_mappings.items():
            command.extend(["-p", f"{host_port}:{container_port}"])

        # Add volume mounts
        for host_path, container_path in config.volumes.items():
            host_path.mkdir(parents=True, exist_ok=True)
            command.extend(["-v", f"{host_path}:{container_path}"])

        # Add tmpfs mounts
        for tmpfs_path in config.tmpfs_mounts:
            command.extend(["--tmpfs", tmpfs_path])

        # Add environment variables
        for key, value in config.environment.items():
            command.extend(["-e", f"{key}={value}"])

        # Add image and command
        command.append(config.image)
        if config.command:
            command.extend(config.command)

        print("command is", " ".join(command))

        self._container_id = self._run_command(command)
        self._save_state()
        logger.info(f"Created and started container {config.name}")

    async def stop(self) -> None:
        """Stop the container."""
        if not self._container_id:
            return

        if self.is_running:
            self._run_command(["podman", "stop", self._container_id])
            logger.info(f"Stopped container {self.get_config().name}")

    async def remove(self) -> None:
        """Stop and remove the container."""
        await self.stop()
        if self._container_id:
            self._run_command(["podman", "rm", self._container_id])
            self._container_id = None
            self._save_state()
            logger.info(f"Removed container {self.get_config().name}")

    def get_info(self) -> ContainerInfo | None:
        """Get information about the running container."""
        if not self._container_id:
            return None

        try:
            output = self._run_command(
                [
                    "podman",
                    "inspect",
                    "-f",
                    '{"id":"{{.Id}}","name":"{{.Name}}","status":"{{.State.Status}}"}',
                    self._container_id,
                ]
            )
            data = json.loads(output)
            return ContainerInfo(
                id=data["id"],
                name=data["name"],
                status=data["status"],
                ports=self.get_config().port_mappings,
            )
        except (subprocess.CalledProcessError, json.JSONDecodeError):
            return None


@final
class PrometheusController(ContainerController):
    """Controller for Prometheus container."""

    def __init__(self, instance_dir: Path, port: int = 9090):
        self.port = port
        self.config_dir = instance_dir / "prometheus-config"
        self.data_dir = instance_dir / "prometheus-data"
        super().__init__(instance_dir / "prometheus")

    def get_config(self) -> ContainerConfig:
        """Get Prometheus container configuration."""
        # Create default Prometheus config if it doesn't exist
        self._ensure_config()
        self._ensure_data_dir()

        return ContainerConfig(
            name="tontester-prometheus",
            image="docker.io/prom/prometheus:latest",
            port_mappings={self.port: 9090},
            volumes={
                self.config_dir: "/etc/prometheus",
                self.data_dir: "/prometheus",
            },
            tmpfs_mounts=["/tmp"],  # For queries.active and other temp files
            command=[
                "--config.file=/etc/prometheus/prometheus.yml",
                "--storage.tsdb.path=/prometheus",
                "--storage.tsdb.retention.time=7d",
                "--web.console.libraries=/usr/share/prometheus/console_libraries",
                "--web.console.templates=/usr/share/prometheus/consoles",
            ],
        )

    def _ensure_data_dir(self) -> None:
        """Ensure the data directory exists with proper permissions."""
        self.data_dir.mkdir(parents=True, exist_ok=True)

    def _ensure_config(self) -> None:
        """Create default Prometheus configuration."""
        self.config_dir.mkdir(parents=True, exist_ok=True)
        config_file = self.config_dir / "prometheus.yml"

        if not config_file.exists():
            # Default config that scrapes from localhost:8080/metrics
            default_config = """
global:
  scrape_interval: 15s
  evaluation_interval: 15s

scrape_configs:
  - job_name: 'ton-validator'
    static_configs:
      - targets: ['host.containers.internal:8080']
"""
            config_file.write_text(default_config.strip())

    def update_scrape_targets(self, targets: list[str]) -> None:
        """Update the Prometheus scrape targets."""
        config_file = self.config_dir / "prometheus.yml"
        config = {
            "global": {
                "scrape_interval": "15s",
                "evaluation_interval": "15s",
            },
            "scrape_configs": [
                {
                    "job_name": "ton-validator",
                    "static_configs": [{"targets": targets}],
                }
            ],
        }
        import yaml

        config_file.write_text(yaml.dump(config, default_flow_style=False))

        # Reload Prometheus config if running
        if self.is_running and self._container_id:
            try:
                self._run_command(["podman", "kill", "-s", "HUP", self._container_id])
            except subprocess.CalledProcessError:
                pass  # Ignore errors on reload


@final
class GrafanaController(ContainerController):
    """Controller for Grafana container."""

    def __init__(self, instance_dir: Path, port: int = 3000):
        self.port = port
        self.data_dir = instance_dir / "grafana-data"
        self.provisioning_dir = instance_dir / "grafana-provisioning"
        super().__init__(instance_dir / "grafana")

    def get_config(self) -> ContainerConfig:
        """Get Grafana container configuration."""
        self._ensure_provisioning()
        self._ensure_data_dir()

        return ContainerConfig(
            name="tontester-grafana",
            image="docker.io/grafana/grafana:latest",
            port_mappings={self.port: 3000},
            volumes={
                self.data_dir: "/var/lib/grafana",
                self.provisioning_dir: "/etc/grafana/provisioning",
            },
            tmpfs_mounts=["/tmp"],  # For temporary files
            environment={
                "GF_SECURITY_ADMIN_PASSWORD": "admin",
                "GF_USERS_ALLOW_SIGN_UP": "false",
                "GF_AUTH_ANONYMOUS_ENABLED": "true",
                "GF_AUTH_ANONYMOUS_ORG_ROLE": "Viewer",
            },
        )

    def _ensure_data_dir(self) -> None:
        """Ensure the data directory exists with proper permissions."""
        self.data_dir.mkdir(parents=True, exist_ok=True)

    def _ensure_provisioning(self) -> None:
        """Create Grafana provisioning configuration."""
        # Create datasource provisioning
        datasources_dir = self.provisioning_dir / "datasources"
        datasources_dir.mkdir(parents=True, exist_ok=True)

        datasource_file = datasources_dir / "prometheus.yml"
        if not datasource_file.exists():
            datasource_config = """
apiVersion: 1

datasources:
  - name: Prometheus
    type: prometheus
    access: proxy
    url: http://tontester-prometheus:9090
    isDefault: true
    editable: true
"""
            datasource_file.write_text(datasource_config.strip())

        # Create dashboards provisioning
        dashboards_dir = self.provisioning_dir / "dashboards"
        dashboards_dir.mkdir(parents=True, exist_ok=True)

        dashboard_file = dashboards_dir / "dashboard.yml"
        if not dashboard_file.exists():
            dashboard_config = """
apiVersion: 1

providers:
  - name: 'default'
    orgId: 1
    folder: ''
    type: file
    disableDeletion: false
    updateIntervalSeconds: 10
    options:
      path: /etc/grafana/provisioning/dashboards/definitions
"""
            dashboard_file.write_text(dashboard_config.strip())

        # Create directory for dashboard definitions
        (dashboards_dir / "definitions").mkdir(parents=True, exist_ok=True)


@final
class ServiceManager:
    """Manages all containers for the daemon."""

    def __init__(self, instance_dir: Path, prometheus_port: int, grafana_port: int):
        self.instance_dir = instance_dir
        self.prometheus = PrometheusController(instance_dir, prometheus_port)
        self.grafana = GrafanaController(instance_dir, grafana_port)
        self.systemd_unit = "tontester-daemon.service"
        self.systemd_slice = "tontester-daemon.slice"
        self.network_name = "tontester"

    def _ensure_network(self) -> None:
        """Ensure the podman network exists for container-to-container communication."""
        try:
            # Check if network already exists
            subprocess.run(
                ["podman", "network", "exists", self.network_name],
                check=True,
                capture_output=True,
            )
        except subprocess.CalledProcessError:
            # Network doesn't exist, create it
            subprocess.run(
                ["podman", "network", "create", self.network_name],
                check=True,
                capture_output=True,
            )
            logger.info(f"Created podman network: {self.network_name}")

    async def start_all(self) -> None:
        """Start all services within a systemd scope for proper cleanup."""
        # Check if podman is available
        if shutil.which("podman") is None:
            raise RuntimeError("Podman is not installed or not in PATH")

        # Ensure network exists
        self._ensure_network()

        # Start containers in parallel
        await asyncio.gather(
            self.prometheus.start(),
            self.grafana.start(),
        )

        logger.info(f"Prometheus available at: http://localhost:{self.prometheus.port}")
        logger.info(f"Grafana available at: http://localhost:{self.grafana.port}")

    async def stop_all(self) -> None:
        """Stop all services."""
        await asyncio.gather(
            self.prometheus.stop(),
            self.grafana.stop(),
        )

    async def cleanup(self) -> None:
        """Remove all containers and clean up data."""
        await asyncio.gather(
            self.prometheus.remove(),
            self.grafana.remove(),
        )

        # Try to remove the network (will fail if other containers are using it)
        try:
            subprocess.run(
                ["podman", "network", "rm", self.network_name],
                check=False,
                capture_output=True,
            )
        except subprocess.CalledProcessError:
            pass  # Network might be in use by other containers

    def start_with_systemd(self) -> None:
        """Start the daemon in a systemd scope for proper cleanup.

        This ensures that all child processes (containers) are properly
        cleaned up when the daemon exits.
        """
        import os
        import sys

        # Check if already running under systemd-run
        if os.environ.get("INVOCATION_ID"):
            # Already running under systemd
            return

        # Re-exec ourselves under systemd-run
        args = [
            "systemd-run",
            "--user",
            f"--unit={self.systemd_unit}",
            f"--slice={self.systemd_slice}",
            "-p",
            "Type=simple",
            "-p",
            "Delegate=yes",
            sys.executable,
        ] + sys.argv

        os.execvp("systemd-run", args)
