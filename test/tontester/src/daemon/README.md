# TON Network Monitoring Tools: Analysis and Architecture

## Current State Analysis (Initial Research)

### 1. tontester (Local Network Orchestration)

**Location**: `test/tontester/src/tontester/`

**Purpose**: Runs local TON networks for integration testing

**Key Components**:
- `network.py`: Main orchestration (Network class)
  - Creates DHT nodes and full validator nodes
  - Generates zerostate with Fift scripts
  - Manages node lifecycle via subprocesses
  - Configures session-logs output: `--session-logs {node_dir}/session-logs`
- `zerostate.py`: Genesis state generation
- `key.py`: Ed25519 key generation
- Session-stats format: JSON Lines (`.jsonl` files)
- Output location: `node<N>/session-logs/*.jsonl`

**Communication**:
- Tonlib client (liteserver port) for blockchain queries
- Engine console (ADNL protocol) for admin operations
- TL protocol for type-safe messaging

### 2. stat-visualizer (Performance Metrics Dashboard)

**Location**: `../stat-visualizer/`

**Architecture**:
- **Backend**: Flask REST API + SQLite
- **Frontend**: React SPA (pre-built, sources not in repo)
- **Database**: `session-stats-accel.sqlite`
  - Tables: `stats`, `data`, `processed_blocks_applied`, `processed_blocks_from`
  - Metrics aggregated to 60-second windows
- **Data Pipeline**: `updater.py` → parses session-stats JSON Lines → SQLite

**Endpoints**:
- `GET /api/stats` - Time-series data for multiple stats
- `GET /api/stats_single` - Time-series data for single stat (both workchains)
- `GET /api/chart-config` - Chart configuration metadata
- `GET /api/test_runs` - Optional test run intervals

**Metrics Tracked** (50+ charts):
- Consensus phases: collate/validate timelines (stacked area charts)
- Performance: blocks/sec, transactions/sec
- Block metrics: size, collated data size, transactions per block
- Queue metrics: message queue size, cleaned, processed, skipped
- Timing breakdowns: collate/validate work time (real & CPU)
- Storage stat cache: hits, misses, cells

**Current Issues**:
- Manual `updater.py` execution required
- No live updates (post-factum only)
- Single SQLite file (not designed for multi-run)

### 3. consensus_explorer (Slot-level Consensus Visualization)

**Location**: `test/tontester/src/consensus_explorer/`

**Architecture**: Dash/Plotly Python application (monolithic)

**Data Source**: Parses **text logs** with regex (NOT session-stats)
- Looks for patterns: `StatsTargetReached`, `Published event`, vote broadcasts
- Tracks per-validator events at slot level

**Purpose**:
- Slot-by-slot timeline visualization
- Per-validator event tracking (collate started/finished, validate started/finished)
- Phase tracking: collate, notarize (voting), finalize (voting)
- Vote weight threshold calculations

**Current Issues**:
- Separate from session-stats infrastructure
- Different abstraction level than stat-visualizer

### 4. slowest_blocks_tool (Block Processing Analysis)

**Location**: `../slowest_blocks_tool/`

**Architecture**: CLI tool with 3-stage pipeline

**Pipeline**:
1. `collect_logs.py`: SSH to remote servers, grep for `Broadcast_benchmark` marker
   - Remote paths: `/var/log/devnet/devnet_YYYY-MM-DD.log`
   - Outputs: `logs/<experiment_name>/benchmark.log`, `info.json`
2. `parse_logs.py`: Parse log lines into structured JSON
   - Extracts: timestamp, node_id, block_id, operation type, duration, sizes
   - Outputs: `logs/<experiment_name>/records.json`
3. `analyse_lifecycle.py`: Analysis and visualization
   - Groups by block lifecycle
   - Identifies slowest blocks
   - Type signature analysis

**Tracked Operations**:
- `compress_candidate_data`, `compress_block_full`, `compress_block_broadcast`
- `deserialize_candidate_data`, `deserialize_block_full`
- Compression types: `compressed`, `compressedV2_*`, `none`

**Current Issues**:
- SSH-based collection (hardcoded `devnet-log` host)
- Parses text logs (not session-stats)
- No integration with other tools

### 5. ton-dashboard (Test Run Tracker)

**Location**: `../ton-dashboard/`

**Architecture**:
- **Backend**: Node.js Express
- **Frontend**: Plain HTML/CSS/JavaScript (no framework)

**Data Model**: Parses `.txt` files with test run metadata
- Format: `operation: start/stop/drop` sections
- Extracts: description, git branch/commit, node count, duration, bps/spb

**Endpoints**:
- `GET /api/tests` - List all test runs
- `GET /api/tests/:filename` - Get single test
- `GET /api/network/:id` - Network config (`.net` file)
- `GET /api/network-quality/:id` - Network quality (`.slow.summary` file)
- `GET /api/gremlin-logs/:id` - Gremlin logs
- `PATCH /api/tests/:id/description` - Update description
- `DELETE /api/tests/:id` - Delete test files

**UI Features**:
- Paginated table with test runs
- User filter dropdown
- Expandable details view
- Link to stat-visualizer (port 8000)
- Editable descriptions (double-click)

---

## Proposed Architecture

### Design Goals

1. **Single source of truth**: Session-stats for all tools
2. **Environment abstraction**: Same code for local (tontester) and dev cluster
3. **Daemon-based**: Long-running process for local runs, auto-reloading config
4. **Unix socket IPC**: Simple run registration and lifecycle tracking

---

## Operational Modes

### Mode 1: Local (tontester)

**Use Case**: Developer running integration tests locally

**Daemon Lifecycle**:
1. Test harness creates config: `test/integration/.dashboard/config.yaml`
2. Daemon auto-starts (or already running)
3. Daemon watches config for changes (host/port updates)
4. If config deleted → daemon shuts down gracefully

**Run Lifecycle**:
1. Test starts → connects to daemon via Unix socket
2. Sends hello message: `{"run_id": "...", "metadata": {...}}`
3. Daemon registers run, responds with OK
4. Connection stays open for duration of test
5. Test ends → closes connection
6. Daemon detects closed connection → marks run as "completed"

**Storage**: In-memory SQLite (`:memory:`)
- Tables: `runs`, `metrics`
- Data retention: configurable (e.g., keep last 50 runs or 7 days)
- Optional: periodic export to Parquet for persistence

**Config Format** (`test/integration/.dashboard/config.yaml`):
```yaml
host: 127.0.0.1
port: 8080
```

**File Structure**:
```
test/integration/.dashboard/
├── config.yaml          # Written by test harness
├── daemon.sock          # Unix socket for IPC
└── (optional) snapshots/  # Parquet exports
```

**Features**:
- Multi-run dashboard with run selector
- Automatic cleanup of old runs
- Live updates during test execution
- No manual intervention required

---

### Mode 2: Devnet (24-server cluster)

**Use Case**: Continuous testing on development cluster

**Daemon Lifecycle**:
1. Deployed as systemd service on monitoring server
2. Reads config from: `/etc/ton-dashboard/config.yaml`
3. Config changes require daemon restart (systemctl reload)
4. Runs indefinitely, survives server reboots

**Data Collection**:
- **Option A**: SSH-based collection from validator nodes
  - Daemon SSHs to each node, tails session-stats files
  - Requires SSH keys and network access
- **Option B**: Push-based collection
  - Validator nodes push metrics to daemon via HTTP API
  - Simpler network topology, better security

**Storage**: ClickHouse or PostgreSQL/TimescaleDB
- Persistent storage across daemon restarts
- Efficient time-series queries
- Configurable retention policy (e.g., 90 days)
- Support for high write throughput (24 nodes × continuous metrics)

**Config Format** (`/etc/ton-dashboard/config.yaml`):
```yaml
mode: devnet
host: 0.0.0.0
port: 8080

storage:
  type: clickhouse
  url: "clickhouse://localhost:9000/ton_stats"
  retention_days: 90

sources:
  type: ssh  # or 'push'
  nodes:
    - host: devnet-01
      session_logs_path: /var/log/validator/session-logs/
    - host: devnet-02
      session_logs_path: /var/log/validator/session-logs/
    # ... repeat for all 24 nodes
  ssh_key: /root/.ssh/devnet_key
  poll_interval: 1.0  # seconds
```

**Run Management**:
- Runs identified by git commit + timestamp
- Manual run registration via API or CLI
- Runs marked as "completed" via API call or time-based heuristics
- Dashboard shows all runs with filtering/search

**Deployment**:
```bash
# On monitoring server
cd ton-monitoring
sudo systemctl start ton-dashboard
sudo systemctl enable ton-dashboard

# Access dashboard
https://devnet-monitor.internal:8080
```

**Features**:
- Historical run comparison
- Long-term trend analysis
- Integration with CI/CD pipeline
- Alerting on performance regressions

---

### Mode 3: Testnet (production monitoring)

**Use Case**: Live monitoring of testnet validator performance

**Daemon Lifecycle**:
1. Deployed as systemd service
2. Single in-progress run (no multi-run view)
3. Continuous monitoring, no concept of "run completion"
4. Config changes require daemon restart

**Data Collection**:
- Same as devnet (SSH or push-based)
- Higher security requirements (read-only access, limited network exposure)
- May connect to subset of validators (not all testnet validators)

**Storage**: ClickHouse (reuse existing testnet infrastructure)
- Production-grade retention policy
- Archival to cold storage for historical analysis
- High availability and replication

**Config Format** (`/etc/ton-dashboard/config.yaml`):
```yaml
mode: testnet
host: 0.0.0.0
port: 8080
single_run: true  # Only show current in-progress run

storage:
  type: clickhouse
  url: "clickhouse://testnet-ch:9000/ton_stats"
  retention_days: 365

sources:
  type: ssh
  nodes:
    - host: testnet-val-01
      session_logs_path: /var/log/validator/session-logs/
    # ... selected testnet validators
  ssh_key: /root/.ssh/testnet_key_readonly
  poll_interval: 5.0
```

**Features**:
- Real-time performance dashboard
- No historical run selection (single live view)
- Focus on current network state
- Export API for external monitoring tools
- Read-only access (no run management)

---

## Daemon Architecture

### Components

#### 1. ConfigWatcher (`daemon/config.py`)
- Uses `watchfiles` library (Rust-based, fast)
- Watches `.dashboard/` directory for config changes
- Detects: file modified, deleted
- Callback: `on_config_change(config: DaemonConfig | None)`

#### 2. IPCServer (`daemon/ipc.py`)
- Unix socket server at `.dashboard/daemon.sock`
- **Protocol**:
  - Client connects → sends JSON hello with run info
  - Server responds with OK
  - Connection held open until client disconnects
  - On disconnect → mark run as completed
  - Special case: `{"command": "ping"}` for health checks

#### 3. StorageBackend (`daemon/storage.py`)
Abstract interface with implementations:
- **SQLiteStorage** (`daemon/sqlite_storage.py`): In-memory for local mode
- **ClickHouseStorage** (TODO): Persistent for devnet/testnet mode

**Schema**:
- `runs`: run_id (PK), start_time, end_time, status, metadata (JSON), node_count
- `metrics`: run_id (FK), node_id, metric_name, timestamp, workchain, value, tags (JSON)
- Indexes: (run_id, timestamp), (run_id, metric_name)

#### 4. FastAPI Backend (`daemon/api.py`)
- Endpoints compatible with ton-dashboard frontend:
  - `GET /api/tests` → list runs (maps runs to frontend format)
  - `GET /api/tests/:run_id` → get run metadata
  - `GET /health` → health check
- Serves static frontend files via `StaticFiles`

#### 5. DashboardDaemon (`daemon/daemon.py`)
- Orchestrates all components
- **Config change handling**:
  - `None` → shutdown daemon
  - Changed → restart HTTP server with new host/port
- HTTP server: Uvicorn with dynamic config
- Signal handlers: SIGTERM, SIGINT → graceful shutdown

#### 6. CLI (`daemon/cli.py`, `daemon/__main__.py`)
- Commands: `start`, `stop`, `status`
- Usage: `python -m daemon start`
- No external dependencies (plain Python)

### Config Reloading Mechanics

```python
# ConfigWatcher monitors config file
async for changes in awatch(config_dir, stop_event=stop_event):
    for change_type, changed_path in changes:
        if Path(changed_path).name == "config.yaml":
            if change_type == Change.deleted:
                # Config deleted → shutdown daemon
                on_config_change(None)
            elif change_type in (Change.added, Change.modified):
                new_config = load_config()
                if new_config != last_config:
                    # Config changed → reload
                    on_config_change(new_config)

# Daemon handles config changes
def _on_config_change(new_config: DaemonConfig | None):
    if new_config is None:
        # Shutdown both HTTP server and daemon
        asyncio.create_task(_stop_http_server())
        asyncio.create_task(_shutdown())
    elif new_config != _current_config:
        # Restart HTTP server with new config
        asyncio.create_task(_restart_http_server(new_config))
```

### Stopping Mechanics for Local Runs

#### 1. Graceful Shutdown Flow
```
Config deleted or SIGTERM/SIGINT received
→ Set shutdown_event
→ Stop ConfigWatcher (stop_event.set())
→ Stop HTTP server (uvicorn_server.should_exit = True)
→ Stop IPC server (close Unix socket)
→ Close SQLite connection
→ Exit
```

#### 2. Run Completion Detection
```python
# IPC server holds connection open
async def handle_client(reader, writer):
    hello = await reader.read()  # Get run info
    await storage.register_run(run_id, metadata)
    writer.write(b"OK")

    # Wait for client to disconnect
    await reader.read()  # Blocks until EOF

    # Client disconnected → run completed
    await storage.update_run_status(run_id, "completed")
```

#### 3. Test Integration
```python
# In tontester Network class
async def __aenter__(self):
    ipc_client = IPCClient(socket_path)
    await ipc_client.connect_and_register(run_id, metadata)
    # Connection held in background
    return self

async def __aexit__(self, ...):
    await ipc_client.disconnect()  # Closes connection
    # Daemon automatically marks run as completed
```

---

## Current Implementation State

### Files Created

#### 1. `test/tontester/src/daemon/__init__.py`
```python
__version__ = "0.1.0"
```

#### 2. `test/tontester/src/daemon/storage.py`
Abstract storage interface with dataclasses:
- `RunMetadata`: run_id, start_time, end_time, status, metadata, node_count
- `MetricPoint`: run_id, node_id, metric_name, timestamp, value, workchain, tags
- `StorageBackend` (ABC): register_run, update_run_status, write_metric, list_runs, get_run_metadata, query_metrics

#### 3. `test/tontester/src/daemon/sqlite_storage.py`
SQLite implementation of `StorageBackend`:
- In-memory database (`:memory:`)
- Schema:
  - `runs`: run_id (TEXT PK), start_time (REAL), end_time (REAL), status (TEXT), metadata (TEXT/JSON), node_count (INTEGER)
  - `metrics`: run_id (TEXT FK), node_id (TEXT), metric_name (TEXT), timestamp (REAL), workchain (INTEGER), value (REAL), tags (TEXT/JSON)
- Indexes: idx_metrics_run_time, idx_metrics_name
- Timestamps stored as Unix epochs (REAL)
- JSON serialization for metadata/tags

#### 4. `test/tontester/src/daemon/config.py`
Config watching with `watchfiles`:
- `DaemonConfig`: host (str), port (int)
- `ConfigWatcher`:
  - Watches config directory using `awatch`
  - Detects: file added, modified, deleted
  - Callback on changes: `on_config_change(config | None)`
  - Initial config load before watching
  - Compares configs to detect actual changes

#### 5. `test/tontester/src/daemon/ipc.py`
Unix socket IPC:
- `IPCServer`:
  - Unix socket at specified path
  - Protocol: client connects → hello JSON → server responds → wait for disconnect
  - On disconnect: mark run as completed
  - Handles ping command for health checks
- `IPCClient`:
  - `connect_and_register()`: open connection, send hello, wait for OK
  - `disconnect()`: close connection
  - `ping()`: health check (send ping, get pong, close immediately)

#### 6. `test/tontester/src/daemon/api.py`
FastAPI backend:
- `create_app(storage, frontend_dir)`: factory function
- Endpoints:
  - `GET /api/tests`: list runs (mapped to ton-dashboard format)
  - `GET /api/tests/{run_id}`: get run metadata
  - `GET /health`: health check
- Static file serving: mounts frontend_dir at `/`
- Response format: compatible with ton-dashboard frontend
  - Fields: fileName, fileDateTime, description, gitBranch, gitCommitId, nodes, startDateTime, startTimeISO, endTimeISO

#### 7. `test/tontester/src/daemon/daemon.py`
Main daemon orchestrator:
- `DashboardDaemon`:
  - Initializes: SQLiteStorage, IPCServer, ConfigWatcher
  - Config change handler:
    - `None` → shutdown daemon
    - Changed → restart HTTP server (Uvicorn)
  - HTTP server management:
    - Start: create Uvicorn server with config
    - Stop: set should_exit, await task
    - Restart: stop + start
  - Signal handlers: SIGTERM, SIGINT → graceful shutdown
  - Shutdown flow: stop config watcher → stop HTTP → stop IPC → close storage

#### 8. `test/tontester/src/daemon/cli.py`
CLI interface (plain Python, no Click):
- `get_default_paths()`: returns (config_path, socket_path, frontend_dir)
  - Config: `test/integration/.dashboard/config.yaml`
  - Socket: `test/integration/.dashboard/daemon.sock`
  - Frontend: `test/tontester/src/daemon/frontend/`
- Commands:
  - `start`: check if already running (ping), start daemon
  - `stop`: placeholder (TODO: send signal to daemon)
  - `status`: check if running, print config paths
- Usage: `python -m daemon start`

#### 9. `test/tontester/src/daemon/__main__.py`
```python
from .cli import main

if __name__ == "__main__":
    main()
```

#### 10. `test/tontester/src/daemon/frontend/`
Copied from `ton-dashboard/frontend/`:
- `index.html`: main page structure
- `app.js`: frontend logic (27KB)
- `styles.css`: styling (11KB)
- `nginx.conf`, `Dockerfile`, `.htpasswd`: deployment files

### Type Safety

All code written with proper type hints for basedpyright strict mode:
- No `# type: ignore` directives
- Explicit types for function parameters and return values
- Union types using `|` syntax (Python 3.10+)
- `dict[str, Any]` for JSON-like structures
- Optional types: `Type | None`

---

## Current Status

### ✅ Completed and Tested

#### Core Infrastructure
1. **SQLite Storage Backend**: In-memory database with runs and metrics tables
2. **Config Live-Reloading**: Daemon watches config file and reloads on changes
   - ✅ Tested: Changed port 8080 → 8081, server restarted automatically
3. **Daemon Shutdown**: Graceful shutdown when config is deleted
   - ✅ Tested: Deleted config.yaml, daemon stopped cleanly
4. **IPC Protocol**: Unix socket with ping support and run registration
   - ✅ Tested: Ping command works for health checks
5. **FastAPI Backend**: HTTP API compatible with ton-dashboard frontend
   - ✅ Tested: `/health` and `/api/tests` endpoints working
6. **CLI Interface**: Commands for start/stop/status
   - ✅ Tested: `uv run daemon start`, `uv run daemon status`

#### Dependencies
Added to `pyproject.toml`:
- ✅ fastapi
- ✅ uvicorn
- ✅ pyyaml
- ✅ watchfiles

#### Testing Results
```bash
# Start daemon
$ uv run daemon start
Starting dashboard daemon...
Config: test/integration/.dashboard/config.yaml
Socket: test/integration/.dashboard/daemon.sock
INFO: Uvicorn running on http://127.0.0.1:8080

# Check status
$ uv run daemon status
Daemon is running
Config: test/integration/.dashboard/config.yaml
Socket: test/integration/.dashboard/daemon.sock

# Test API
$ curl http://127.0.0.1:8080/health
{"status":"ok"}

$ curl http://127.0.0.1:8080/api/tests
[]

# Config reload (change port 8080 → 8081)
# Daemon automatically restarts on new port

# Config deletion
# Daemon gracefully shuts down
```

---

## Remaining Work

### High Priority (for local mode)

#### 1. Session-stats Ingestion
Add a `StatsCollector` component to watch and parse session-stats files:
- Watch `node<N>/session-logs/*.jsonl` files
- Parse JSON Lines format (TL-serialized objects)
- Extract metrics and write to storage
- Handle new files appearing during test run

**Implementation**:
```python
class StatsCollector:
    def __init__(self, session_logs_dirs: list[Path], storage: StorageBackend):
        pass

    async def watch_and_collect(self, run_id: str):
        # Use watchfiles to monitor session-logs directories
        # Parse validatorStats.* TL objects
        # Extract metrics and store via storage.write_metric()
        pass
```

#### 2. Tontester Integration
Modify `tontester/network.py` to register runs with daemon:
- Add `enable_dashboard: bool` parameter to Network class
- In `__aenter__`: connect to daemon via IPC, register run
- In `__aexit__`: disconnect from daemon (marks run completed)
- Pass session-logs paths to daemon for collection

**Implementation**:
```python
class Network:
    async def __aenter__(self):
        if self.enable_dashboard:
            from daemon.ipc import IPCClient
            self.ipc_client = IPCClient(socket_path)
            await self.ipc_client.connect_and_register(
                run_id=self.run_id,
                metadata={
                    "node_count": len(self.nodes),
                    "config": self.config.to_dict(),
                    "session_logs_dirs": [
                        str(node.directory / "session-logs")
                        for node in self.nodes
                    ]
                }
            )

    async def __aexit__(self, ...):
        if self.ipc_client:
            await self.ipc_client.disconnect()
```

#### 3. Frontend Adaptation
The current ton-dashboard frontend expects `.txt` file format. Adapt for run-based model:
- Update table to show runs from database instead of file listings
- Add "Status" column (running/completed)
- Show live indicator for in-progress runs
- Link to stat-visualizer with run's time range

**Option A**: Minimal changes to existing frontend
**Option B**: Replace with new React/Vue dashboard

#### 4. Basic Metrics Visualization
Add endpoints for querying metrics:
- `GET /api/runs/{run_id}/metrics?names=...&start=...&end=...`
- Integrate with stat-visualizer charts or simple Plotly charts

### Medium Priority

#### 5. Storage Persistence
Add optional SQLite file persistence for local mode:
- Parameter: `storage.db_path` in config (default: `:memory:`)
- Periodic export to Parquet for long-term storage
- Retention policy: keep last N runs or last N days

#### 6. Better Error Handling
- Connection errors when daemon not running
- Invalid config file format
- Session-stats parsing errors
- Storage errors

#### 7. Logging
Add structured logging:
- Log level configuration
- Separate log files for daemon vs HTTP requests
- Run-specific log context

### Future (for devnet/testnet modes)

#### 8. ClickHouseStorage Backend
Implement persistent storage for cluster deployments:
- Schema migration from SQLite
- Connection pooling
- Batch inserts for performance
- Retention policies via TTL

#### 9. SSH-based Collection
For devnet/testnet without shared filesystem:
- SSH to each validator node
- Tail session-stats files remotely
- Stream over SSH connection
- Handle disconnects and reconnections

#### 10. Push-based Collection API
Alternative to SSH for better security:
- `POST /api/runs/{run_id}/metrics` endpoint
- Validator nodes push metrics to daemon
- Authentication with API keys or mTLS
- Rate limiting and validation

#### 11. Run Management API
For manual control in devnet/testnet:
- `POST /api/runs` - manually create run
- `PATCH /api/runs/{run_id}` - update status/metadata
- `DELETE /api/runs/{run_id}` - delete run
- Authentication and authorization

#### 12. Deployment
- systemd service file
- Docker container
- Nginx reverse proxy config
- TLS certificate setup
- Monitoring and alerting integration

---

## Next Steps (Immediate)

1. **Implement StatsCollector**: Watch and parse session-logs files
2. **Integrate with tontester**: Add IPCClient to Network class
3. **Test end-to-end**: Start daemon → run test → verify data collection → view in dashboard
4. **Document usage**: Add examples to tontester documentation
5. **Add basic metrics API**: Query endpoints for time-series data
