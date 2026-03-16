$ErrorActionPreference = 'Stop'

# Load .env
$repoRoot = Split-Path -Parent $PSScriptRoot
Get-Content (Join-Path $repoRoot '.env') | ForEach-Object {
    if ($_ -match '^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$') {
        $n = $matches[1]; $v = $matches[2].Trim('"').Trim("'")
        Set-Item -Path "Env:$n" -Value $v
    }
}
if (-not $env:NEO4J_URI -and $env:AURA_NEO4J_URI) { $env:NEO4J_URI = $env:AURA_NEO4J_URI }
if (-not $env:NEO4J_USER -and $env:AURA_NEO4J_USER) { $env:NEO4J_USER = $env:AURA_NEO4J_USER }
if (-not $env:NEO4J_PASSWORD -and $env:AURA_NEO4J_PASSWORD) { $env:NEO4J_PASSWORD = $env:AURA_NEO4J_PASSWORD }
$db = if ($env:NEO4J_DATABASE) { $env:NEO4J_DATABASE } elseif ($env:AURA_NEO4J_DATABASE) { $env:AURA_NEO4J_DATABASE } else { 'neo4j' }

Write-Host "URI  : $($env:NEO4J_URI)"
Write-Host "USER : $($env:NEO4J_USER)"

$initMsg  = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"copilot-test","version":"0.1"}}}'
$notifMsg = '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName  = 'uvx'
$psi.Arguments = "mcp-server-neo4j --uri $($env:NEO4J_URI) --username $($env:NEO4J_USER) --password $($env:NEO4J_PASSWORD) --database $db"
$psi.UseShellExecute        = $false
$psi.RedirectStandardInput  = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true

$proc = [System.Diagnostics.Process]::Start($psi)

# Send bare JSON messages (one per line — mcp stdio uses NDJSON, not LSP framing)
foreach ($msg in @($initMsg, $notifMsg)) {
    $proc.StandardInput.WriteLine($msg)
    $proc.StandardInput.Flush()
}
$proc.StandardInput.Close()

# ReadToEnd blocks until the process exits (safe since stdin is already closed)
$stdout = $proc.StandardOutput.ReadToEnd()
$stderr = $proc.StandardError.ReadToEnd()
$proc.WaitForExit(5000) | Out-Null

Write-Host "`n=== STDOUT (MCP response) ==="
if ($stdout.Trim()) { Write-Host $stdout } else { Write-Host '(empty)' }

Write-Host "`n=== STDERR (server log) ==="
if ($stderr.Trim()) { Write-Host $stderr } else { Write-Host '(empty)' }
