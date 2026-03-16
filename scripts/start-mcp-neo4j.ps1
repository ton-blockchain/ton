$ErrorActionPreference = 'Stop'

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $repoRoot '.env'

if (Test-Path $envFile) {
  Get-Content $envFile | ForEach-Object {
    if ($_ -match '^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*)\s*$') {
      $name = $matches[1]
      $value = $matches[2].Trim('"').Trim("'")
      if (-not [string]::IsNullOrWhiteSpace($name) -and -not (Test-Path "Env:$name")) {
        Set-Item -Path "Env:$name" -Value $value
      }
    }
  }
}

if (-not $env:NEO4J_URI -and $env:AURA_NEO4J_URI) {
  $env:NEO4J_URI = $env:AURA_NEO4J_URI
}
if (-not $env:NEO4J_USER -and $env:AURA_NEO4J_USER) {
  $env:NEO4J_USER = $env:AURA_NEO4J_USER
}
if (-not $env:NEO4J_PASSWORD -and $env:AURA_NEO4J_PASSWORD) {
  $env:NEO4J_PASSWORD = $env:AURA_NEO4J_PASSWORD
}

if (-not $env:NEO4J_URI -or -not $env:NEO4J_PASSWORD) {
  Write-Error 'Missing NEO4J_URI/NEO4J_PASSWORD (or AURA_NEO4J_URI/AURA_NEO4J_PASSWORD) in .env'
}

$db = if ($env:NEO4J_DATABASE) { $env:NEO4J_DATABASE } elseif ($env:AURA_NEO4J_DATABASE) { $env:AURA_NEO4J_DATABASE } else { 'neo4j' }
uvx mcp-server-neo4j --uri $env:NEO4J_URI --username $env:NEO4J_USER --password $env:NEO4J_PASSWORD --database $db
